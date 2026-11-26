/*
 * rawimport-pipeline - Upgrade-path integration tests
 * Requirements: live database containers + BUILD_INTEGRATION_TESTS=ON at configure time.
 * Env vars: RAWIMPORT_TEST_MARIADB_CONN, RAWIMPORT_TEST_POSTGRESQL_CONN
 * Purpose: Prove init_schema() upgrades legacy schemas without regressing new-data paths.
 */

#include "utest.h"
#include "domain/ports/import_store.hpp"
#include "domain/types.hpp"

#ifdef WITH_MARIADB
#include "infrastructure/mariadb_import_store.hpp"
#endif
#ifdef WITH_POSTGRESQL
#include "infrastructure/postgresql_import_store.hpp"
#endif

#include <cstdlib>
#include <cstdint>
#include <string>
#include <iostream>

using namespace rawimport::domain;
using namespace rawimport::infrastructure;

static std::string get_env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// ---------------------------------------------------------------------------
// Legacy-schema bootstrappers
// Both recreate the exact shapes present in migrations/0001_init.sql before
// the fd3eddb upgrade changes landed (missing next_val, started_at, UNIQUE).
// ---------------------------------------------------------------------------

#ifdef WITH_MARIADB
static void setup_legacy_mariadb(MariadbImportStore& store) {
    store.execute_migration_sql("DROP TABLE IF EXISTS preview_edits");
    store.execute_migration_sql("DROP TABLE IF EXISTS reconversions");
    store.execute_migration_sql("DROP TABLE IF EXISTS alerts");
    store.execute_migration_sql("DROP TABLE IF EXISTS locks");
    store.execute_migration_sql("DROP TABLE IF EXISTS imports");
    store.execute_migration_sql("DROP TABLE IF EXISTS sequences");

    // Legacy sequences: only `name`; no seq_name, no next_val
    bool ok = store.execute_migration_sql(
        "CREATE TABLE sequences ("
        "  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "  name VARCHAR(32) NOT NULL UNIQUE,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB");
    UTEST_ASSERT(ok);

    // Legacy imports: no started_at/completed_at; FK to sequences(id) present
    ok = store.execute_migration_sql(
        "CREATE TABLE imports ("
        "  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "  sequence_id BIGINT UNSIGNED NOT NULL,"
        "  source_path TEXT NOT NULL,"
        "  source_hash CHAR(64) NOT NULL,"
        "  output_path TEXT NOT NULL,"
        "  output_hash CHAR(64) NOT NULL,"
        "  status VARCHAR(20) NOT NULL,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY (sequence_id) REFERENCES sequences(id)"
        ") ENGINE=InnoDB");
    UTEST_ASSERT(ok);

    ok = store.execute_migration_sql("INSERT INTO sequences (name) VALUES ('import')");
    UTEST_ASSERT(ok);

    // Four rows with legacy numeric statuses '0'..'3' and sequence_ids 1..4
    for (int i = 1; i <= 4; ++i) {
        std::string sql = "INSERT INTO imports "
                          "(sequence_id, source_path, source_hash, output_path, status, created_at) "
                          "VALUES (" + std::to_string(i) +
                          ", '/legacy/source" + std::to_string(i) + ".cr3', "
                          "'hash" + std::to_string(i) + "abcdef0123456789abcdef01234567', "
                          "'/legacy/output" + std::to_string(i) + ".dng', '" +
                          std::string(1, '0' + i) + "', NOW())";
        ok = store.execute_migration_sql(sql);
        UTEST_ASSERT(ok);
    }
}
#endif

#ifdef WITH_POSTGRESQL
static void setup_legacy_postgresql(PostgresqlImportStore& store) {
    store.execute_migration_sql("DROP TABLE IF EXISTS preview_edits CASCADE");
    store.execute_migration_sql("DROP TABLE IF EXISTS reconversions CASCADE");
    store.execute_migration_sql("DROP TABLE IF EXISTS alerts CASCADE");
    store.execute_migration_sql("DROP TABLE IF EXISTS locks CASCADE");
    store.execute_migration_sql("DROP TABLE IF EXISTS imports CASCADE");
    store.execute_migration_sql("DROP TABLE IF EXISTS sequences CASCADE");
    store.execute_migration_sql("DROP SEQUENCE IF EXISTS imports_sequence_id_seq");

    // Legacy sequences: only `name`; no seq_name/next_val
    bool ok = store.execute_migration_sql(
        "CREATE TABLE sequences ("
        "  id BIGSERIAL PRIMARY KEY,"
        "  name VARCHAR(32) NOT NULL UNIQUE,"
        "  created_at TIMESTAMPTZ DEFAULT NOW()"
        ")");
    UTEST_ASSERT(ok);

    // Legacy imports: no started_at/completed_at
    ok = store.execute_migration_sql(
        "CREATE TABLE imports ("
        "  id BIGSERIAL PRIMARY KEY,"
        "  sequence_id BIGINT NOT NULL,"
        "  source_path TEXT NOT NULL,"
        "  source_hash CHAR(64) NOT NULL,"
        "  output_path TEXT NOT NULL,"
        "  output_hash CHAR(64) NOT NULL,"
        "  status VARCHAR(20) NOT NULL,"
        "  created_at TIMESTAMPTZ DEFAULT NOW()"
        ")");
    UTEST_ASSERT(ok);

    ok = store.execute_migration_sql("INSERT INTO sequences (name) VALUES ('import')");
    UTEST_ASSERT(ok);

    for (int i = 1; i <= 4; ++i) {
        std::string sql = "INSERT INTO imports "
                          "(sequence_id, source_path, source_hash, output_path, status, created_at) "
                          "VALUES (" + std::to_string(i) +
                          ", '/legacy/source" + std::to_string(i) + ".cr3', "
                          "'hash" + std::to_string(i) + "abcdef0123456789abcdef01234567', "
                          "'/legacy/output" + std::to_string(i) + ".dng', '" +
                          std::string(1, '0' + i) + "', NOW())";
        ok = store.execute_migration_sql(sql);
        UTEST_ASSERT(ok);
    }
}
#endif

// ===========================================================================
// MariaDB upgrade-path assertions
// ===========================================================================
#ifdef WITH_MARIADB

// Assertion #1: init_schema() on a legacy-schema DB succeeds and is idempotent.
TEST(mariadb_upgrade_init_schema_idempotent) {
    std::string conn = get_env("RAWIMPORT_TEST_MARIADB_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_MARIADB_CONN not set\n"; return; }
    MariadbImportStore store(conn);
    setup_legacy_mariadb(store);

    bool ok = store.init_schema();
    UTEST_ASSERT(ok);
    ok = store.init_schema();          // second run must also succeed (idempotent)
    UTEST_ASSERT(ok);
}

// Assertion #2: create_import() works on an upgraded DB (was null + "Unknown column 'next_val'").
TEST(mariadb_upgrade_create_import_on_upgraded_db) {
    std::string conn = get_env("RAWIMPORT_TEST_MARIADB_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_MARIADB_CONN not set\n"; return; }
    MariadbImportStore store(conn);
    setup_legacy_mariadb(store);
    UTEST_ASSERT(store.init_schema());

    ImportRecord rec;
    rec.source_path.native = "/upgraded/source.cr3";
    rec.output_path.native = "/upgraded/output.dng";
    rec.status = ImportStatus::Completed;
    for (int i = 0; i < 32; ++i) rec.source_hash[i] = static_cast<uint8_t>(i + 10);
    rec.output_hash.fill(0);
    rec.created_at = std::chrono::system_clock::now();

    auto id_opt = store.create_import(rec);
    UTEST_ASSERT(id_opt.has_value());

    auto got = store.get_import(*id_opt);
    UTEST_ASSERT(got.has_value());
    UTEST_ASSERT(got->status == ImportStatus::Completed);
    UTEST_ASSERT(got->sequence_id > 4);   // historical max was 4
}

// Assertion #5: legacy numeric status '2' reads as Completed after upgrade.
TEST(mariadb_upgrade_legacy_status_reads_as_completed) {
    std::string conn = get_env("RAWIMPORT_TEST_MARIADB_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_MARIADB_CONN not set\n"; return; }
    MariadbImportStore store(conn);
    setup_legacy_mariadb(store);
    UTEST_ASSERT(store.init_schema());

    auto all = store.list_imports(std::nullopt);
    UTEST_ASSERT_EQ(all.size(), 4u);
    int completed_count = 0;
    for (const auto& r : all) {
        if (r.status == ImportStatus::Completed) ++completed_count;
    }
    UTEST_ASSERT_EQ(completed_count, 1);   // exactly one row had status '2'
}

// Assertion #6: duplicate-detection via find_import_by_source_hash works on upgraded DB.
TEST(mariadb_upgrade_duplicate_detection) {
    std::string conn = get_env("RAWIMPORT_TEST_MARIADB_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_MARIADB_CONN not set\n"; return; }
    MariadbImportStore store(conn);
    setup_legacy_mariadb(store);
    UTEST_ASSERT(store.init_schema());

    SHA256Hash hash{};
    for (int i = 0; i < 32; ++i) hash[i] = static_cast<uint8_t>(i + 42);

    ImportRecord rec;
    rec.source_path.native = "/dup/source.cr3";
    rec.output_path.native = "/dup/output.dng";
    rec.status = ImportStatus::Queued;
    rec.source_hash = hash;
    rec.output_hash.fill(0);
    rec.created_at = std::chrono::system_clock::now();

    auto id_opt = store.create_import(rec);
    UTEST_ASSERT(id_opt.has_value());

    // Re-querying by same hash must return the existing record, not a new one
    auto found = store.find_import_by_source_hash(hash);
    UTEST_ASSERT(found.has_value());
    UTEST_ASSERT_EQ(found->sequence_id, *id_opt);
}
#endif // WITH_MARIADB

// ===========================================================================
// PostgreSQL upgrade-path assertions
// ===========================================================================
#ifdef WITH_POSTGRESQL

// Assertion #3: next sequence_id strictly exceeds MAX(historical sequence_id).
TEST(postgresql_upgrade_init_schema) {
    std::string conn = get_env("RAWIMPORT_TEST_POSTGRESQL_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_POSTGRESQL_CONN not set\n"; return; }
    PostgresqlImportStore store(conn);
    setup_legacy_postgresql(store);

    bool ok = store.init_schema();
    UTEST_ASSERT(ok);

    int64_t next_id = store.next_sequence_id();
    UTEST_ASSERT(next_id > 4);   // historical max was 4
}

// Assertion #4: UNIQUE violation actually fires on duplicate sequence_id.
TEST(postgresql_upgrade_unique_constraint_fires) {
    std::string conn = get_env("RAWIMPORT_TEST_POSTGRESQL_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_POSTGRESQL_CONN not set\n"; return; }
    PostgresqlImportStore store(conn);
    setup_legacy_postgresql(store);
    UTEST_ASSERT(store.init_schema());

    // Insert a row with sequence_id=2 which already exists (from legacy seed).
    // The UNIQUE constraint added by init_schema() should reject this.
    bool ok = store.execute_migration_sql(
        "INSERT INTO imports "
        "(sequence_id, source_path, source_hash, output_path, output_hash, status, created_at) "
        "VALUES (2, '/dup-crash', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', "
        "         '/dup-out', 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb', "
        "         'pending', NOW())");
    UTEST_ASSERT(!ok);   // must fail due to uq_imports_sequence_id
}

// Assertion #5: legacy numeric status '2' reads as Completed after upgrade.
TEST(postgresql_upgrade_legacy_status_reads_as_completed) {
    std::string conn = get_env("RAWIMPORT_TEST_POSTGRESQL_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_POSTGRESQL_CONN not set\n"; return; }
    PostgresqlImportStore store(conn);
    setup_legacy_postgresql(store);
    UTEST_ASSERT(store.init_schema());

    auto all = store.list_imports(std::nullopt);
    UTEST_ASSERT_EQ(all.size(), 4u);
    int completed_count = 0;
    for (const auto& r : all) {
        if (r.status == ImportStatus::Completed) ++completed_count;
    }
    UTEST_ASSERT_EQ(completed_count, 1);   // exactly one row had status '2'
}

// Assertion #6: duplicate-detection via find_import_by_source_hash works on upgraded DB.
TEST(postgresql_upgrade_duplicate_detection) {
    std::string conn = get_env("RAWIMPORT_TEST_POSTGRESQL_CONN");
    if (conn.empty()) { std::cerr << "SKIP: RAWIMPORT_TEST_POSTGRESQL_CONN not set\n"; return; }
    PostgresqlImportStore store(conn);
    setup_legacy_postgresql(store);
    UTEST_ASSERT(store.init_schema());

    SHA256Hash hash{};
    for (int i = 0; i < 32; ++i) hash[i] = static_cast<uint8_t>(i + 42);

    ImportRecord rec;
    rec.source_path.native = "/dup/pg-source.cr3";
    rec.output_path.native = "/dup/pg-output.dng";
    rec.status = ImportStatus::Queued;
    rec.source_hash = hash;
    rec.output_hash.fill(0);
    rec.created_at = std::chrono::system_clock::now();

    auto id_opt = store.create_import(rec);
    UTEST_ASSERT(id_opt.has_value());

    auto found = store.find_import_by_source_hash(hash);
    UTEST_ASSERT(found.has_value());
    UTEST_ASSERT_EQ(found->sequence_id, *id_opt);
}

#endif // WITH_POSTGRESQL

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}
