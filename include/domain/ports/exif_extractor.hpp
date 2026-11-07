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
};

} // namespace rawimport::domain
