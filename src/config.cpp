// config.cpp - environment-driven configuration loading for the portable build.
// Env vars only; no INI parser (action plan §5 Phase 2).
#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace rawimport {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
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

std::string exe_directory() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(std::string(buf, n)).parent_path().string();
    }
#else
    char buf[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        return std::filesystem::path(std::string(buf, static_cast<size_t>(n)))
            .parent_path()
            .string();
    }
#endif
    return ".";
}

} // anonymous namespace

std::string get_portable_root() {
    // 1. RAWIMPORT_HOME explicit override.
    const char* home = std::getenv("RAWIMPORT_HOME");
    if (home && home[0]) return trim(std::string(home));
    // 2. Directory containing the executable (true portable mode).
    return exe_directory();
}

// Resolve an external tool binary (action plan §5 Phase 3):
// 1. Explicit env override (CONVERTER_ENGINE_BIN / EXIFTOOL_BIN)
// 2. <root>/tools/<name> or <root>/tools/<name>.exe
// 3. PATH lookup (bare name)
std::string resolve_tool(const std::string& override_path, const std::string& root,
                         const std::string& name) {
    namespace fs = std::filesystem;
    if (!override_path.empty()) return override_path;
    for (const char* suffix : {"", ".exe"}) {
        fs::path candidate =
            fs::path(root) / "tools" / (name + suffix);
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate.string();
    }
    return name;  // bare name -> PATH lookup
}

Config LoadConfig() {
    Config c;
    c.root = get_portable_root();

    // Auto-folder defaults under the portable root (action plan §5 Phase 2).
    c.watch_dir = env_or("WATCH_DIR", c.root + "/watch");
    c.output_dir = env_or("OUTPUT_DIR", c.root + "/output");
    c.archive_dir = env_or("ARCHIVE_DIR", c.root + "/archive");
    c.db_name = env_or("DB_NAME", c.root + "/data/rawimport.db");
    // db_host / db_port / db_user / db_password are intentionally absent:
    // SQLite needs no server.

    c.folder_schema = env_or("FOLDER_SCHEMA", c.folder_schema);
    c.file_pattern = env_or("FILE_PATTERN", c.file_pattern);

    // Tool resolution (action plan §5 Phase 3): no downloader, bundled binaries.
    c.converter_engine_bin = resolve_tool(env_or("CONVERTER_ENGINE_BIN", ""), c.root, "dnglab");
    c.exiftool_bin = resolve_tool(env_or("EXIFTOOL_BIN", ""), c.root, "exiftool");

    c.gen_thumb_jpeg = env_bool("GEN_THUMB_JPEG", c.gen_thumb_jpeg);
    c.def_compression = env_or("DEF_COMPRESSION", c.def_compression);

    // Accept both spellings; POLL_INTERVAL is the documented name.
    c.poll_interval_sec = env_int("POLL_INTERVAL", env_int("POLL_INTERVAL_SEC", c.poll_interval_sec));
    c.debounce_sec = env_int("DEBOUNCE_SEC", c.debounce_sec);
    c.queue_size = env_int("QUEUE_SIZE", c.queue_size);

    c.max_converter_workers = env_int("MAX_CONVERTER_WORKERS", c.max_converter_workers);
    c.dead_letter_max_retries = env_int("DEAD_LETTER_MAX_RETRIES", c.dead_letter_max_retries);
    c.fast_fingerprint = env_bool("FAST_FINGERPRINT", c.fast_fingerprint);

    c.http_port = env_int("HTTP_PORT", c.http_port);

    return c;
}

} // namespace rawimport