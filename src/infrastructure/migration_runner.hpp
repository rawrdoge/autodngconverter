/*
 * rawimport-pipeline - Streaming migration runner
 * Reads from source ImportStore, writes to target ImportStore row-by-row
 * with explicit sequence preservation (no resequencing).
 */
#pragma once

#ifdef WITH_MARIADB
#ifdef WITH_POSTGRESQL

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <chrono>

#include "domain/ports/import_store.hpp"
#include "migration_options.hpp"

namespace rawimport::migration {

class MigrationRunner {
public:
    explicit MigrationRunner(MigrationOptions opts);
    ~MigrationRunner() = default;

    // Run the full migration pipeline. Returns result (never throws).
    MigrationResult run();

    // Check whether a table exists in the target database.
    static bool table_exists(domain::ImportStore& store, const std::string& table_name);

    // Get row count for a table.
    static int64_t count_rows(domain::ImportStore& store, const std::string& table_name);

    // Get max seq value from source sequences table.
    static int64_t get_max_sequence(domain::ImportStore& store);

private:
    MigrationOptions opts_;

    // ── step functions ────────────────────────────────────────────────────────
    MigrationResult connect_both_();
    MigrationResult create_target_schema_();
    MigrationResult drain_locks_();
    MigrationResult verify_not_empty_();
    MigrationResult stream_sequences_();
    MigrationResult stream_imports_();
    MigrationResult stream_reconversions_();
    MigrationResult stream_preview_edits_();
    MigrationResult stream_alerts_();
    MigrationResult verify_counts_();
    MigrationResult reset_target_sequences_();

    // ── helpers ───────────────────────────────────────────────────────────────
    void  set_failed_(MigrationResult& r, MigrationStatus s, const std::string& msg);
    int64_t read_seq_max_from_source_();
    void  record_diff_(const std::string& table, int64_t src_count, int64_t tgt_count,
                       bool fulltext_skip, const std::string& notes);
};

} // namespace rawimport::migration

#endif // WITH_POSTGRESQL
#endif // WITH_MARIADB
