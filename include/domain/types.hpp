/*
 * rawimport-pipeline - domain data types (hexagonal architecture)
 * Pure C++, zero external dependencies.  Requires C++20 for span & defaulted ops.
 */
#pragma once

#include <cstdint>
#include <string>
#include <sstream>
#include <vector>
#include <optional>
#include <chrono>
#include <array>
#include <map>
#include <span>
#include <iomanip>

namespace rawimport::domain {

// ─── Timestamps ───────────────────────────────────────────────────────────────
using Instant = std::chrono::system_clock::time_point;
inline Instant now() { return std::chrono::system_clock::now(); }

// ─── File identity ────────────────────────────────────────────────────────────
struct FilePath {
    std::string native;
    std::string filename() const {
        std::size_t p = native.find_last_of("/\\");
        return p == std::string::npos ? native : native.substr(p + 1);
    }
    std::string extension() const {
        std::size_t p = native.find_last_of('.');
        return p == std::string::npos ? "" : native.substr(p);
    }
    bool operator==(const FilePath&) const = default;
};

// ─── Hash / fingerprint ───────────────────────────────────────────────────────
using SHA256Hash = std::array<uint8_t, 32>;
struct FastFingerprint {
    uint64_t size_bytes{0};
    Instant  mtime{};
    uint64_t fnv1a{0};
    bool operator==(const FastFingerprint&) const = default;
};

// ─── Engine selection ─────────────────────────────────────────────────────────
enum class RawConverterEngine : uint8_t {
    Dnglab          = 0,
    AdobeDngConverter = 1,
};
inline std::string engine_name(RawConverterEngine e) {
    switch (e) {
        case RawConverterEngine::Dnglab:            return "dnglab";
        case RawConverterEngine::AdobeDngConverter: return "adobe_dng";
    }
    return "unknown";
}

// ─── Conversion settings ─────────────────────────────────────────────────────
struct ConversionSettings {
    RawConverterEngine engine       = RawConverterEngine::Dnglab;
    bool               linear_dng   = false;
    bool               lossy        = false;
    int                compression  = 4;
    std::string        metadata_profile;
    std::map<std::string, std::string> extra;
};

// ─── EXIF result ──────────────────────────────────────────────────────────────
struct ExifResult {
    std::string make;
    std::string model;
    std::string datetime_original;
    int         orientation{1};
    std::map<std::string, std::string> raw;
};

// ─── Import record ────────────────────────────────────────────────────────────
enum class ImportStatus : uint8_t {
    Queued     = 0,
    Converting = 1,
    Completed  = 2,
    Failed     = 3,
};
inline const char* import_status_str(ImportStatus s) {
    switch (s) {
        case ImportStatus::Queued:      return "queued";
        case ImportStatus::Converting:  return "converting";
        case ImportStatus::Completed:   return "completed";
        case ImportStatus::Failed:      return "failed";
    }
    return "unknown";
}

struct ImportRecord {
    int64_t              sequence_id{-1};
    FilePath             source_path;
    FilePath             output_path;
    ImportStatus         status        = ImportStatus::Queued;
    std::string          error_message;
    FastFingerprint      source_fingerprint;
    Instant              created_at;
    Instant              started_at;
    Instant              completed_at;
};

// ─── Reconversion record ─────────────────────────────────────────────────────
enum class ReconversionStatus : uint8_t {
    Pending  = 0,
    Running  = 1,
    Done     = 2,
    Failed   = 3,
};

struct ReconversionRecord {
    int64_t     id{-1};
    int64_t     import_id;
    FilePath    source_dng_path;
    FilePath    output_dng_path;
    ReconversionStatus status = ReconversionStatus::Pending;
    std::string error_message;
    Instant     scheduled_at;
    Instant     started_at;
    Instant     completed_at;
};

// ─── Preview edit ─────────────────────────────────────────────────────────────
struct PreviewEdit {
    int64_t     id{-1};
    int64_t     import_id{0};
    FilePath    preview_path;
    ConversionSettings settings;
    Instant     created_at;
};

// ─── Alert ────────────────────────────────────────────────────────────────────
enum class AlertSeverity : uint8_t { Info, Warning, Error };

struct Alert {
    int64_t     id{-1};
    int64_t     import_id{-1};
    AlertSeverity severity = AlertSeverity::Info;
    std::string message;
    std::string code;
    Instant     created_at;
};

// ─── Lock ─────────────────────────────────────────────────────────────────────
struct Lock {
    int64_t     id{-1};
    int64_t     import_id{0};
    std::string holder;
    Instant     acquired_at;
    Instant     expires_at;
};

// ─── Scan result ──────────────────────────────────────────────────────────────
struct ScanResult {
    std::vector<FilePath> new_files;
    std::vector<FilePath> removed_files;
    std::vector<FilePath> unchanged_files;
};

// ─── HealthStatus ─────────────────────────────────────────────────────────────
enum class HealthStatus : uint8_t { Healthy, Degraded, Unhealthy };
struct HealthReport {
    HealthStatus status      = HealthStatus::Healthy;
    std::string  message;
    Instant      checked_at;
};

// ─── Config validation ────────────────────────────────────────────────────────
struct RequiredConfigKey {
    std::string name;
    std::string description;
};
struct ValidationReport {
    bool                   valid             = false;
    std::vector<std::string> missing_keys;
    std::vector<std::string> invalid_values;
};

// SHA-256 hex encoding utility (used by HashService implementations).
inline std::string sha256_hex(const SHA256Hash& h) {
    std::ostringstream oss;
    for (auto byte : h)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return oss.str();
}

inline std::vector<RequiredConfigKey> required_config_keys() {
    return {
        {"OUTPUT_DIR",   "Destination directory for converted DNG files"},
        {"INPUT_DIR",    "Watched directory containing RAW source files"},
        {"CONVERTER_ENGINE", "Converter engine to use (dnglab | adobedng)"},
        {"DB_CONNECTION",    "Database connection string (libpq/MariaDB format)"},
        {"DATABASE_URL",     "Alternative database connection string"},
        {"DB_DRIVER",        "Database driver: postgresql | mariadb"},
    };
}

} // namespace rawimport::domain
