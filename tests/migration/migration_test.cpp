/*
 * rawimport-pipeline - Migration runner unit tests
 */
#include "utest.h"
#include "infrastructure/migration_options.hpp"
#include "cli/migrate_command.hpp"

using namespace rawimport::migration;

TEST(migration_options_defaults) {
    MigrationOptions opts;
    UTEST_ASSERT(opts.from == SourceDB::MariaDB);
    UTEST_ASSERT(opts.to == TargetDB::MariaDB); // Fixed: default is MariaDB
    UTEST_ASSERT(opts.dry_run == false);
    UTEST_ASSERT_EQ(opts.batch_size, 5000);
    UTEST_ASSERT(opts.verify_counts == false);
    UTEST_ASSERT(opts.guide_mode == false);
}

TEST(migration_options_custom) {
    MigrationOptions opts;
    opts.from = SourceDB::PostgreSQL;
    opts.to = TargetDB::MariaDB;
    opts.dry_run = true;
    opts.batch_size = 1000;
    opts.verify_counts = true;
    opts.guide_mode = true;
    opts.force = true;
    opts.source_dsn = "host=localhost;database=rawimport";
    opts.target_dsn = "host=localhost;database=rawimport_pg";

    UTEST_ASSERT(opts.from == SourceDB::PostgreSQL);
    UTEST_ASSERT(opts.to == TargetDB::MariaDB);
    UTEST_ASSERT(opts.dry_run == true);
    UTEST_ASSERT_EQ(opts.batch_size, 1000);
    UTEST_ASSERT(opts.verify_counts == true);
    UTEST_ASSERT(opts.guide_mode == true);
    UTEST_ASSERT(opts.force == true);
    UTEST_ASSERT(!opts.source_dsn.empty());
    UTEST_ASSERT(!opts.target_dsn.empty());
}

#ifdef WITH_MARIADB
#ifdef WITH_POSTGRESQL
TEST(migration_runner_init) {
    MigrationOptions opts;
    opts.source_dsn = "host=localhost;database=test";
    opts.target_dsn = "host=localhost;database=test_pg";
    MigrationRunner runner(opts);
    // Verify it instantiates without error
    UTEST_ASSERT(true);
}
#endif
#endif

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    utest_register(&utest_reg_migration_options_defaults);
    utest_register(&utest_reg_migration_options_custom);
#ifdef WITH_MARIADB
#ifdef WITH_POSTGRESQL
    utest_register(&utest_reg_migration_runner_init);
#endif
#endif
    return utest_main();
}
