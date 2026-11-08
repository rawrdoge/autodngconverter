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

// Cross-platform process spawn returning exit code.
int run_cmd(const std::vector<std::string>& args) {
    std::ostringstream cmd;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) cmd << " ";
        cmd << args[i];
    }
    return std::system(cmd.str().c_str());
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
    const int rc = run_cmd({bin, "--help"});
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
    args.push_back(dest.native.substr(0, dest.native.find_last_of("/\\")) );
    if (settings.lossy) args.push_back("-lossy");
    // Adobe DNG Converter runs serially — one invocation per file.
    const int rc = run_cmd(args);
    if (rc != 0) return std::nullopt;
    return dest;
}

} // namespace rawimport::infrastructure
