/*
 * rawimport-pipeline - dnglab ConverterEngine adapter
 * Requires dnglab library (header-only or shared). Guarded by WITH_DNGLAB.
 */
#pragma once

#ifdef WITH_DNGLAB

#include <string>
#include <vector>
#include <memory>

#include "domain/ports/converter_engine.hpp"

namespace rawimport::infrastructure {

class DnglabConverterEngine final : public domain::ConverterEngine {
public:
    DnglabConverterEngine();
    ~DnglabConverterEngine() override;

    domain::HealthReport              health_check() override;
    std::vector<std::string>          supported_extensions() override;
    bool                              supports_linear_dng() override;
    bool                              supports_lossy_compression() override;
    int                               max_parallel_workers() override;
    std::optional<domain::FilePath>   convert(const domain::FilePath& source,
                                               const domain::FilePath& dest,
                                               const domain::ConversionSettings& settings) override;

private:
    int max_workers_;
};

} // namespace rawimport::infrastructure

#endif // WITH_DNGLAB
