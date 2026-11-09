/*
 * rawimport-pipeline - dnglab ConverterEngine adapter implementation
 */
#ifdef WITH_DNGLAB

#include "infrastructure/dnglab_converter_engine.hpp"

#include <filesystem>
#include <vector>
#include <sstream>
#include <cstdlib>

// dnglab is expected as a header-only or shared-lib dependency.
// When present, include its public header.
#ifdef __has_include
#  if __has_include(<dnglab/converter.hpp>)
#    include <dnglab/converter.hpp>
#  endif
#endif

namespace rawimport::infrastructure {

DnglabConverterEngine::DnglabConverterEngine() : max_workers_(8) {}

DnglabConverterEngine::~DnglabConverterEngine() = default;

domain::HealthReport DnglabConverterEngine::health_check() {
#ifdef WITH_DNGLAB
    return {domain::HealthStatus::Healthy, "dnglab library available", domain::now()};
#else
    // Check for binary in PATH as fallback.
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {domain::HealthStatus::Unhealthy, "dnglab not found (no lib, no binary)", domain::now()};
    return {domain::HealthStatus::Degraded, "dnglab C++ library unavailable, using CLI mode", domain::now()};
#endif
}

std::vector<std::string> DnglabConverterEngine::supported_extensions() {
    return {".NEF", ".CR2", ".CR3", ".ARW", ".RAF", ".RW2", ".ORF", ".DNG"};
}

bool DnglabConverterEngine::supports_linear_dng() { return true; }
bool DnglabConverterEngine::supports_lossy_compression() { return true; }
int  DnglabConverterEngine::max_parallel_workers() { return max_workers_; }

std::optional<domain::FilePath> DnglabConverterEngine::convert(const domain::FilePath& source,
                                                                const domain::FilePath& dest,
                                                                const domain::ConversionSettings& settings) {
#ifdef WITH_DNGLAB
    try {
        dnglab::Converter converter;
        dnglab::ConvertOptions opts;
        opts.linear_dng   = settings.linear_dng;
        opts.lossy        = settings.lossy;
        opts.compression  = settings.compression;
        for (const auto& [k, v] : settings.extra)
            opts.extra[k] = v;

        const auto result = converter.convert(source.native, dest.native, opts);
        if (result.success) return dest;
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
#else
    // CLI fallback: invoke dnglab binary if available.
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    
    std::string bin;
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        auto p = std::filesystem::path(dir) / "dnglab";
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
            bin = p.string();
            break;
        }
    }
    if (bin.empty()) return std::nullopt;

    std::vector<std::string> args = {bin, "convert"};
    if (settings.linear_dng) args.push_back("--linear");
    if (settings.lossy) args.push_back("--lossy");
    if (settings.compression > 0) {
        args.push_back("--compression");
        args.push_back(std::to_string(settings.compression));
    }
    for (const auto& [k, v] : settings.extra) {
        args.push_back("--" + k);
        args.push_back(v);
    }
    args.push_back(source.native);
    args.push_back(dest.native);

    // Execute via execvp (POSIX) or CreateProcessW (Windows)
    // Simplified: use system() for CLI fallback (not production-ready)
    std::ostringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd << ' ';
        cmd << args[i];
    }
    int rc = std::system(cmd.str().c_str());
    if (rc == 0) return dest;
    return std::nullopt;
#endif
}

} // namespace rawimport::infrastructure

#endif // WITH_DNGLAB
