#pragma once
// config.h — environment-driven configuration for the RawImport portable build.
// Env vars only (no INI, no second config system) per action plan §5 Phase 2.
// Defaults resolve against the portable root: RAWIMPORT_HOME if set, otherwise
// the directory containing the executable.
#include <string>
#include "pipeline.h"

namespace rawimport {

// Portable root resolution order (action plan §5 Phase 2):
// 1. RAWIMPORT_HOME env var (explicit override)
// 2. Directory containing the executable
std::string get_portable_root();

// Resolve an external tool binary (action plan §5 Phase 3):
// 1. override_path (env var like CONVERTER_ENGINE_BIN / EXIFTOOL_BIN)
// 2. <root>/tools/<name>[.exe]
// 3. bare name (PATH lookup)
std::string resolve_tool(const std::string& override_path, const std::string& root,
                         const std::string& name);

struct Config {
    // Portable root (exe dir or RAWIMPORT_HOME).
    std::string root;

    // Directories (default to <root>/...; overridable by env)
    std::string watch_dir;
    std::string output_dir;
    std::string archive_dir;
    std::string db_name;   // full path of the SQLite DB file (<root>/data/rawimport.db)

    // Naming / EXIF
    std::string folder_schema = "%Y/%m";
    std::string file_pattern = "IMG_{seq}";
    std::string converter_engine_bin;   // resolved dnglab path (env -> tools/ -> PATH)
    std::string exiftool_bin;           // resolved exiftool path (env -> tools/ -> PATH)

    // Default conversion settings (dnglab CLI accepts only -c/-f/--keep-mtime)
    bool gen_thumb_jpeg = false;
    std::string def_compression = "lossless";

    // Watcher
    int poll_interval_sec = 10;
    int debounce_sec = 10;
    int queue_size = 100;

    // Performance & reliability
    int max_converter_workers = 0;  // 0 = auto (hardware_concurrency, capped at 8)
    int dead_letter_max_retries = 3;
    bool fast_fingerprint = true;   // two-stage dedup gate before full SHA-256

    // Observability
    int http_port = 8080;
};

// Load configuration from environment variables with portable-root defaults.
Config LoadConfig();

} // namespace rawimport