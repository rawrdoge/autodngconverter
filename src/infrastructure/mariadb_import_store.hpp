/*
 * rawimport-pipeline - MariaDB ImportStore adapter
 * Requires libmariadb-dev / mariadb-connector-c. Guarded by WITH_MARIADB.
 */
#pragma once

#ifdef WITH_MARIADB

#include <string>
#include <vector>
#include <optional>
#include <memory>

#include "domain/ports/import_store.hpp"

// Opaque MariaDB connection handle.
struct st_mariadb;
using MariadbHandle = st_mariadb*;

namespace rawimport::infrastructure {

class MariadbImportStore final : public domain::ImportStore {
public:
    // connection_string: "host=X;user=Y;password=Z;database=D" (semicolon-separated).
    explicit MariadbImportStore(std::string connection_string);
    ~MariadbImportStore() override;

    // ── imports ───────────────────────────────────────────────────────────────
    std::optional<int64_t>   create_import(const domain::ImportRecord& rec) override;
    std::optional<domain::ImportRecord> get_import(int64_t id) override;
    bool                     update_import(domain::ImportRecord rec) override;
    std::vector<domain::ImportRecord> list_imports(std::optional<domain::ImportStatus> status_filter) override;

    // ── reconversions ─────────────────────────────────────────────────────────
    std::optional<int64_t>     create_reconversion(const domain::ReconversionRecord& rec) override;
    std::optional<domain::ReconversionRecord> get_reconversion(int64_t id) override;
    bool                       update_reconversion(domain::ReconversionRecord rec) override;
    std::vector<domain::ReconversionRecord> list_reconversions(int64_t import_id) override;

    // ── preview edits ─────────────────────────────────────────────────────────
    std::optional<int64_t>   create_preview_edit(const domain::PreviewEdit& edit) override;
    std::optional<domain::PreviewEdit> get_preview_edit(int64_t id) override;
    bool                     delete_preview_edit(int64_t id) override;
    std::vector<domain::PreviewEdit> list_preview_edits(int64_t import_id) override;

    // ── alerts ────────────────────────────────────────────────────────────────
    std::optional<int64_t>   create_alert(const domain::Alert& alert) override;
    std::optional<domain::Alert> get_alert(int64_t id) override;
    bool                     delete_alert(int64_t id) override;
    std::vector<domain::Alert> list_alerts(std::optional<int64_t> import_id) override;

    // ── locks ─────────────────────────────────────────────────────────────────
    std::optional<int64_t> acquire_lock(int64_t import_id, const std::string& holder) override;
    bool                   release_lock(int64_t lock_id) override;
    bool                   is_locked(int64_t import_id) override;
    std::vector<domain::Lock> list_active_locks() override;

    // ── sequences ─────────────────────────────────────────────────────────────
    int64_t next_sequence_id() override;

    // ── migration ─────────────────────────────────────────────────────────────
    std::string snapshot_to_json() override;
    bool        restore_from_json(const std::string& json) override;

    // ── migration streaming helpers ───────────────────────────────────────────
    std::vector<std::map<std::string, std::string>>
        list_migration_rows(const std::string& table,
                            const std::vector<std::string>& columns) override;
    int64_t count_migration_rows(const std::string& table) override;
    bool bulk_insert_migration(const std::string& table,
                               const std::vector<std::string>& columns,
                               const std::vector<std::vector<std::string>>& values) override;
    int64_t get_max_pk(const std::string& table) override;
    bool execute_migration_sql(const std::string& sql) override;
    bool table_exists(const std::string& table) override;
    int expire_stale_locks(int64_t max_age_seconds) override;

    // ── health ────────────────────────────────────────────────────────────────
    domain::HealthReport health_check() override;

    // ── schema initialization ──────────────────────────────────────────────────
    bool init_schema() override;

private:
    std::string connection_string_;
    MariadbHandle connect_() const;
    void drop_(MariadbHandle conn) const;

    // Helper: escape string for SQL.
    static std::string sql_escape(const std::string& s);
};

} // namespace rawimport::infrastructure

#endif // WITH_MARIADB
