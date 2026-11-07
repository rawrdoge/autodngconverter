/*
 * rawimport-pipeline - Migration options and result types
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <chrono>

namespace rawimport::migration {

// ─── Migration direction ──────────────────────────────────────────────────────
enum class SourceDB : uint8_t { MariaDB, PostgreSQL };
enum class TargetDB   : uint8_t { MariaDB, PostgreSQL };

inline const char* source_db_str(SourceDB db) {
    switch (db) {
        case SourceDB::MariaDB:   return "mariadb";
        case SourceDB::PostgreSQL: return "postgresql";
    }
    return "unknown";
}
inline const char* target_db_str(TargetDB db) {
    switch (db) {
        case TargetDB::MariaDB:   return "mariadb";
        case TargetDB::PostgreSQL: return "postgresql";
    }
    return "unknown";
}

// ─── Options ───────────────────────────────────────────────────────────────────
struct MigrationOptions {
    SourceDB         from{};
    TargetDB         to{};
    std::string      source_dsn;          // e.g. "host=X;user=Y;password=***;database=D"
    std::string      target_dsn;          // libpq conninfo or mariadb connection string
    bool             dry_run  = false;
    int              batch_size = 5000;
    bool             verify_counts = false;
    bool             guide_mode   = false;
    bool             force        = false; // allow migrating into non-empty target
};

// ─── Schema diff entry ────────────────────────────────────────────────────────
struct SchemaDiffEntry {
    std::string table_name;
    int64_t     source_count = 0;
    int64_t     target_count = 0;
    bool        fulltext_skip = false; // FULLTEXT index — needs manual tsvector rebuild
    std::string notes;
};

// ─── Migration statistics ─────────────────────────────────────────────────────
struct MigrationStats {
    int64_t sequences_inserted = 0;
    int64_t imports_inserted   = 0;
    int64_t reconversions_inserted = 0;
    int64_t preview_edits_inserted = 0;
    int64_t alerts_inserted    = 0;
    double  elapsed_seconds    = 0.0;
    std::vector<SchemaDiffEntry> diff;
};

// ─── Result ────────────────────────────────────────────────────────────────────
enum class MigrationStatus : uint8_t {
    Success,
    SkippedDryRun,
    FailedConnectSource,
    FailedConnectTarget,
    FailedSchemaCreate,
    FailedDrainLocks,
    FailedCountVerify,
    FailedSequencePreserve,
    TargetNotEmpty,
    UserCancelled,
    Aborted,
};

struct MigrationResult {
    MigrationStatus status = MigrationStatus::Aborted;
    MigrationStats  stats{};
    std::string     message;       // human-readable summary
    std::vector<std::string> warnings;
};

} // namespace rawimport::migration
