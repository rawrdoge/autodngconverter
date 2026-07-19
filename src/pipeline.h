#pragma once
// pipeline.h — shared data types for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §3.1, §7.
#include <cstdint>
#include <string>
#include <vector>

namespace rawimport {

// Conversion settings forwarded to the dnglab `convert` engine.
// NOTE: dnglab `convert` accepts ONLY `-c`/`-f`/`--keep-mtime` + positional
// INPUT OUTPUT. Preview size / dng-version / jpeg-quality are dnglab built-in
// defaults and MUST NOT be passed (they abort conversion).
struct ConversionSettings {
    std::string compression = "lossless";   // lossless | uncompressed
    std::string preview_medium = "1024x1024";
    std::string preview_full = "4000x3000";
    std::string version = "1.4";            // dng version
    int jpeg_quality = 92;
    bool linear = false;
    std::string seed;                        // re-embed determinism only
};

enum class ImportStatus {
    Pending,
    Converting,
    Completed,
    Failed,
    Restored,
    Legacy
};

inline const char* to_string(ImportStatus s) {
    switch (s) {
        case ImportStatus::Pending:    return "pending";
        case ImportStatus::Converting: return "converting";
        case ImportStatus::Completed:  return "completed";
        case ImportStatus::Failed:     return "failed";
        case ImportStatus::Restored:   return "restored";
        case ImportStatus::Legacy:     return "legacy";
    }
    return "pending";
}

enum class DateSource { Exif, Mtime };

// Atomic import record (mirrors Go `ImportRecord` / DB `imports` row).
struct ImportRecord {
    int64_t id = 0;
    int64_t sequence_id = 0;
    std::string source_path;
    std::string source_hash;     // SHA-256 hex (64 chars)
    std::string output_path;
    std::string output_hash;     // SHA-256 hex (64 chars)
    std::string camera_model;
    std::string capture_date;    // YYYY-MM-DD
    std::string capture_time;    // HH:MM:SS
    std::string folder_schema;   // e.g. 2026/07
    std::string conversion_settings; // JSON string
    ImportStatus status = ImportStatus::Pending;
    DateSource date_source = DateSource::Exif;
    int orientation = 0;         // EXIF Orientation 1-8 (0 = unset)
    std::string created_at;
    std::string completed_at;
    std::string error_message;
    std::string sequence_name;   // IMG_{n}
};

// Re-conversion job (mirrors Go `ReconversionJob`).
struct ReconversionJob {
    int64_t id = 0;
    int64_t import_id = 0;
    std::string previous_output_hash;
    std::string new_output_hash;
    ConversionSettings settings;
    std::string reason;
    std::string triggered_at;
    std::string completed_at;
    ImportStatus status = ImportStatus::Pending; // pending|running|completed|failed
    std::string error_message;
};

// Compute SHA-256 hex of a file (OpenSSL EVP_sha256). Returns empty on error.
std::string sha256_file(const std::string& path);

// Compute SHA-256 hex of an in-memory buffer.
std::string sha256_bytes(const std::vector<uint8_t>& data);

// Extract EXIF DateTimeOriginal; falls back to file mtime.
// Returns (capture_date, capture_time, date_source).
struct ExifResult { std::string date; std::string time; DateSource source; };
ExifResult extract_exif_date(const std::string& path, const std::string& exiftool_bin);

// Build folder schema path from a date + FOLDER_SCHEMA template (e.g. %Y/%m).
std::string build_folder_schema(const std::string& tmpl, const std::string& capture_date);

// Extract embedded JPEG preview from a DNG (SubIFD1 medium primary, SubIFD2 full
// fallback). Returns path to written sidecar or empty on failure.
std::string extract_thumbnail(const std::string& dng_path, const std::string& out_path);

} // namespace rawimport