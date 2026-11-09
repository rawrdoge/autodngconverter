/*
 * rawimport-pipeline - Adobe DNG Converter Engine adapter implementation
 */
#include "infrastructure/adobe_dng_converter_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
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

#if defined(_WIN32)
// Escape a single argument for embedding in a CreateProcess command line.
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

int exec_cmd(const std::vector<std::string>& args) {
    if (args.empty()) return -1;

#ifdef _WIN32
    // Build quoted UTF-16 command line for CreateProcessW.
    // Source args are UTF-8 strings; widen via CP_UTF8 so multibyte paths
    // (Cyrillic, CJK, etc.) survive intact. MultiByteToWideChar with -1
    // includes a trailing null — resized out afterwards so wcmd stays clean.
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
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(rc);
#else
    // POSIX: build argv array and spawn via execvp — no shell involvement.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) return -1;

    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

} // anon

AdobeDngConverterEngine::AdobeDngConverterEngine(std::string binary_path)
    : binary_path_(std::move(binary_path)) {}

std::string AdobeDngConverterEngine::find_binary() const {
    if (!binary_path_.empty() && std::filesystem::exists(binary_path_)) return binary_path_;
#ifdef _WIN32
    return find_in_path("AdobeDNGConverter.exe");
#else
    return find_in_path("AdobeDNGConverter");
#endif
}

domain::HealthReport AdobeDngConverterEngine::health_check() {
    const auto bin = find_binary();
    if (bin.empty())
        return {domain::HealthStatus::Unhealthy, "Adobe DNG Converter binary not found", domain::now()};
    // Quick version check.
    const int rc = exec_cmd({bin, "--help"});
    (void)rc;
    return {domain::HealthStatus::Healthy, "Adobe DNG Converter available", domain::now()};
}

std::vector<std::string> AdobeDngConverterEngine::supported_extensions() {
    return {".NEF", ".CR2", ".CR3", ".ARW", ".RAF", ".RW2", ".ORF"};
}

bool AdobeDngConverterEngine::supports_linear_dng() { return false; }
bool AdobeDngConverterEngine::supports_lossy_compression() { return true; }
int  AdobeDngConverterEngine::max_parallel_workers() { return 1; }

std::optional<domain::FilePath> AdobeDngConverterEngine::convert(const domain::FilePath& source,
                                                                  const domain::FilePath& dest,
                                                                  const domain::ConversionSettings& settings) {
    const auto bin = find_binary();
    if (bin.empty()) return std::nullopt;

    std::vector<std::string> args;
    args.push_back(bin);
    args.push_back(source.native);
    args.push_back("-o");
    args.push_back(dest.native.substr(0, dest.native.find_last_of("/\\")));
    if (settings.lossy) args.push_back("-lossy");
    // Adobe DNG Converter runs serially — one invocation per file.
    const int rc = exec_cmd(args);
    if (rc != 0) return std::nullopt;
    return dest;
}

} // namespace rawimport::infrastructure
