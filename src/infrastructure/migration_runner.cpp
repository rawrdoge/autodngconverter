/*
 * rawimport-pipeline - Streaming migration runner implementation
 * Reads from source ImportStore, writes to target ImportStore row-by-row
 * with explicit sequence preservation (no resequencing).
 */
#ifdef WITH_MARIADB
#ifdef WITH_POSTGRESQL

#include "infrastructure/migration_runner.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>
#include <sstream>

#include "infrastructure/mariadb_import_store.hpp"
#include "infrastructure/postgresql_import_store.hpp"

namespace rawimport::migration {

namespace {

// Table definitions with columns in order matching source schema.
struct TableDef {
    std::string name;
    std::vector<std::string> columns;
    bool has_fulltext = false;
};

const std::vector<TableDef> kTables = {
    {"sequences", {"id", "name", "created_at"}},
    {"imports", {"id", "sequence_id", "source_path", "source_hash", "output_path", "output_hash",
                 "camera_model", "capture_date", "capture_time", "date_source", "folder_schema",
                 "conversion_settings", "status", "created_at", "completed_at", "error_message",
                 "last_preview_edit_at", "orientation"}},
    {"reconversions", {"id", "import_id", "previous_output_hash", "new_output_hash",
                       "conversion_settings", "reason", "triggered_at", "completed_at",
                       "status", "error_message"}},
    {"preview_edits", {"id", "import_id", "worker", "previous_output_hash", "new_output_hash",
                       "preview_width", "preview_height", "preview_quality", "edited_at"}},
    {"alerts", {"id", "severity", "category", "message", "ref_sequence", "created_at", "acknowledged"}},
    {"locks", {"id", "import_id", "holder", "acquired_at", "expires_at", "released_at"}}, // skipped
};

const std::string kSchemaMigrationsTable = "schema_migrations";

// Progress bar helper
void print_progress(const std::string& label, int64_t current, int64_t total) {
    if (total == 0) return;
    const int width = 40;
    double ratio = static_cast<double>(current) / total;
    int filled = static_cast<int>(ratio * width);
    std::cout << "\r" << label << " [";
    for (int i = 0; i < width; ++i) std::cout << (i < filled ? "=" : " ");
    std::cout << "] " << std::fixed << std::setprecision(1) << (ratio * 100.0) << "% "
              << current << "/" << total;
    std::cout.flush();
}

} // anon namespace

MigrationRunner::MigrationRunner(MigrationOptions opts) : opts_(std::move(opts)) {}

MigrationResult MigrationRunner::run() {
    MigrationResult result;
    auto start = std::chrono::steady_clock::now();

    // 1. Connect both
    auto r = connect_both_();
    if (r.status != MigrationStatus::Success) return r;

    // 2. Create target schema (run migrations up to current version)
    r = create_target_schema_();
    if (r.status != MigrationStatus::Success) return r;

    // 3. Verify schema versions match (check schema_migrations table)
    // Skipped for now — assume both at same version.

    // 4. Drain processing locks (wait up to 5 min, then force-expire)
    r = drain_locks_();
    if (r.status != MigrationStatus::Success) return r;

    // 5. Verify target is empty (unless --force)
    r = verify_not_empty_();
    if (r.status != MigrationStatus::Success) return r;

    // In dry-run mode, all subsequent steps return SkippedDryRun.
    // We treat this as success for the overall migration.
    bool is_dry_run = opts_.dry_run;

    // 6. Stream sequences (CRITICAL: preserve exact seq values)
    r = stream_sequences_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // 7. Stream imports
    r = stream_imports_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // 8. Stream reconversions
    r = stream_reconversions_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // 9. Stream preview_edits
    r = stream_preview_edits_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // 10. Stream alerts
    r = stream_alerts_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // 11. Reset target sequences: setval('sequences_id_seq', max_seq + 1)
    r = reset_target_sequences_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // 12. Verify counts match
    r = verify_counts_();
    if (!is_dry_run && r.status != MigrationStatus::Success) return r;
    if (is_dry_run && r.status == MigrationStatus::SkippedDryRun) r.status = MigrationStatus::Success;

    // Success
    auto end = std::chrono::steady_clock::now();
    result.stats.elapsed_seconds =
        std::chrono::duration<double>(end - start).count();
    result.status = MigrationStatus::Success;
    result.message = "Migration completed successfully in " +
                     std::to_string(result.stats.elapsed_seconds) + "s";

    return result;
}

MigrationResult MigrationRunner::connect_both_() {
    MigrationResult result;

    // Source
    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    // Target
    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    // Test connections
    auto src_health = source->health_check();
    if (src_health.status != domain::HealthStatus::Healthy) {
        set_failed_(result, MigrationStatus::FailedConnectSource,
                    "Source DB health check failed: " + src_health.message);
        return result;
    }

    auto tgt_health = target->health_check();
    if (tgt_health.status != domain::HealthStatus::Healthy) {
        set_failed_(result, MigrationStatus::FailedConnectTarget,
                    "Target DB health check failed: " + tgt_health.message);
        return result;
    }

    // Store for later use (we'll recreate connections per-table for simplicity)
    // In production, we'd hold these, but the adapters create new connections per call.
    return result; // Success
}

MigrationResult MigrationRunner::create_target_schema_() {
    MigrationResult result;

    // Run schema creation SQL on target
    // In production, this would read from a migration SQL file.
    // For now, we assume the target schema already exists (created by rawimport init).

    // Check that tables exist on target
    // Create a temp target store for checking
    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    for (const auto& t : kTables) {
        if (t.name == "locks") continue; // skipped
        if (!target->table_exists(t.name)) {
            set_failed_(result, MigrationStatus::FailedSchemaCreate,
                        "Target table '" + t.name + "' does not exist. Run migrations first.");
            return result;
        }
    }

    return result; // Success
}

MigrationResult MigrationRunner::drain_locks_() {
    MigrationResult result;

    // Dry-run mode must not touch the source — report only.
    if (opts_.dry_run) {
        result.warnings.push_back(
            "Dry run: would check for active locks on source, wait up to 300s for drain, "
            "then force-expire stale locks if any remain.");
        return result;
    }

    // Create source store
    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    // Count actual active locks: unreleased rows on the new schema, or any row
    // on legacy processing_locks. Returns -1 on read error.
    int64_t active = source->count_active_locks();
    if (active < 0) {
        set_failed_(result, MigrationStatus::FailedDrainLocks,
                    "Failed to count active locks on source database (read error)");
        return result;
    }
    if (active == 0) return result;

    result.warnings.push_back(
        "Found " + std::to_string(active) + " active lock(s). Waiting for drain...");

    const int max_wait_seconds = 300;
    const int poll_interval = 5;
    for (int waited = 0; waited < max_wait_seconds; waited += poll_interval) {
        std::this_thread::sleep_for(std::chrono::seconds(poll_interval));
        active = source->count_active_locks();
        if (active < 0) {
            set_failed_(result, MigrationStatus::FailedDrainLocks,
                        "Failed to count active locks during drain wait (read error)");
            return result;
        }
        if (active == 0) break;
    }

    if (active > 0) {
        int expired = source->expire_stale_locks(0);
        result.warnings.push_back(
            "Force-expired " + std::to_string(expired) + " stale lock(s) after " +
            std::to_string(max_wait_seconds) + "s timeout.");
    }

    int64_t remaining = source->count_active_locks();
    if (remaining < 0) {
        set_failed_(result, MigrationStatus::FailedDrainLocks,
                    "Failed to count active locks after drain (read error)");
        return result;
    }
    if (remaining > 0) {
        set_failed_(result, MigrationStatus::FailedDrainLocks,
                    "Remaining active locks after timeout: " + std::to_string(remaining));
        return result;
    }

    return result;
}

MigrationResult MigrationRunner::verify_not_empty_() {
    if (opts_.force) return MigrationResult{};
    // In dry-run mode, we skip target-empty check
    if (opts_.dry_run) return MigrationResult{};

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    // Check if target has any data in sequences table
    int64_t seq_count = target->count_migration_rows("sequences");
    if (seq_count > 0) {
        set_failed_(result, MigrationStatus::TargetNotEmpty,
                    "Target database is not empty (sequences table has " +
                    std::to_string(seq_count) + " rows). Use --force to override.");
        return result;
    }

    return result;
}

MigrationResult MigrationRunner::stream_sequences_() {
    if (opts_.dry_run) {
        MigrationResult r;
        r.status = MigrationStatus::SkippedDryRun;
        return r;
    }

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    const TableDef& t = kTables[0]; // sequences
    int64_t total = source->count_migration_rows(t.name);
    if (total < 0) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to count rows in source table '" + t.name + "' (read error)");
        return result;
    }
    if (total == 0) {
        record_diff_(t.name, 0, 0, false, "");
        return result;
    }

    auto rows = source->list_migration_rows(t.name, t.columns);
    if (static_cast<int64_t>(rows.size()) != total) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to read rows from source table '" + t.name +
                    "': got " + std::to_string(rows.size()) + " rows but expected " +
                    std::to_string(total));
        return result;
    }

    int64_t max_id = -1;

    // Batch insert
    std::vector<std::vector<std::string>> batch;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> vals;
        for (const auto& col : t.columns) {
            vals.push_back(rows[i].at(col));
        }
        batch.push_back(std::move(vals));

        // Track max id (the 'id' column — sequence names are arbitrary).
        int64_t id_val = std::atoll(rows[i].at("id").c_str());
        if (id_val > max_id) max_id = id_val;

        if (batch.size() >= static_cast<size_t>(opts_.batch_size) || i == rows.size() - 1) {
            if (!target->bulk_insert_migration(t.name, t.columns, batch)) {
                set_failed_(result, MigrationStatus::FailedSequencePreserve,
                            "Failed to bulk insert batch into sequences");
                return result;
            }
            result.stats.sequences_inserted += batch.size();
            print_progress("sequences", result.stats.sequences_inserted, total);
            batch.clear();
        }
    }
    std::cout << std::endl;

    record_diff_(t.name, total, result.stats.sequences_inserted, false,
                 max_id >= 0 ? "max id=" + std::to_string(max_id) : "");
    return result;
}

MigrationResult MigrationRunner::stream_imports_() {
    if (opts_.dry_run) {
        MigrationResult r;
        r.status = MigrationStatus::SkippedDryRun;
        return r;
    }

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    const TableDef& t = kTables[1]; // imports
    int64_t total = source->count_migration_rows(t.name);
    if (total < 0) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to count rows in source table '" + t.name + "' (read error)");
        return result;
    }
    if (total == 0) {
        record_diff_(t.name, 0, 0, false, "");
        return result;
    }

    auto rows = source->list_migration_rows(t.name, t.columns);
    if (static_cast<int64_t>(rows.size()) != total) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to read rows from source table '" + t.name +
                    "': got " + std::to_string(rows.size()) + " rows but expected " +
                    std::to_string(total));
        return result;
    }

    std::vector<std::vector<std::string>> batch;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> vals;
        for (const auto& col : t.columns) {
            vals.push_back(rows[i].at(col));
        }
        batch.push_back(std::move(vals));

        if (batch.size() >= static_cast<size_t>(opts_.batch_size) || i == rows.size() - 1) {
            if (!target->bulk_insert_migration(t.name, t.columns, batch)) {
                set_failed_(result, MigrationStatus::FailedCountVerify,
                            "Failed to bulk insert batch into imports");
                return result;
            }
            result.stats.imports_inserted += batch.size();
            print_progress("imports", result.stats.imports_inserted, total);
            batch.clear();
        }
    }
    std::cout << std::endl;

    record_diff_(t.name, total, result.stats.imports_inserted, false, "");
    return result;
}

MigrationResult MigrationRunner::stream_reconversions_() {
    if (opts_.dry_run) {
        MigrationResult r;
        r.status = MigrationStatus::SkippedDryRun;
        return r;
    }

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    const TableDef& t = kTables[2]; // reconversions
    int64_t total = source->count_migration_rows(t.name);
    if (total < 0) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to count rows in source table '" + t.name + "' (read error)");
        return result;
    }
    if (total == 0) {
        record_diff_(t.name, 0, 0, false, "");
        return result;
    }

    auto rows = source->list_migration_rows(t.name, t.columns);
    if (static_cast<int64_t>(rows.size()) != total) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to read rows from source table '" + t.name +
                    "': got " + std::to_string(rows.size()) + " rows but expected " +
                    std::to_string(total));
        return result;
    }

    std::vector<std::vector<std::string>> batch;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> vals;
        for (const auto& col : t.columns) {
            vals.push_back(rows[i].at(col));
        }
        batch.push_back(std::move(vals));

        if (batch.size() >= static_cast<size_t>(opts_.batch_size) || i == rows.size() - 1) {
            if (!target->bulk_insert_migration(t.name, t.columns, batch)) {
                set_failed_(result, MigrationStatus::FailedCountVerify,
                            "Failed to bulk insert batch into reconversions");
                return result;
            }
            result.stats.reconversions_inserted += batch.size();
            print_progress("reconversions", result.stats.reconversions_inserted, total);
            batch.clear();
        }
    }
    std::cout << std::endl;

    record_diff_(t.name, total, result.stats.reconversions_inserted, false, "");
    return result;
}

MigrationResult MigrationRunner::stream_preview_edits_() {
    if (opts_.dry_run) {
        MigrationResult r;
        r.status = MigrationStatus::SkippedDryRun;
        return r;
    }

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    const TableDef& t = kTables[3]; // preview_edits
    int64_t total = source->count_migration_rows(t.name);
    if (total < 0) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to count rows in source table '" + t.name + "' (read error)");
        return result;
    }
    if (total == 0) {
        record_diff_(t.name, 0, 0, false, "");
        return result;
    }

    auto rows = source->list_migration_rows(t.name, t.columns);
    if (static_cast<int64_t>(rows.size()) != total) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to read rows from source table '" + t.name +
                    "': got " + std::to_string(rows.size()) + " rows but expected " +
                    std::to_string(total));
        return result;
    }

    std::vector<std::vector<std::string>> batch;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> vals;
        for (const auto& col : t.columns) {
            vals.push_back(rows[i].at(col));
        }
        batch.push_back(std::move(vals));

        if (batch.size() >= static_cast<size_t>(opts_.batch_size) || i == rows.size() - 1) {
            if (!target->bulk_insert_migration(t.name, t.columns, batch)) {
                set_failed_(result, MigrationStatus::FailedCountVerify,
                            "Failed to bulk insert batch into preview_edits");
                return result;
            }
            result.stats.preview_edits_inserted += batch.size();
            print_progress("preview_edits", result.stats.preview_edits_inserted, total);
            batch.clear();
        }
    }
    std::cout << std::endl;

    record_diff_(t.name, total, result.stats.preview_edits_inserted, false, "");
    return result;
}

MigrationResult MigrationRunner::stream_alerts_() {
    if (opts_.dry_run) {
        MigrationResult r;
        r.status = MigrationStatus::SkippedDryRun;
        return r;
    }

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    const TableDef& t = kTables[4]; // alerts
    int64_t total = source->count_migration_rows(t.name);
    if (total < 0) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to count rows in source table '" + t.name + "' (read error)");
        return result;
    }
    if (total == 0) {
        record_diff_(t.name, 0, 0, false, "");
        return result;
    }

    auto rows = source->list_migration_rows(t.name, t.columns);
    if (static_cast<int64_t>(rows.size()) != total) {
        set_failed_(result, MigrationStatus::FailedReadSource,
                    "Failed to read rows from source table '" + t.name +
                    "': got " + std::to_string(rows.size()) + " rows but expected " +
                    std::to_string(total));
        return result;
    }

    std::vector<std::vector<std::string>> batch;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> vals;
        for (const auto& col : t.columns) {
            vals.push_back(rows[i].at(col));
        }
        batch.push_back(std::move(vals));

        if (batch.size() >= static_cast<size_t>(opts_.batch_size) || i == rows.size() - 1) {
            if (!target->bulk_insert_migration(t.name, t.columns, batch)) {
                set_failed_(result, MigrationStatus::FailedCountVerify,
                            "Failed to bulk insert batch into alerts");
                return result;
            }
            result.stats.alerts_inserted += batch.size();
            print_progress("alerts", result.stats.alerts_inserted, total);
            batch.clear();
        }
    }
    std::cout << std::endl;

    record_diff_(t.name, total, result.stats.alerts_inserted, false, "");
    return result;
}

MigrationResult MigrationRunner::reset_target_sequences_() {
    if (opts_.dry_run) return MigrationResult{};
    if (opts_.to != TargetDB::PostgreSQL) return MigrationResult{}; // MariaDB uses auto-increment

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> target;
    target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);

    // Get max seq from target sequences table
    int64_t max_seq = target->get_max_pk("sequences");
    if (max_seq <= 0) {
        // No sequences migrated, nothing to reset
        return result;
    }

    // Reset the sequence: setval('sequences_id_seq', max_seq)
    // Note: the sequence is on the 'id' column, but seq values are independent.
    // We need to ensure next auto-id doesn't conflict.
    std::ostringstream sql;
    sql << "SELECT setval('sequences_id_seq', " << max_seq << ")";

    if (!target->execute_migration_sql(sql.str())) {
        result.warnings.push_back("Failed to reset sequences_id_seq; manual reset may be needed");
    }

    return result;
}

MigrationResult MigrationRunner::verify_counts_() {
    if (!opts_.verify_counts) return MigrationResult{};
    if (opts_.dry_run) return MigrationResult{};

    MigrationResult result;

    std::unique_ptr<domain::ImportStore> source;
    if (opts_.from == SourceDB::MariaDB) {
        source = std::make_unique<infrastructure::MariadbImportStore>(opts_.source_dsn);
    } else {
        source = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.source_dsn);
    }

    std::unique_ptr<domain::ImportStore> target;
    if (opts_.to == TargetDB::PostgreSQL) {
        target = std::make_unique<infrastructure::PostgresqlImportStore>(opts_.target_dsn);
    } else {
        target = std::make_unique<infrastructure::MariadbImportStore>(opts_.target_dsn);
    }

    for (const auto& t : kTables) {
        if (t.name == "locks") continue;

        int64_t src_count = source->count_migration_rows(t.name);
        int64_t tgt_count = target->count_migration_rows(t.name);

        if (src_count < 0 || tgt_count < 0) {
            set_failed_(result, MigrationStatus::FailedCountVerify,
                        "Failed to count rows in verification of table '" + t.name + "' (read error)");
            return result;
        }
        if (src_count != tgt_count) {
            set_failed_(result, MigrationStatus::FailedCountVerify,
                        "Count mismatch on table '" + t.name + "': source=" +
                        std::to_string(src_count) + ", target=" +
                        std::to_string(tgt_count));
            return result;
        }
    }

    // Verify sequence IDs match between source and target (preserve gaps).
    auto src_seq = source->list_migration_rows("sequences", {"id"});
    std::vector<int64_t> src_ids, tgt_ids;
    for (const auto& row : src_seq)
        src_ids.push_back(std::atoll(row.at("id").c_str()));
    std::sort(src_ids.begin(), src_ids.end());
    auto seq_rows = target->list_migration_rows("sequences", {"id"});
    for (const auto& row : seq_rows)
        tgt_ids.push_back(std::atoll(row.at("id").c_str()));
    std::sort(tgt_ids.begin(), tgt_ids.end());
    if (src_ids != tgt_ids) {
        set_failed_(result, MigrationStatus::FailedSequencePreserve,
                    "Sequence ID mismatch between source and target");
        return result;
    }

    return result;
}

void MigrationRunner::set_failed_(MigrationResult& r, MigrationStatus s, const std::string& msg) {
    r.status = s;
    r.message = msg;
}

int64_t MigrationRunner::read_seq_max_from_source_() {
    // Not used currently — computed during stream_sequences_.
    return -1;
}

void MigrationRunner::record_diff_(const std::string& table, int64_t src_count,
                                   int64_t tgt_count, bool fulltext_skip,
                                   const std::string& notes) {
    // The stats diff vector is populated elsewhere; this is a placeholder.
}

bool MigrationRunner::table_exists(domain::ImportStore& store, const std::string& table_name) {
    return store.table_exists(table_name);
}

int64_t MigrationRunner::count_rows(domain::ImportStore& store, const std::string& table_name) {
    return store.count_migration_rows(table_name);
}

int64_t MigrationRunner::get_max_sequence(domain::ImportStore& store) {
    return store.get_max_pk("sequences");
}

} // namespace rawimport::migration

#endif // WITH_POSTGRESQL
#endif // WITH_MARIADB