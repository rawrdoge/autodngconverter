#pragma once
// config.h — environment-driven configuration for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §9.
#include <string>
#include "pipeline.h"

namespace rawimport {

struct Config {
    // Database (MariaDB only in v1)
    std::string db_host = "mariadb";
    int db_port = 3306;
    std::string db_user = "rawimport";
    std::string db_password;
    std::string db_name = "rawimport";

    // Directories
    std::string watch_dir = "/watch";
    std::string output_dir = "/output";
    std::string archive_dir = "/archive";
    std::string db_dir = "/db";

    // Naming / EXIF
    std::string folder_schema = "%Y/%m";
    std::string file_pattern = "IMG_{seq}";
    std::string converter_engine = "dnglab";
    std::string exiftool_bin = "exiftool";

    // Default conversion settings (PRD §9)
    bool gen_thumb_jpeg = false;
    std::string def_compression = "lossless";
    std::string def_dng_version = "1.4";
    std::string def_preview_medium = "1024x1024";
    std::string def_preview_full = "4000x3000";
    int def_jpeg_quality = 92;
    bool def_linear = false;

    // Watcher
    int poll_interval_sec = 10;
    int debounce_sec = 2;
    int queue_size = 100;

    // Observability
    std::string alert_push_url;   // optional ntfy/Gotify
    int http_port = 8080;

    // Rotation sync (PRD §5)
    int rotation_grace_ms = 2000;
    std::string rotation_mode = "metadata"; // metadata only safe for live sync
    std::string immich_url;
    std::string immich_token;
    std::string digikam_rescan = "touch";   // touch | dbus

    // Lua plugin facing
    std::string api_token;        // optional bearer for Lua->API
};

// Load configuration from environment variables. Mirrors Go LoadConfig (config.go).
Config LoadConfig();

} // namespace rawimport