#pragma once
// db.h — MariaDB store interface for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §3.4, §7.
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mysql.h>
#include "pipeline.h"
#include "config.h"

namespace rawimport {

// Thin MariaDB-backed store.
class Store {
public:
    Store();
    // Adopt an externally created connection (per-worker connections, PRD §7.1).
    // Does not require Open(); the destructor releases the connection via Close().
    explicit Store(MYSQL* adopted_conn);
    ~Store();

    // Open + ping-retry connection. Returns false on failure.
    bool Open(const Config& cfg);

    // Apply pending migrations from the migrations/ directory (idempotent).
    bool Migrate(const std::string& migrations_dir);

    // Allocate a monotonic sequence id (AUTO_INCREMENT + LAST_INSERT_ID()).
    // Returns (id, "IMG_{id}").
    std::pair<int64_t, std::string> AllocateSequence();

    // Insert a completed import atomically. Returns new row id (0 on failure).
    int64_t InsertImport(const ImportRecord& rec);

    // Lookup by sequence name (IMG_{n}).
    std::optional<ImportRecord> GetImportBySequence(const std::string& seq);
    // Lookup by primary row id (imports.id). Used by reconvert (L1 fix).
    std::optional<ImportRecord> GetImportById(int64_t id);
    // Lookup by source or output SHA-256.
    std::optional<ImportRecord> GetImportByHash(const std::string& sha);
    // Lookup by source file path (used by Lua re-embed resolution).
    std::optional<ImportRecord> GetImportBySourcePath(const std::string& path);
    // Lookup by output DNG path (used by preview-updated hash sync, PRD §3.2).
    std::optional<ImportRecord> GetImportByOutputPath(const std::string& path);

    // Update output hash after re-embed (preview-updated notify).
    bool UpdateOutputHash(int64_t id, const std::string& new_hash);

    // Update agreed EXIF orientation (rotation sync). PRD §5.
    bool UpdateOrientation(int64_t id, int orientation);

    // Record a preview re-embed audit row.
    bool RecordPreviewEdit(int64_t import_id, const std::string& worker,
                           const std::string& prev_hash, const std::string& new_hash,
                           int width, int height, int quality);

    // Insert a reconversion job; returns new id.
    int64_t InsertReconversion(int64_t import_id, const ReconversionJob& job);
    bool UpdateReconversion(int64_t id, const std::string& new_hash, ImportStatus status);

    // ---- processing_locks (serialization for reconvert/re-embed/rotation) ----
    // Returns true if lock acquired (or already held by same worker within TTL).
    bool AcquireLock(int64_t import_id, const std::string& worker_id, int ttl_sec);
    bool ReleaseLock(int64_t import_id);
    bool HasOutputHash(const std::string& hash);

    // Insert an alert row. Reuses the existing alerts table (amendment
    // ruling #3 — no new table). ref_sequence is optional (empty = NULL-less '').
    bool InsertAlert(const std::string& severity, const std::string& category,
                     const std::string& message, const std::string& ref_sequence = "");

    // ---- CCT analysis results (PRD-CCT-001 §4.2) ----
    struct CctResult {
        int64_t id = 0;
        int64_t import_id = 0;
        std::string sequence_name;
        std::string algorithm;
        double cct_kelvin = 0;
        double tint = 0;
        double xy_x = 0;
        double xy_y = 0;
        double hue = 0;       // radians, D50-adapted
        double chroma = 0;    // D50-adapted distance
        double confidence = 0;
        std::string sampled_area;
        std::string source_used;
        std::string status;
        std::string error_msg;
        std::string created_at;
        std::string completed_at;
    };

    // Insert a pending job. Returns new row id (0 on failure).
    int64_t InsertCctJob(int64_t import_id, const std::string& sequence_name,
                         const std::string& algorithm, const std::string& sampled_area);

    // Update result after analysis completes or fails. Sets completed_at
    // automatically for terminal statuses.
    bool UpdateCctResult(int64_t id, const CctResult& r);

    // Latest result for an import + algorithm (nullopt if none).
    std::optional<CctResult> GetCctResult(int64_t import_id,
                                          const std::string& algorithm);

    // All results for an import, oldest first.
    std::vector<CctResult> ListCctResults(int64_t import_id);

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

    // Cheap DB connectivity probe (mysql_ping). Used by GET /ready.
    bool Ping();

    // Create a new independent connection for worker threads.
    // Caller owns the returned MYSQL* and must mysql_close() it.
    static MYSQL* CreateConnection(const Config& cfg);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport