#pragma once
// db.h — SQLite store interface for the RawImport portable branch (v2.1.0-portable).
// Replaces the MariaDB-backed store: local WAL-mode DB file, zero external infra.
// Feature subset per ACTION_PLAN_Portable_Layman_Binary_v2.1.0.md §4:
// no rotation sync, reconversions, preview edits, alerts, or path lookups.
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>

struct sqlite3;

#include "pipeline.h"
#include "config.h"

namespace rawimport {

// Thin SQLite-backed store. Each thread owns its own connection
// (CreateConnection + adopted-ctor), all pointing at the same WAL DB file.
class Store {
public:
    Store();
    // Adopt an externally created connection (per-worker connections).
    // Does not require Open(); the destructor releases it via Close().
    explicit Store(sqlite3* adopted_db);
    ~Store();

    // Open (creating the DB file and its parent dirs if needed) + apply
    // pragmas: journal_mode=WAL, busy_timeout=5000, foreign_keys=ON.
    bool Open(const Config& cfg);

    // Apply the embedded schema (idempotent; mirrors migrations/0001_init_sqlite.sql,
    // which is compiled into the binary so the release ZIP needs no migrations dir).
    bool Migrate();

    // Allocate a monotonic sequence id (BEGIN IMMEDIATE + AUTOINCREMENT +
    // last_insert_rowid; SQLite's writer lock guarantees monotonicity).
    // Returns (id, "IMG_{id}"); (0, "") on failure.
    std::pair<int64_t, std::string> AllocateSequence();

    // Insert a completed import atomically. Returns new row id (0 on failure).
    int64_t InsertImport(const ImportRecord& rec);

    // Lookup by sequence name (IMG_{n}).
    std::optional<ImportRecord> GetImportBySequence(const std::string& seq);
    // Lookup by source OR output SHA-256 (dedup gate).
    std::optional<ImportRecord> GetImportByHash(const std::string& sha);

    // Stats for /api/v1/stats.
    struct Stats {
        int64_t total = 0;
        int64_t completed = 0;
        int64_t failed = 0;
        int queue_depth = 0;
    };
    Stats GetStats();

    // List imports (pagination + optional status/camera filters).
    std::vector<ImportRecord> ListImports(int page, int limit,
                                          const std::string& status_filter,
                                          const std::string& camera_filter);

    void Close();

    // Cheap DB connectivity probe. Used by GET /ready.
    bool Ping();

    // Create a new independent connection for worker threads.
    // Caller owns the returned handle; pass it to Store(sqlite3*) which closes it.
    static sqlite3* CreateConnection(const Config& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport