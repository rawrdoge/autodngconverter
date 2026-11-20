/*
 * rawimport-pipeline - ExifTool CLI ExifExtractor adapter implementation
 */
#include "infrastructure/exiftool_exif_extractor.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace rawimport::infrastructure {

namespace {

std::string find_in_path(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        auto p = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) return p.string();
    }
    return {};
}

std::string exec_capture_argv(const std::vector<std::string>& args) {
    if (args.empty()) return {};
    std::array<char, 4096> buf{};
    std::ostringstream out;

    // Build argv array for execvp
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) == -1) return {};

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {};
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);
    FILE* fp = fdopen(pipefd[0], "r");
    if (!fp) {
        waitpid(pid, nullptr, 0);
        return {};
    }

    while (fgets(buf.data(), buf.size(), fp))
        out << buf.data();
    fclose(fp);

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        return {};
    }

    return out.str();
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

} // anon

ExiftoolExifExtractor::ExiftoolExifExtractor(std::string binary_path)
    : binary_path_(std::move(binary_path)) {}

std::string ExiftoolExifExtractor::find_binary() const {
    if (!binary_path_.empty() && std::filesystem::exists(binary_path_)) return binary_path_;
    return find_in_path("exiftool");
}

domain::HealthReport ExiftoolExifExtractor::health_check() {
    const auto bin = find_binary();
    if (bin.empty())
        return {domain::HealthStatus::Unhealthy, "exiftool binary not found", domain::now()};
    const auto ver = exec_capture_argv({bin, "-ver"});
    return {domain::HealthStatus::Healthy, "exiftool " + ver, domain::now()};
}

std::optional<domain::ExifResult> ExiftoolExifExtractor::extract(const domain::FilePath& path) {
    const auto bin = find_binary();
    if (bin.empty()) return std::nullopt;

    const auto json = exec_capture_argv({bin, "-j", "-DateTimeOriginal", "-Model", "-Make", "-Orientation", "-FileSize", "-ModifyDate", path.native});

    if (json.empty()) {
        // Fallback to mtime.
        domain::ExifResult r;
        struct stat st{};
        if (stat(path.native.c_str(), &st) == 0) {
            auto tp = std::chrono::system_clock::from_time_t(st.st_mtime);
            r.datetime_original = std::to_string(tp.time_since_epoch().count());
        }
        return r;
    }

    return parse_json(json, path);
}

domain::ExifResult ExiftoolExifExtractor::parse_json(const std::string& json,
                                                      const domain::FilePath& path) const {
    domain::ExifResult r;
    r.make       = extract_json_field(json, "CameraMaker");
    if (r.make.empty()) r.make = extract_json_field(json, "Make");
    r.model      = extract_json_field(json, "CameraModelName");
    if (r.model.empty()) r.model = extract_json_field(json, "Model");
    r.datetime_original = extract_json_field(json, "DateTimeOriginal");
    r.orientation   = 1;
    {
        auto o = extract_json_field(json, "Orientation");
        if (!o.empty()) r.orientation = std::stoi(o);
    }
    return r;
}

} // namespace rawimport::infrastructure
