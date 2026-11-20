/*
 * rawimport-pipeline - ConverterEngine port interface
 */
#pragma once

#include <string>
#include <vector>
#include <map>

#include "domain/types.hpp"

namespace rawimport::domain {

class ConverterEngine {
public:
    virtual ~ConverterEngine() = default;

    virtual HealthReport      health_check() = 0;
    virtual std::vector<std::string> supported_extensions() = 0;
    virtual bool              supports_linear_dng() = 0;
    virtual bool              supports_lossy_compression() = 0;
    virtual int               max_parallel_workers() = 0;

    // Convert a single RAW file to DNG. Returns output path on success.
    virtual std::optional<FilePath> convert(const FilePath& source,
                                             const FilePath& dest,
                                             const ConversionSettings& settings) = 0;

    // Engine identifier for logging/metrics (e.g., "dnglab", "adobe_dng")
    virtual std::string name() const = 0;
};

} // namespace rawimport::domain
