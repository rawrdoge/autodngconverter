/*
 * rawimport-pipeline - ImportStore port interface
 */
#pragma once

#include <vector>
#include <optional>
#include <string>
#include <map>

#include "domain/types.hpp"

namespace rawimport::domain {

class ImportStore {
public:
    virtual ~ImportStore() = default;

    // ── CRUD on imports ───────────────────────────────────────────────────────
    virtual std::optional<int64_t> create_import(const ImportRecord& rec) = 0;
    virtual std::optional<ImportRecord> get_import(int64_t id) = 0;
    virtual bool update_import(ImportRecord rec) = 0;
    virtual std::vector<ImportRecord> list_imports(std::optional<ImportStatus> status_filter = std::nullopt) = 0;

    // ── Reconversions ─────────────────────────────────────────────────────────
    virtual std::optional<int64_t> create_reconversion(const ReconversionRecord& rec) = 0;
    virtual std::optional<ReconversionRecord> get_reconversion(int64_t id) = 0;
    virtual bool update_reconversion(ReconversionRecord rec) = 0;
    virtual std::vector<ReconversionRecord> list_reconversions(int64_t import_id) = 0;

    // ── Preview edits ─────────────────────────────────────────────────────────
    virtual std::optional<int64_t> create_preview_edit(const PreviewEdit& edit) = 0;
    virtual std::optional<PreviewEdit> get_preview_edit(int64_t id) = 0;
    virtual bool delete_preview_edit(int64_t id) = 0;
    virtual std::vector<PreviewEdit> list_preview_edits(int64_t import_id) = 0;

    // ── Alerts ────────────────────────────────────────────────────────────────
    virtual std::optional<int64_t> create_alert(const Alert& alert) = 0;
    virtual std::optional<Alert> get_alert(int64_t id) = 0;
    virtual bool delete_alert(int64_t id) = 0;
    virtual std::vector<Alert> list_alerts(std::optional<int64_t> import_id = std::nullopt) = 0;

    // ── Locks ─────────────────────────────────────────────────────────────────
    virtual std::optional<int64_t> acquire_lock(int64_t import_id, const std::string& holder) = 0;
    virtual bool release_lock(int64_t lock_id) = 0;
    virtual bool is_locked(int64_t import_id) = 0;
    virtual std::vector<Lock> list_active_locks() = 0;

    // ── Sequences ─────────────────────────────────────────────────────────────
    virtual int64_t next_sequence_id() = 0;

    // ── Migration snapshot / restore ───────────────────────────────────────────
    virtual std::string snapshot_to_json() = 0;
    virtual bool restore_from_json(const std::string& json) = 0;

    // ── Migration streaming helpers ────────────────────────────────────────────
    // List all rows from an arbitrary table (for migration).
    virtual std::vector<std::map<std::string, std::string>>
        list_migration_rows(const std::string& table,
                            const std::vector<std::string>& columns) = 0;
    // Row count for an arbitrary table. Returns -1 on read error.
    virtual int64_t count_migration_rows(const std::string& table) = 0;
    // Bulk insert rows (values are SQL-safe string representations).
    virtual bool bulk_insert_migration(const std::string& table,
                                       const std::vector<std::string>& columns,
                                       const std::vector<std::vector<std::string>>& values) = 0;
    // Get max primary-key/sequence value from a table.
    virtual int64_t get_max_pk(const std::string& table) = 0;
    // Execute a raw SQL statement (returns true on success).
    virtual bool execute_migration_sql(const std::string& sql) = 0;
    // Check whether a table exists.
    virtual bool table_exists(const std::string& table) = 0;
    // Force-expire stale locks older than max_age_seconds ago.
    virtual int expire_stale_locks(int64_t max_age_seconds) = 0;
    // Count currently held (active) locks on the source database.
    // Active = unreleased (new schema) or any row (legacy processing_locks).
    // Returns -1 on read error, 0 when no lock table exists.
    virtual int64_t count_active_locks() = 0;

    // ── Health ────────────────────────────────────────────────────────────────
    virtual HealthReport health_check() = 0;

    // ── Schema initialization ──────────────────────────────────────────────────
    // Create or upgrade database schema to the current version.
    // Returns true on success, false on failure.
    virtual bool init_schema() = 0;
};

} // namespace rawimport::domain
