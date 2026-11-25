/*
 * rawimport-pipeline - End-to-end test with .NRW fixtures
 * Validates the full import pipeline using real Nikon NRW files.
 *
 * This test requires:
 * - PostgreSQL or MariaDB database (provided by CI services)
 * - dnglab or Adobe DNG Converter binary in PATH
 * - Test suite files in /workspace/autodngconverter/testsuite/
 */

#include "utest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Domain types and ports
#include "domain/types.hpp"
#include "domain/ports/config_provider.hpp"
#include "domain/ports/import_store.hpp"
#include "domain/ports/converter_engine.hpp"
#include "domain/ports/exif_extractor.hpp"
#include "domain/ports/file_system.hpp"
#include "domain/ports/hash_service.hpp"
#include "domain/ports/log_sink.hpp"
#include "domain/ports/metrics_sink.hpp"

// Infrastructure adapters - include headers only, link libraries in CMake
#include "infrastructure/env_config_provider.hpp"
#include "infrastructure/std_filesystem.hpp"
#include "infrastructure/prometheus_metrics_sink.hpp"

#ifdef HAVE_OPENSSL
#include "infrastructure/openssl_hash_service.hpp"
#endif

#ifdef WITH_EXIFTOOL
#include "infrastructure/exiftool_daemon_exif_extractor.hpp"
#else
// No-op extractor for testing without exiftool
namespace rawimport::infrastructure {
struct NoopExifExtractor final : domain::ExifExtractor {
    domain::HealthReport health_check() override {
        return {domain::HealthStatus::Healthy, "No-op EXIF extractor", domain::now()};
    }
    std::optional<domain::ExifResult> extract(const domain::FilePath&) override {
        return std::nullopt;
    }
    bool is_available() const override { return false; }
};
}
#endif

#ifdef WITH_POSTGRESQL
#include "infrastructure/postgresql_import_store.hpp"
#endif

#ifdef WITH_MARIADB
#include "infrastructure/mariadb_import_store.hpp"
#endif

#if defined(WITH_DNGLAB) || defined(WITH_ADOBE_DNG)
#include "infrastructure/factories/converter_factory.hpp"
#endif

#ifdef WITH_SPDLOG
#include "infrastructure/spdlog_log_sink.hpp"
#else
// No-op log sink
namespace rawimport::infrastructure {
class NoopLogSink final : public domain::LogSink {
public:
    void LogStructured(std::string_view, std::string_view, const std::unordered_map<std::string, std::string>&) override {}
    void SetCorrelationId(std::string_view) override {}
    void ClearCorrelationId() override {}
    std::string correlation_id() override { return ""; }
    void trace(const std::string&, const std::map<std::string, std::string>& = {}) override {}
    void debug(const std::string&, const std::map<std::string, std::string>& = {}) override {}
    void info(const std::string&, const std::map<std::string, std::string>& = {}) override {}
    void warn(const std::string&, const std::map<std::string, std::string>& = {}) override {}
    void error(const std::string&, const std::map<std::string, std::string>& = {}) override {}
    void critical(const std::string&, const std::map<std::string, std::string>& = {}) override {}
    bool is_enabled(domain::LogLevel) override { return true; }
    domain::LogLevel minimum_level() override { return domain::LogLevel::Info; }
    void LogCollisionDuplicateHash(std::string_view, std::string_view, std::string_view, std::string_view) override {}
    void LogCollisionDuplicateHashDeadLetter(std::string_view, std::string_view) override {}
    void LogConversionFailedEngineError(std::string_view, std::string_view, int, std::string_view) override {}
    void LogImportFailed(std::string_view, std::string_view) override {}
    void LogCollisionDuplicateFound(std::string_view, std::string_view, const domain::SHA256Hash&, std::string_view) override {}
    void LogConversionFailedHashMismatch(std::string_view, std::string_view, std::string_view) override {}
    void LogConversionFailedTimeout(std::string_view, int) override {}
    void LogDeadLetterMaxRetries(std::string_view, int, std::string_view) override {}
    void LogWatchDirectoryInaccessible(std::string_view, int) override {}
    void LogWatchFilesystemFull(std::string_view) override {}
    void LogDbConnectionLost(int, int) override {}
    void LogDbDeadlock(std::string_view) override {}
    void LogDbQueryFailed(std::string_view, std::string_view) override {}
    void LogExifExtractionFailed(std::string_view, std::string_view, std::string_view, bool) override {}
    void LogExifToolDaemonRestarted(std::string_view, int) override {}
    void LogConverterEngineUnavailable(std::string_view, std::string_view) override {}
    void LogConverterUnsupportedFormat(std::string_view, std::string_view, std::string_view) override {}
    void LogFilesystemReadError(std::string_view, std::string_view, int, std::string_view) override {}
    void LogFilesystemWriteError(std::string_view, std::string_view, int, std::string_view) override {}
    void LogFilesystemArchiveFailed(std::string_view, std::string_view, std::string_view, std::string_view) override {}
    void LogHashVerificationFailed(std::string_view, std::string_view, std::string_view, std::string_view) override {}
    void LogReconversionLockFailed(std::string_view, std::string_view, int) override {}
    void LogReconversionSourceMissing(std::string_view, std::string_view) override {}
    void LogRotationPushFailed(std::string_view, std::string_view, std::string_view) override {}
    void LogConfigInvalidValue(std::string_view, std::string_view, std::string_view) override {}
    void LogWatchScanFailed(std::string_view, std::string_view) override {}
    void LogWatchStabilityTimeout(std::string_view, std::string_view, int) override {}
};
std::shared_ptr<domain::LogSink> make_spdlog_log_sink(std::string) { return std::make_shared<NoopLogSink>(); }
}
#endif

// Domain use cases
#include "../../src/domain/use_cases.cpp"

using namespace rawimport::domain;
using namespace rawimport::infrastructure;
using namespace rawimport::use_cases;

namespace fs = std::filesystem;

// Cross-platform temp directory helper
static fs::path make_temp_dir(const char* prefix) {
    auto base = fs::temp_directory_path();
    fs::path tmpdir = base / (std::string(prefix) + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directory(tmpdir);
    return tmpdir;
}

// Helper to get environment variable with default
static std::string get_env(const char* name, const std::string& def = "") {
    const char* val = std::getenv(name);
    return val ? val : def;
}

// Test configuration from environment (CI provides these)
struct E2EConfig {
    std::string db_driver;        // "postgresql" or "mariadb"
    std::string db_connection;    // Connection string
    std::string converter_engine; // "dnglab" or "adobe_dng"
    std::string testsuite_dir;    // Path to testsuite NRW files
    std::string output_dir;       // Output directory for DNGs
    std::string watch_dir;        // Watch/input directory
    std::string archive_dir;      // Archive directory
};

static E2EConfig load_e2e_config() {
    E2EConfig cfg;
    cfg.db_driver = get_env("E2E_DB_DRIVER", "postgresql");
    cfg.db_connection = get_env("E2E_DB_CONNECTION", "");
    cfg.converter_engine = get_env("E2E_CONVERTER_ENGINE", "dnglab");
    cfg.testsuite_dir = get_env("E2E_TESTSUITE_DIR", "/workspace/autodngconverter/testsuite");
    cfg.output_dir = get_env("E2E_OUTPUT_DIR", "");
    cfg.watch_dir = get_env("E2E_WATCH_DIR", "");
    cfg.archive_dir = get_env("E2E_ARCHIVE_DIR", "");
    return cfg;
}

// Custom config provider for e2e test that uses our E2EConfig
class E2EConfigProvider final : public ConfigProvider {
    E2EConfig cfg_;
    std::map<std::string, std::string> env_map_;

public:
    explicit E2EConfigProvider(const E2EConfig& cfg) : cfg_(cfg) {
        // Build environment map with RAVIMPORT_ prefix
        env_map_["RAWIMPORT_DB_DRIVER"] = cfg.db_driver;
        env_map_["RAWIMPORT_DB_CONNECTION"] = cfg.db_connection;
        env_map_["RAWIMPORT_DATABASE_URL"] = cfg.db_connection;
        env_map_["RAWIMPORT_CONVERTER_ENGINE"] = cfg.converter_engine;
        env_map_["RAWIMPORT_INPUT_DIR"] = cfg.watch_dir;
        env_map_["RAWIMPORT_OUTPUT_DIR"] = cfg.output_dir;
        env_map_["RAWIMPORT_ARCHIVE_DIR"] = cfg.archive_dir;
        env_map_["RAWIMPORT_FOLDER_SCHEMA"] = "%Y/%m";
        env_map_["RAWIMPORT_FILE_PATTERN"] = "IMG_{seq}";
        env_map_["RAWIMPORT_LOG_LEVEL"] = "debug";
        env_map_["RAWIMPORT_EXIFTOOL_MODE"] = "cli";
    }

    std::optional<std::string> get_string(const std::string& key) override {
        auto it = env_map_.find(key);
        if (it != env_map_.end()) return it->second;
        return std::nullopt;
    }

    std::optional<int> get_int(const std::string&) override { return std::nullopt; }
    std::optional<double> get_double(const std::string&) override { return std::nullopt; }
    std::optional<bool> get_bool(const std::string&) override { return std::nullopt; }

    ValidationReport validate(const std::vector<RequiredConfigKey>&) override {
        ValidationReport report;
        report.valid = !cfg_.db_connection.empty() && !cfg_.watch_dir.empty() && !cfg_.output_dir.empty();
        if (cfg_.db_connection.empty()) report.missing_keys.push_back("DB_CONNECTION");
        if (cfg_.watch_dir.empty()) report.missing_keys.push_back("INPUT_DIR");
        if (cfg_.output_dir.empty()) report.missing_keys.push_back("OUTPUT_DIR");
        return report;
    }

    std::map<std::string, std::string> to_map() override { return env_map_; }
};

// Test fixture for e2e tests
struct E2ETestFixture {
    fs::path temp_root_;
    fs::path watch_dir_;
    fs::path output_dir_;
    fs::path archive_dir_;
    E2EConfig cfg_;
    std::unique_ptr<ImportStore> store_;
    std::unique_ptr<ConverterEngine> engine_;
    std::unique_ptr<ExifExtractor> extractor_;
    std::shared_ptr<FileSystem> fs_;
    std::shared_ptr<HashService> hash_svc_;
    std::shared_ptr<LogSink> log_;
    std::shared_ptr<MetricsSink> metrics_;

    E2ETestFixture() {
        cfg_ = load_e2e_config();

        // Create temp directories
        temp_root_ = make_temp_dir("rawimport_e2e_");
        watch_dir_ = temp_root_ / "watch";
        output_dir_ = temp_root_ / "output";
        archive_dir_ = temp_root_ / "archive";
        fs::create_directories(watch_dir_);
        fs::create_directories(output_dir_);
        fs::create_directories(archive_dir_);

        // Override config with our temp directories
        cfg_.watch_dir = watch_dir_.string();
        cfg_.output_dir = output_dir_.string();
        cfg_.archive_dir = archive_dir_.string();

        UTEST_ASSERT(!cfg_.db_connection.empty());
        UTEST_ASSERT(fs::exists(cfg_.testsuite_dir));
    }

    ~E2ETestFixture() {
        // Cleanup temp directory on success (comment out for debugging)
        // fs::remove_all(temp_root_);
    }

    void setup_infrastructure() {
        // Config provider
        auto config = std::make_shared<E2EConfigProvider>(cfg_);

        // Log sink
        log_ = make_spdlog_log_sink("e2e-test");

        // Hash service
#ifdef HAVE_OPENSSL
        hash_svc_ = std::make_shared<OpenSSLHashService>();
#else
        UTEST_ASSERT(false); // OpenSSL required for e2e test
#endif

        // File system
        fs_ = std::make_shared<StdFileSystem>(*hash_svc_);

        // Import store (database)
        if (cfg_.db_driver == "postgresql") {
#ifdef WITH_POSTGRESQL
            store_ = std::make_unique<PostgresqlImportStore>(cfg_.db_connection);
#else
            UTEST_ASSERT(false); // PostgreSQL requested but WITH_POSTGRESQL not enabled
#endif
        } else {
#ifdef WITH_MARIADB
            store_ = std::make_unique<MariadbImportStore>(cfg_.db_connection);
#else
            UTEST_ASSERT(false); // MariaDB requested but WITH_MARIADB not enabled
#endif
        }

        // Initialize database schema
        auto health = store_->health_check();
        UTEST_ASSERT(health.status == HealthStatus::Healthy);
        UTEST_ASSERT(store_->init_schema());

        // Clear any existing test data to ensure test isolation
        UTEST_ASSERT(store_->truncate_test_data());

        // Converter engine
#if defined(WITH_DNGLAB) || defined(WITH_ADOBE_DNG)
        engine_ = MakeConverterEngine(*config, log_);
        auto engine_health = engine_->health_check();
        UTEST_ASSERT(engine_health.status != HealthStatus::Unhealthy);
#else
        UTEST_ASSERT(false);
#endif

        // Exif extractor
#ifdef WITH_EXIFTOOL
        extractor_ = std::make_unique<ExiftoolDaemonExifExtractor>();
#else
        extractor_ = std::make_unique<NoopExifExtractor>();
#endif

        // Metrics
        metrics_ = std::make_shared<PrometheusMetricsSink>();
    }

    // Copy testsuite NRW files to watch directory
    void copy_test_files() {
        int copied = 0;
        for (const auto& entry : fs::directory_iterator(cfg_.testsuite_dir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".nrw") {
                    fs::copy_file(entry.path(), watch_dir_ / entry.path().filename(),
                                  fs::copy_options::overwrite_existing);
                    copied++;
                }
            }
        }
        UTEST_ASSERT_EQ(copied, 20); // Expect exactly 20 NRW files
        log_->info("Copied " + std::to_string(copied) + " NRW files to watch directory");
    }

    // Run full import pipeline on all files in watch directory
    // Returns {success_count, failed_count} - caller verifies expectations
    std::pair<int, int> run_import_pipeline() {
        Import import_uc(*store_, *engine_, *extractor_, *fs_, *hash_svc_, *log_, *metrics_);
        Scan scan_uc(*store_, *fs_, *hash_svc_, *log_);

        auto extensions = engine_->supported_extensions();
        auto scan_result = scan_uc.execute(watch_dir_.string(), extensions);

        log_->info("Scan found " + std::to_string(scan_result.new_files.size()) + " new files");
        UTEST_ASSERT_EQ(scan_result.new_files.size(), 20);

        int success_count = 0;
        int failed_count = 0;

        for (const auto& fp : scan_result.new_files) {
            // Wait for file stability (size and mtime stop changing)
            auto prev_fp = fs_->compute_fast_fingerprint(fp);
            bool stable = false;
            for (int i = 0; i < 30; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                auto curr_fp = fs_->compute_fast_fingerprint(fp);
                if (curr_fp.size_bytes == prev_fp.size_bytes && curr_fp.mtime == prev_fp.mtime) {
                    stable = true;
                    break;
                }
                prev_fp = curr_fp;
            }
            UTEST_ASSERT(stable);

            // Import with auto-naming (output_dir ends with /)
            FilePath dest_dir{output_dir_.string() + "/"};
            ConversionSettings settings;
            auto outcome = import_uc.execute(fp, dest_dir, settings);

            if (outcome.success) {
                success_count++;
                log_->info("Imported: " + fp.native + " -> " + outcome.record.output_path.native);
            } else {
                failed_count++;
                log_->error("Import failed: " + fp.native + " - " + outcome.error);
            }
        }

        log_->info("Import complete: " + std::to_string(success_count) + " success, " +
                   std::to_string(failed_count) + " failed");
        return {success_count, failed_count};
    }

    // Verify all imports in database
    void verify_database_records() {
        auto all_imports = store_->list_imports(std::nullopt);
        UTEST_ASSERT_EQ(all_imports.size(), 20);

        for (const auto& rec : all_imports) {
            // Verify status is Completed
            UTEST_ASSERT(rec.status == ImportStatus::Completed);

            // Verify source_hash is set (non-zero)
            bool hash_zero = true;
            for (auto b : rec.source_hash) { if (b != 0) { hash_zero = false; break; } }
            UTEST_ASSERT(!hash_zero);

            // Verify output_hash is set
            hash_zero = true;
            for (auto b : rec.output_hash) { if (b != 0) { hash_zero = false; break; } }
            UTEST_ASSERT(!hash_zero);

            // Verify output_path exists and follows IMG_{seq} naming
            UTEST_ASSERT(fs::exists(rec.output_path.native));

            std::string filename = rec.output_path.filename();
            UTEST_ASSERT(filename.rfind("IMG_", 0) == 0);
            UTEST_ASSERT(filename.size() > 4 && filename.substr(filename.size() - 4) == ".dng");

            // Extract sequence number from filename (between "IMG_" and ".dng")
            UTEST_ASSERT(filename.size() >= 8u);
            std::string seq_str = filename.substr(4, filename.size() - 8); // Remove "IMG_" and ".dng"
            UTEST_ASSERT(!seq_str.empty());
            int64_t file_seq = std::stoll(seq_str);
            UTEST_ASSERT_EQ(file_seq, rec.sequence_id);

            // Verify source file hash matches
            int hash_errno = 0;
            auto source_hash_opt = fs_->sha256_file(rec.source_path, &hash_errno);
            UTEST_ASSERT(source_hash_opt.has_value());
            UTEST_ASSERT(*source_hash_opt == rec.source_hash);

            // Verify output file hash matches
            auto output_hash_opt = fs_->sha256_file(rec.output_path, &hash_errno);
            UTEST_ASSERT(output_hash_opt.has_value());
            UTEST_ASSERT(*output_hash_opt == rec.output_hash);
        }

        log_->info("All 20 database records verified successfully");
    }

    // Verify output files exist and are valid DNGs
    void verify_output_files() {
        int dng_count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(output_dir_)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".dng") {
                    dng_count++;
                    UTEST_ASSERT(fs::file_size(entry.path()) > 0);
                }
            }
        }
        UTEST_ASSERT_EQ(dng_count, 20);
        log_->info("Verified " + std::to_string(dng_count) + " DNG output files");
    }
};

// ─── E2E Test: Full pipeline with 20 NRW files ────────────────────────────────

TEST(e2e_nrw_full_pipeline) {
    E2ETestFixture fixture;
    fixture.setup_infrastructure();
    fixture.copy_test_files();
    auto [success, failed] = fixture.run_import_pipeline();
    UTEST_ASSERT_EQ(failed, 0);
    UTEST_ASSERT_EQ(success, 20);
    fixture.verify_database_records();
    fixture.verify_output_files();
}

// ─── E2E Test: Duplicate detection ────────────────────────────────────────────

TEST(e2e_nrw_duplicate_detection) {
    E2ETestFixture fixture;
    fixture.setup_infrastructure();
    fixture.copy_test_files();
    auto [success1, failed1] = fixture.run_import_pipeline();
    UTEST_ASSERT_EQ(failed1, 0);
    UTEST_ASSERT_EQ(success1, 20);

    // Run import pipeline again - should detect duplicates
    auto [success2, failed2] = fixture.run_import_pipeline(); // This should not create new records
    UTEST_ASSERT_EQ(success2, 0);
    UTEST_ASSERT_EQ(failed2, 20);

    // Verify still only 20 imports in database
    auto all_imports = fixture.store_->list_imports(std::nullopt);
    UTEST_ASSERT_EQ(all_imports.size(), 20);
}

// ─── E2E Test: Hash verification ──────────────────────────────────────────────

TEST(e2e_nrw_hash_verification) {
    E2ETestFixture fixture;
    fixture.setup_infrastructure();
    fixture.copy_test_files();
    auto [success, failed] = fixture.run_import_pipeline();
    UTEST_ASSERT_EQ(failed, 0);
    UTEST_ASSERT_EQ(success, 20);

    // Verify all source hashes are unique (no collisions in test set)
    auto all_imports = fixture.store_->list_imports(std::nullopt);
    std::set<std::string> seen_hashes;
    for (const auto& rec : all_imports) {
        std::string hash_hex = sha256_hex(rec.source_hash);
        UTEST_ASSERT(seen_hashes.insert(hash_hex).second);
    }
}

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}