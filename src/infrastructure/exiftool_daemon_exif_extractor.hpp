/*
 * rawimport-pipeline - ExifTool Daemon ExifExtractor adapter
 * Uses persistent -stay_open daemon for batch efficiency.
 */
#pragma once

#include <string>
#include <optional>
#include <cstdio>
#include <memory>

#include "domain/ports/exif_extractor.hpp"

namespace rawimport::infrastructure {

class ExiftoolDaemonExifExtractor final : public domain::ExifExtractor {
public:
    explicit ExiftoolDaemonExifExtractor(std::string binary_path = "");
    ~ExiftoolDaemonExifExtractor() override;

    domain::HealthReport                      health_check() override;
    std::optional<domain::ExifResult> extract(const domain::FilePath& path) override;

    // Non-copyable
    ExiftoolDaemonExifExtractor(const ExiftoolDaemonExifExtractor&) = delete;
    ExiftoolDaemonExifExtractor& operator=(const ExiftoolDaemonExifExtractor&) = delete;

private:
    std::string binary_path_;
    std::FILE*  daemon_stream_{nullptr};

    bool open_daemon();
    void close_daemon();
    std::string read_daemon_response() const;
    domain::ExifResult parse_json(const std::string& json_output, const domain::FilePath& path) const;
};

} // namespace rawimport::infrastructure
