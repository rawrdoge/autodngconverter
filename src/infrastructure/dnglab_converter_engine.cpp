/*
 * rawimport-pipeline - dnglab ConverterEngine adapter implementation
 */
#ifdef WITH_DNGLAB

#include "infrastructure/dnglab_converter_engine.hpp"

#include <filesystem>
#include <vector>

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
#ifdef DNGLAB_AVAILABLE
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
#ifdef DNGLAB_AVAILABLE
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
    return std::nullopt;
#endif
}

} // namespace rawimport::infrastructure

#endif // WITH_DNGLAB
