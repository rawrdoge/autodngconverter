/*
 * rawimport-pipeline - ExifTool ExifExtractor adapter (CLI mode)
 * Spawns exiftool per file.
 */
#pragma once

#include <string>
#include <optional>

#include "domain/ports/exif_extractor.hpp"

namespace rawimport::infrastructure {

class ExiftoolExifExtractor final : public domain::ExifExtractor {
public:
    // Path to exiftool binary. Empty = search PATH.
    explicit ExiftoolExifExtractor(std::string binary_path = "");

    HealthReport            health_check() override;
    std::optional<domain::ExifResult> extract(const domain::FilePath& path) override;

private:
    std::string binary_path_;
    std::string find_binary() const;
    std::string run_exiftool(const std::string& args) const;
    domain::ExifResult parse_json(const std::string& json_output, const domain::FilePath& path) const;
};

} // namespace rawimport::infrastructure
