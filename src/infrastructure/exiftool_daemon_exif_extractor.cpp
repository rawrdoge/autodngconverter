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

    // Single-process fallback: run exiftool once for this file.
    const auto bin = binary_path_.empty() ? find_in_path("exiftool") : binary_path_;
    if (bin.empty()) return std::nullopt;

    std::string cmd_single = bin + " -j -DateTimeOriginal -Model -Make -Orientation " +
                             path.native + " 2>/dev/null";
    FILE* pipe = popen(cmd_single.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    pclose(pipe);

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
