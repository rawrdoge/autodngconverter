/*
 * rawimport-pipeline - CLI migrate command implementation
 * rawimport migrate --from mariadb --to postgresql --source-dsn ... --target-dsn ...
 */
#pragma once

#ifdef WITH_MARIADB
#ifdef WITH_POSTGRESQL

#include <string>
#include <vector>
#include <optional>
#include <iostream>
#include <sstream>
#include <map>

#include "infrastructure/migration_options.hpp"
#include "infrastructure/migration_runner.hpp"

namespace rawimport::cli {

// Parse CLI arguments into MigrationOptions.
// Returns true on success, false on parse error (with error_msg set).
bool parse_migrate_args(int argc, char* argv[], migration::MigrationOptions& opts,
                        std::string& error_msg);

// Run guided mode: interactive prompts for source/target config.
migration::MigrationResult run_guided_mode();

// Print usage/help for migrate command.
void print_migrate_usage();

} // namespace rawimport::cli

#endif // WITH_POSTGRESQL
#endif // WITH_MARIADB
