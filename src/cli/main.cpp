/*
 * rawimport-pipeline - Application Entry Point (Hexagonal Architecture)
 * Composition root wiring domain ports to infrastructure adapters.
 */

#include <memory>
#include <string>
#include <iostream>
#include <stdexcept>

// Domain ports
#include "domain/ports/config_provider.hpp"
#include "domain/ports/import_store.hpp"
#include "domain/ports/converter_engine.hpp"
#include "domain/ports/exif_extractor.hpp"
#include "domain/ports/file_system.hpp"
#include "domain/ports/hash_service.hpp"
#include "domain/ports/log_sink.hpp"
#include "domain/ports/metrics_sink.hpp"
#include "domain/ports/rotation_sink.hpp"

// Domain use cases
#include "domain/use_cases.hpp"

// Infrastructure adapters (always available)
#include "infrastructure/env_config_provider.hpp"
#include "infrastructure/std_filesystem.hpp"
#include "infrastructure/prometheus_metrics_sink.hpp"

#ifdef HAVE_OPENSSL
#include "infrastructure/openssl_hash_service.hpp"
#else
// No-op hash service for environments without OpenSSL dev headers
namespace rawimport::infrastructure {
class NoopHashService final : public domain::HashService {
public:
    domain::SHA256Hash sha256_bytes(std::span<const uint8_t> data) override { return {}; }
    uint64_t fnv1a_64(std::span<const uint8_t> data) override { return 0; }
};
} // namespace rawimport::infrastructure
#endif

#ifdef WITH_SPDLOG
#include "infrastructure/spdlog_log_sink.hpp"
#else
// No-op log sink for environments without spdlog
namespace rawimport::infrastructure {
class NoopLogSink final : public domain::LogSink {
public:
    void LogStructured(std::string_view event, std::string_view level, const std::unordered_map<std::string, std::string>& fields) override {}
    void SetCorrelationId(std::string_view id) override {}
    void ClearCorrelationId() override {}
    std::string correlation_id() override { return ""; }
    void trace(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) override {}
    void debug(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) override {}
    void info(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) override {}
    void warn(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) override {}
    void error(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) override {}
    void critical(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) override {}
    bool is_enabled(domain::LogLevel level) override { return true; }
    domain::LogLevel minimum_level() override { return domain::LogLevel::Info; }
    void LogCollisionDuplicateHash(std::string_view, std::string_view, std::string_view, std::string_view) override {}
    void LogCollisionDuplicateHashDeadLetter(std::string_view, std::string_view) override {}
    void LogConversionFailedEngineError(std::string_view, std::string_view, int, std::string_view) override {}
    void LogConversionFailedHashMismatch(std::string_view, std::string_view, std::string_view) override {}
    void LogConversionFailedTimeout(std::string_view, int) override {}
    void LogDeadLetterMaxRetries(std::string_view, int, std::string_view) override {}
    void LogWatchDirectoryInaccessible(std::string_view, int) override {}
    void LogWatchFilesystemFull(std::string_view) override {}
    void LogDbConnectionLost(int, int) override {}
    void LogDbDeadlock(std::string_view) override {}
    void LogDbQueryFailed(std::string_view, std::string_view) override {}
};
std::shared_ptr<domain::LogSink> make_spdlog_log_sink(std::string logger_name) { return std::make_shared<NoopLogSink>(); }
} // namespace rawimport::infrastructure
#endif

// Conditional adapters
#ifdef WITH_MARIADB
#include "infrastructure/mariadb_import_store.hpp"
#endif
#ifdef WITH_POSTGRESQL
#include "infrastructure/postgresql_import_store.hpp"
#endif
#ifdef WITH_EXIFTOOL
#include "infrastructure/exiftool_exif_extractor.hpp"
#include "infrastructure/exiftool_daemon_exif_extractor.hpp"
#endif

#if defined(WITH_DNGLAB) || defined(WITH_ADOBE_DNG)
#include "infrastructure/factories/converter_factory.hpp"
#endif

// Migration CLI support
#ifdef WITH_MARIADB
#ifdef WITH_POSTGRESQL
#include "cli/migrate_command.hpp"
#include "infrastructure/migration_options.hpp"
#include "infrastructure/migration_runner.hpp"
#endif
#endif

namespace rawimport::cli {

using namespace rawimport::domain;
using namespace rawimport::infrastructure;
using namespace rawimport::use_cases;

int run_application(int argc, char** argv) {
    // ─── Load .env file if present ─────────────────────────────────────────────
    // Note: In production, use a proper dotenv library. Here we assume env vars
    // are already set (e.g., by systemd, docker, or shell).
    // load_dotenv(".env"); // Requires external lib; omitted for zero-dep build.

    // Early exit for help command
    if (argc < 2) {
        std::cout << "Usage: rawimport <command> [args...]\n";
        std::cout << "Commands: import, reconvert, scan, rotate, stats, config, migrate, watch\n";
        return 0;
    }

    std::string command = argv[1];

    // Commands that don't require full config validation (just show help)
    if (command == "help") {
        std::cout << "Usage: rawimport <command> [args...]\n";
        std::cout << "Commands: import, reconvert, scan, rotate, stats, config, migrate, watch\n";
        return 0;
    }

    // ─── Migrate Command (handles its own config via CLI args) ──────────────────
#ifdef HAVE_MIGRATION
    if (command == "migrate") {
        migration::MigrationOptions opts;
        std::string error_msg;

        // Shift argv so argv[1] is the command we already consumed
        // migrate_command expects args AFTER the subcommand
        for (int i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
        --argc;

        if (!parse_migrate_args(argc, argv, opts, error_msg)) {
            std::cerr << "Error: " << error_msg << "\n";
            print_migrate_usage();
            return 1;
        }

        if (opts.guide_mode) {
            auto result = run_guided_mode();
            if (result.status == migration::MigrationStatus::Success) {
                std::cout << "Guided migration setup completed.\n";
            } else {
                std::cerr << "Guided setup failed: " << result.message << "\n";
                return 1;
            }
            return 0;
        }

        if (static_cast<int>(opts.from) == static_cast<int>(opts.to)) {
            std::cerr << "Error: Source and target databases must differ.\n";
            return 1;
        }

        std::cout << "=== rawimport migrate ===\n";
        std::cout << "Running migration from "
                  << (opts.from == migration::SourceDB::MariaDB ? "MariaDB" : "PostgreSQL")
                  << " to "
                  << (opts.to == migration::TargetDB::PostgreSQL ? "PostgreSQL" : "MariaDB") << "\n";
        std::cout << "Source DSN: " << opts.source_dsn << "\n";
        std::cout << "Target DSN: " << opts.target_dsn << "\n";
        std::cout << "Dry run: " << (opts.dry_run ? "yes" : "no") << "\n";
        std::cout << "Batch size: " << opts.batch_size << "\n";

        migration::MigrationRunner runner(opts);
        auto result = runner.run();
        if (result.status == migration::MigrationStatus::Success) {
            std::cout << result.message << "\n";
            return 0;
        } else {
            std::cerr << "Migration failed: " << result.message << "\n";
            for (const auto& w : result.warnings) {
                std::cerr << "Warning: " << w << "\n";
            }
            return 1;
        }
    }
#endif

    // ─── Config Provider ───────────────────────────────────────────────────────
    auto config = std::make_shared<EnvConfigProvider>("RAWIMPORT_");

    // Validate required config (domain-level rules: at least one of DB_CONNECTION/DATABASE_URL).
    auto validate_result = domain::validate_config(config->to_map());
    if (!validate_result.valid) {
        std::cerr << "Config validation failed. Missing keys: ";
        for (const auto& k : validate_result.missing_keys) {
            std::cerr << k << " ";
        }
        std::cerr << "\n";
        return 1;
    }

    // ─── Log Sink ──────────────────────────────────────────────────────────────
    auto log = make_spdlog_log_sink("rawimport-cli");
    auto log_level_str = config->get_string("LOG_LEVEL").value_or("info");
    log->info("Starting rawimport CLI", {{"log_level", log_level_str}});

    // ─── Hash Service ──────────────────────────────────────────────────────────
    std::shared_ptr<domain::HashService> hash_svc;
#ifdef HAVE_OPENSSL
    hash_svc = std::make_shared<OpenSSLHashService>();
#else
    log->error("OpenSSL required for hash service; configure WITH_OPENSSL or install libssl-dev");
    return 1;
#endif

    // ─── File System ───────────────────────────────────────────────────────────
    StdFileSystem fs(*hash_svc);

    // ─── Import Store (selected by DB_DRIVER) ──────────────────────────────────
    std::string db_driver = config->get_string("DB_DRIVER").value_or("postgresql");
    std::string db_connection = config->get_string("DB_CONNECTION").value_or("");
    std::string database_url = config->get_string("DATABASE_URL").value_or("");
    std::string conn_str = !db_connection.empty() ? db_connection : database_url;

    if (conn_str.empty()) {
        log->error("Database connection string not configured (RAWIMPORT_DB_CONNECTION or RAWIMPORT_DATABASE_URL)");
        return 1;
    }

    std::unique_ptr<ImportStore> store;
    if (db_driver == "postgresql") {
#ifdef WITH_POSTGRESQL
        log->info("Using PostgreSQL ImportStore", {{"driver", db_driver}});
        store = std::make_unique<PostgresqlImportStore>(conn_str);
#else
        log->error("PostgreSQL requested but WITH_POSTGRESQL not enabled at compile time");
        return 1;
#endif
    } else {
#ifdef WITH_MARIADB
        log->info("Using MariaDB ImportStore", {{"driver", db_driver}});
        store = std::make_unique<MariadbImportStore>(conn_str);
#else
        log->error("MariaDB requested but WITH_MARIADB not enabled at compile time");
        return 1;
#endif
    }

    // Connect and initialize schema
    auto health = store->health_check();
    if (health.status == HealthStatus::Unhealthy) {
        log->error("Database health check failed: " + health.message);
        return 1;
    }
    log->info("Database connected and healthy");

    // Initialize schema
    if (!store->init_schema()) {
        log->error("Failed to initialize database schema");
        return 1;
    }
    log->info("Database schema initialized");

    // ─── Converter Engine (via factory) ────────────────────────────────────────
    std::unique_ptr<ConverterEngine> engine;
#if defined(WITH_DNGLAB) || defined(WITH_ADOBE_DNG)
    try {
        engine = MakeConverterEngine(*config, log);
        log->info("Converter engine created", {{"engine", engine->health_check().message}});
    } catch (const std::exception& e) {
        log->error("Failed to create converter engine: " + std::string(e.what()));
        return 1;
    }
#else
    log->error("No converter engine available: neither WITH_DNGLAB nor WITH_ADOBE_DNG enabled");
    return 1;
#endif

    // ─── Exif Extractor ────────────────────────────────────────────────────────
    std::unique_ptr<ExifExtractor> extractor;
#ifdef WITH_EXIFTOOL
    std::string exiftool_mode = config->get_string("EXIFTOOL_MODE").value_or("daemon");
    if (exiftool_mode == "daemon") {
        log->info("Using ExifTool daemon extractor");
        extractor = std::make_unique<ExiftoolDaemonExifExtractor>();
    } else {
        log->info("Using ExifTool CLI extractor");
        extractor = std::make_unique<ExiftoolExifExtractor>();
    }
#else
    // No-op extractor for testing without exiftool
    struct NoopExifExtractor : ExifExtractor {
        HealthReport health_check() override {
            return {HealthStatus::Healthy, "No-op EXIF extractor", now()};
        }
        std::optional<ExifResult> extract(const FilePath&) override {
            return std::nullopt;
        }
    };
    extractor = std::make_unique<NoopExifExtractor>();
    log->warn("WITH_EXIFTOOL not enabled - using no-op ExifExtractor");
#endif

    // ─── Metrics Sink ──────────────────────────────────────────────────────────
    auto metrics = std::make_shared<PrometheusMetricsSink>();

    // ─── Rotation Sink (optional, Immich) ──────────────────────────────────────
    std::unique_ptr<RotationSink> rotation_sink;
    std::string immich_url = config->get_string("IMMICH_BASE_URL").value_or("");
    std::string immich_key = config->get_string("IMMICH_API_KEY").value_or("");
    if (!immich_url.empty() && !immich_key.empty()) {
#ifdef WITH_IMMICH
        log->info("Configuring Immich RotationSink", {{"base_url", immich_url}});
        rotation_sink = std::make_unique<ImmichRotationSink>(immich_url, immich_key);
        auto rot_health = rotation_sink->health_check();
        if (rot_health.status == HealthStatus::Unhealthy) {
            log->warn("Immich rotation sink health check failed: " + rot_health.message);
        }
#else
        log->warn("Immich configured but WITH_IMMICH not enabled at compile time");
#endif
    }

    // ─── Wire Use Cases ────────────────────────────────────────────────────────
    Import import_uc(*store, *engine, *extractor, fs, *hash_svc, *log, *metrics);
    Reconversion reconversion_uc(*store, *engine, fs, *log);
    Scan scan_uc(*store, fs, *hash_svc, *log);
    Stats stats_uc(*store);
    ConfigValidate config_uc(*config);

    // ─── Command Dispatch ──────────────────────────────────────────────────────

    // ─── Watch Command (service mode) ──────────────────────────────────────────
    if (command == "watch") {
        std::string watch_dir = config->get_string("INPUT_DIR").value_or("");
        if (watch_dir.empty()) {
            log->error("INPUT_DIR not configured for watch mode");
            return 1;
        }
        log->info("Starting watch mode", {{"watch_dir", watch_dir}});
        // In production, this would run a filesystem watcher loop.
        // For now, we run a single scan as a demo.
        auto extensions = engine->supported_extensions();
        auto result = scan_uc.execute(watch_dir, extensions);
        log->info("Watch scan complete",
                  {{"new_files", std::to_string(result.new_files.size())},
                   {"unchanged", std::to_string(result.unchanged_files.size())}});
        return 0;
    }

    try {
        if (command == "import") {
            if (argc < 4) {
                log->error("Usage: rawimport import <source> <dest> [--linear] [--lossy] [--compression N]");
                return 1;
            }
            FilePath source{argv[2]};
            FilePath dest{argv[3]};
            ConversionSettings settings;
            for (int i = 4; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--linear") settings.linear_dng = true;
                else if (arg == "--lossy") settings.lossy = true;
                else if (arg == "--compression" && i + 1 < argc) settings.compression = std::stoi(argv[++i]);
            }
            auto outcome = import_uc.execute(source, dest, settings);
            if (outcome.success) {
                log->info("Import succeeded", {{"output", outcome.record.output_path.native}});
                return 0;
            } else {
                log->error("Import failed: " + outcome.error);
                return 1;
            }
        }
        else if (command == "reconvert") {
            if (argc < 5) {
                log->error("Usage: rawimport reconvert <import_id> <source_dng> <output_dng> [--linear] [--lossy] [--compression N]");
                return 1;
            }
            int64_t import_id = std::stoll(argv[2]);
            FilePath source{argv[3]};
            FilePath dest{argv[4]};
            ConversionSettings settings;
            for (int i = 5; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--linear") settings.linear_dng = true;
                else if (arg == "--lossy") settings.lossy = true;
                else if (arg == "--compression" && i + 1 < argc) settings.compression = std::stoi(argv[++i]);
            }
            auto outcome = reconversion_uc.execute(import_id, source, dest, settings);
            if (outcome.success) {
                log->info("Reconversion succeeded", {{"output", outcome.record.output_dng_path.native}});
                return 0;
            } else {
                log->error("Reconversion failed: " + outcome.error);
                return 1;
            }
        }
        else if (command == "scan") {
            if (argc < 3) {
                log->error("Usage: rawimport scan <input_dir>");
                return 1;
            }
            std::string input_dir = argv[2];
            auto extensions = engine->supported_extensions();
            auto result = scan_uc.execute(input_dir, extensions);
            log->info("Scan complete",
                      {{"new_files", std::to_string(result.new_files.size())},
                       {"unchanged", std::to_string(result.unchanged_files.size())}});
            for (const auto& f : result.new_files) {
                std::cout << "NEW: " << f.native << "\n";
            }
            return 0;
        }
        else if (command == "rotate") {
            if (argc < 4) {
                log->error("Usage: rawimport rotate <import_id> <orientation>");
                return 1;
            }
            if (!rotation_sink) {
                log->error("Rotation sink not configured (IMMICH_BASE_URL and IMMICH_API_KEY required)");
                return 1;
            }
            Rotation rotation_uc(*store, *rotation_sink, *log);
            int64_t import_id = std::stoll(argv[2]);
            int orientation = std::stoi(argv[3]);
            auto outcome = rotation_uc.execute(import_id, orientation);
            if (outcome.success) {
                log->info("Rotation applied", {{"orientation", std::to_string(outcome.applied_orientation)}});
                return 0;
            } else {
                log->error("Rotation failed: " + outcome.error);
                return 1;
            }
        }
        else if (command == "stats") {
            auto snap = stats_uc.compute();
            std::cout << "Total imports: " << snap.total_imports << "\n";
            std::cout << "Completed: " << snap.completed_imports << "\n";
            std::cout << "Failed: " << snap.failed_imports << "\n";
            std::cout << "Active locks: " << snap.active_locks << "\n";
            std::cout << "Alerts: " << snap.alerts_count << "\n";
            return 0;
        }
        else if (command == "config") {
            auto report = config_uc.execute();
            std::cout << "Config valid: " << (report.valid ? "yes" : "no") << "\n";
            if (!report.missing_keys.empty()) {
                std::cout << "Missing keys:\n";
                for (const auto& k : report.missing_keys) std::cout << "  - " << k << "\n";
            }
            auto cfg_map = config->to_map();
            for (const auto& [k, v] : cfg_map) {
                std::cout << k << "=" << v << "\n";
            }
            return report.valid ? 0 : 1;
        }
        else {
            log->error("Unknown command: " + command);
            return 1;
        }
    } catch (const std::exception& e) {
        log->error("Unhandled exception: " + std::string(e.what()));
        return 1;
    }
}

} // namespace rawimport::cli

int main(int argc, char** argv) {
    return rawimport::cli::run_application(argc, argv);
}
