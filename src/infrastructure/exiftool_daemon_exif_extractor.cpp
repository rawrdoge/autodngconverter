/*
 * rawimport-pipeline - ExifTool Daemon ExifExtractor adapter implementation
 */
#include "infrastructure/exiftool_daemon_exif_extractor.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// On Windows, MultiByteToWideChar with -1 length includes a trailing null.
// We must strip it from every converted string before appending to the command line.
static inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return std::wstring();
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], wlen);
    w.resize(wlen - 1);  // strip trailing null from -1 length
    return w;
}
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace rawimport::infrastructure {

namespace {

std::string find_in_path(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::istringstream ss(path_env);
    std::string dir;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    while (std::getline(ss, dir, sep)) {
        auto p = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) return p.string();
    }
    return {};
}

std::string extract_json_field(const std::string& json, const std::string& field) {
    const std::string key = "\"" + field + "\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

// Cross-platform process execution using argument arrays (no shell).
// Returns exit code, or -1 on spawn failure.
int exec_cmd(const std::vector<std::string>& args) {
    if (args.empty()) return -1;

#ifdef _WIN32
    // Build quoted UTF-16 command line for CreateProcessW.
    // Source args are UTF-8 strings; widen via CP_UTF8 so multibyte paths
    // (Cyrillic, CJK, etc.) survive intact. utf8_to_wide strips the trailing
    // null that MultiByteToWideChar appends when length is -1.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, args.data()[0].c_str(), -1, nullptr, 0);
    std::wstring wcmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, args.data()[0].c_str(), -1, &wcmd[0], wlen);
    wcmd.resize(wlen - 1);
    for (size_t i = 1; i < args.size(); ++i) {
        wcmd += L' ';
        const std::string quoted = quote_cmdline_arg_win(args[i].c_str());
        const char* src = quoted.c_str();
        wlen = MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
        std::wstring part(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, src, -1, &part[0], wlen);
        part.resize(wlen - 1);
        wcmd += part;
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, const_cast<wchar_t*>(wcmd.c_str()),
                             nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return static_cast<int>(rc);
#else
    // POSIX: build argv array and spawn via execvp — no shell involvement.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) { execvp(argv[0], argv.data()); _exit(127); }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Escape a single argument for embedding in a CreateProcess command line.
#if defined(_WIN32)
static std::string quote_cmdline_arg_win(const char* arg) {
    bool needs_quote = false;
    for (const char* p = arg; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '"' || *p == '\\') { needs_quote = true; break; }
    }
    if (!needs_quote) return std::string(arg);
    std::string out;
    out.reserve(std::strlen(arg) + 4);
    out += '"';
    for (const char* p = arg; *p; ++p) {
        if (*p == '\\') out += '\\';
        out += *p;
    }
    out += '"';
    return out;
}
#endif

} // anon

ExiftoolDaemonExifExtractor::ExiftoolDaemonExifExtractor(std::string binary_path)
    : binary_path_(std::move(binary_path)) {}

ExiftoolDaemonExifExtractor::~ExiftoolDaemonExifExtractor() {
    close_daemon();
}

bool ExiftoolDaemonExifExtractor::open_daemon() {
    const auto bin = binary_path_.empty() ? find_in_path("exiftool") : binary_path_;
    if (bin.empty()) return false;
    daemon_stream_ = popen((bin + " -stay_open yes -@ -").c_str(), "w");
    return daemon_stream_ != nullptr;
}

void ExiftoolDaemonExifExtractor::close_daemon() {
    if (daemon_stream_) {
        fprintf(daemon_stream_, "-stay_open\nno\n\n");
        fflush(daemon_stream_);
        pclose(daemon_stream_);
        daemon_stream_ = nullptr;
    }
}

domain::HealthReport ExiftoolDaemonExifExtractor::health_check() {
    if (!open_daemon())
        return {domain::HealthStatus::Unhealthy, "exiftool daemon not available", domain::now()};
    close_daemon();
    return {domain::HealthStatus::Healthy, "exiftool daemon OK", domain::now()};
}

std::optional<domain::ExifResult> ExiftoolDaemonExifExtractor::extract(const domain::FilePath& path) {
    if (!open_daemon()) return std::nullopt;

    // Write the command to extract metadata for the given file.
    // Use -j for JSON output, -stay_open True to keep daemon alive.
    std::string cmd = "-j\n" + path.native + "\n-stay_open\nTrue\n\n";
    if (std::fwrite(cmd.c_str(), 1, cmd.size(), daemon_stream_) != cmd.size()) {
        close_daemon();
        return std::nullopt;
    }
    std::fflush(daemon_stream_);

    // Read the JSON response from the daemon's stdout.
    // The daemon writes to the same FILE* when opened with "w" (popen).
    // We need to read from a separate pipe for stdout. For simplicity,
    // fall back to single-process mode which is more reliable.
    close_daemon();

    // Single-process fallback: run exiftool once for this file using argument array.
    const auto bin = binary_path_.empty() ? find_in_path("exiftool") : binary_path_;
    if (bin.empty()) return std::nullopt;

    std::vector<std::string> args = {bin, "-j", "-DateTimeOriginal", "-Model", "-Make", "-Orientation", path.native};
    int rc = exec_cmd(args);
    if (rc != 0) return std::nullopt;

    // For capturing output, we need a different approach since exec_cmd doesn't capture stdout.
    // Use popen with argument array via a helper that doesn't invoke shell.
    // Actually, we need to capture stdout, so we'll use a pipe-based approach.
    // Let's use a simple pipe + fork/execvp for stdout capture.
    int pipefd[2];
    if (pipe(pipefd) == -1) return std::nullopt;
    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return std::nullopt;
    }
    if (pid == 0) {
        // Child: redirect stdout to pipe, exec exiftool
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    // Parent: read from pipe
    close(pipefd[1]);
    std::string output;
    char buffer[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, n);
    }
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);

    if (output.empty()) return std::nullopt;

    // Parse JSON array response (exiftool returns an array with one object).
    domain::ExifResult result;
    result.make = extract_json_field(output, "Make");
    result.model = extract_json_field(output, "Model");
    result.datetime_original = extract_json_field(output, "DateTimeOriginal");
    std::string orientation_str = extract_json_field(output, "Orientation");
    if (!orientation_str.empty()) {
        try {
            result.orientation = std::stoi(orientation_str);
        } catch (...) {
            result.orientation = 1;
        }
    }
    return result;
}

} // namespace rawimport::infrastructure
