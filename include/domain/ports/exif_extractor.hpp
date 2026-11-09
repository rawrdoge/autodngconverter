/*
 * rawimport-pipeline - ExifExtractor port interface
 */
#pragma once

#include <string>
#include <optional>

#include "domain/types.hpp"

namespace rawimport::domain {

class ExifExtractor {
public:
    virtual ~ExifExtractor() = default;

    virtual HealthReport  health_check() = 0;

    // Extract EXIF from a single file. Returns nullopt if no EXIF found.
    virtual std::optional<ExifResult> extract(const FilePath& path) = 0;

    // Returns true if this extractor can actually extract EXIF (false for no-op/disabled).
    virtual bool is_available() const = 0;
};

} // namespace rawimport::domain
