// config.cpp - environment-driven configuration loading for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md section 9.
#include <cstdlib>
#include <string>
#include <algorithm>
#include "config.h"

namespace rawimport {

namespace {

std::string trim(const std::string& s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    auto begin = std::find_if_not(s.begin(), s.end(), is_space);
    if (begin == s.end()) return {};
    auto end = std::find_if_not(s.rbegin(), s.rend(), is_space).base();
    return std::string(begin, end);
}

std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    std::string s = trim(std::string(v));
    return s.empty() ? def : s;
}

bool env_bool(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    std::string s = trim(std::string(v));
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s == "1" || s == "true" || s == "yes";
}

int env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    std::string s = trim(std::string(v));
    if (s.empty()) return def;
    try {
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

} // anonymous namespace

Config LoadConfig() {
    Config c;

    c.db_host = env_or("DB_HOST", c.db_host);
    c.db_port = env_int("DB_PORT", c.db_port);
    c.db_user = env_or("DB_USER", c.db_user);
    c.db_password = env_or("DB_PASSWORD", c.db_password);
    c.db_name = env_or("DB_NAME", c.db_name);

    c.watch_dir = env_or("WATCH_DIR", c.watch_dir);
    c.output_dir = env_or("OUTPUT_DIR", c.output_dir);
    c.archive_dir = env_or("ARCHIVE_DIR", c.archive_dir);
    c.db_dir = env_or("DB_DIR", c.db_dir);

    c.folder_schema = env_or("FOLDER_SCHEMA", c.folder_schema);
    c.file_pattern = env_or("FILE_PATTERN", c.file_pattern);
    c.converter_engine = env_or("CONVERTER_ENGINE", c.converter_engine);
    c.exiftool_bin = env_or("EXIFTOOL_BIN", c.exiftool_bin);

    c.gen_thumb_jpeg = env_bool("GEN_THUMB_JPEG", c.gen_thumb_jpeg);
    c.def_compression = env_or("DEF_COMPRESSION", c.def_compression);
    c.def_dng_version = env_or("DEF_DNG_VERSION", c.def_dng_version);
    c.def_preview_medium = env_or("DEF_PREVIEW_MEDIUM", c.def_preview_medium);
    c.def_preview_full = env_or("DEF_PREVIEW_FULL", c.def_preview_full);
    c.def_jpeg_quality = env_int("DEF_JPEG_QUALITY", c.def_jpeg_quality);
    c.def_linear = env_bool("DEF_LINEAR", c.def_linear);

    c.poll_interval_sec = env_int("POLL_INTERVAL_SEC", c.poll_interval_sec);
    c.debounce_sec = env_int("DEBOUNCE_SEC", c.debounce_sec);
    c.queue_size = env_int("QUEUE_SIZE", c.queue_size);

    c.alert_push_url = env_or("ALERT_PUSH_URL", c.alert_push_url);
    c.http_port = env_int("HTTP_PORT", c.http_port);

    c.rotation_grace_ms = env_int("ROTATION_GRACE_MS", c.rotation_grace_ms);
    c.rotation_mode = env_or("ROTATION_MODE", c.rotation_mode);
    c.immich_url = env_or("IMMICH_URL", c.immich_url);
    c.immich_token = env_or("IMMICH_TOKEN", c.immich_token);
    c.digikam_rescan = env_or("DIGIKAM_RESCAN", c.digikam_rescan);

    c.api_token = env_or("API_TOKEN", c.api_token);

    return c;
}

} // namespace rawimport
