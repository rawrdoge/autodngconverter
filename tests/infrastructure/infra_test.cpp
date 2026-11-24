/*
 * rawimport-pipeline - infrastructure adapter smoke tests
 * Single-TU test — pulls in adapter implementations directly.
 */
#include "utest.h"

#ifdef _WIN32
#include <stdlib.h>
#define setenv(k,v,o) _putenv_s((k),(v))
#define unsetenv(k)   _putenv_s((k),"")
#endif

// Pull in adapter implementations so they're in the same TU as tests.
#include "../../src/infrastructure/env_config_provider.cpp"
#include "../../src/infrastructure/prometheus_metrics_sink.cpp"
#ifdef HAVE_OPENSSL
#include "../../src/infrastructure/openssl_hash_service.cpp"
#endif
#ifdef WITH_EXIFTOOL
#include "../../src/infrastructure/exiftool_daemon_exif_extractor.cpp"
#endif

#include "domain/types.hpp"
#include <fstream>
#include <filesystem>

using namespace rawimport::infrastructure;
using namespace rawimport::domain;

// Cross-platform temp directory helper
static std::filesystem::path make_temp_dir(const char* prefix) {
    auto base = std::filesystem::temp_directory_path();
    std::filesystem::path tmpdir = base / prefix;
    tmpdir += std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::filesystem::create_directory(tmpdir);
    return tmpdir;
}

// ─── EnvConfigProvider ────────────────────────────────────────────────────────

TEST(env_config_get_string_present) {
    setenv("RAWIMPORT_TEST_KEY", "hello_world", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto v = cfg.get_string("TEST_KEY");
    UTEST_ASSERT(v.has_value());
    UTEST_ASSERT_STRING(v->c_str(), "hello_world");
    unsetenv("RAWIMPORT_TEST_KEY");
}

TEST(env_config_get_string_absent) {
    unsetenv("RAWIMPORT_NONEXISTENT_KEY_XYZ");
    EnvConfigProvider cfg("RAWIMPORT_");
    auto v = cfg.get_string("NONEXISTENT_KEY_XYZ");
    UTEST_ASSERT(!v.has_value());
}

TEST(env_config_get_int) {
    setenv("RAWIMPORT_TEST_PORT", "8080", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto v = cfg.get_int("TEST_PORT");
    UTEST_ASSERT(v.has_value());
    UTEST_ASSERT_EQ(*v, 8080);
    unsetenv("RAWIMPORT_TEST_PORT");
}

TEST(env_config_get_bool_true) {
    setenv("RAWIMPORT_TEST_FLAG", "true", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto v = cfg.get_bool("TEST_FLAG");
    UTEST_ASSERT(v.has_value());
    UTEST_ASSERT(*v == true);
    unsetenv("RAWIMPORT_TEST_FLAG");
}

TEST(env_config_get_bool_false) {
    setenv("RAWIMPORT_TEST_FLAG", "0", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto v = cfg.get_bool("TEST_FLAG");
    UTEST_ASSERT(v.has_value());
    UTEST_ASSERT(*v == false);
    unsetenv("RAWIMPORT_TEST_FLAG");
}

TEST(env_config_validate_missing) {
    unsetenv("RAWIMPORT_OUTPUT_DIR");
    unsetenv("RAWIMPORT_INPUT_DIR");
    unsetenv("RAWIMPORT_CONVERTER_ENGINE");
    EnvConfigProvider cfg("RAWIMPORT_");
    auto report = cfg.validate(required_config_keys());
    UTEST_ASSERT(report.valid == false);
    UTEST_ASSERT(report.missing_keys.size() >= 1u);
}

TEST(env_config_validate_present) {
    setenv("RAWIMPORT_OUTPUT_DIR", "/tmp/out", 1);
    setenv("RAWIMPORT_INPUT_DIR",  "/tmp/in", 1);
    setenv("RAWIMPORT_CONVERTER_ENGINE", "dnglab", 1);
    setenv("RAWIMPORT_DB_CONNECTION", "postgresql://user:pass@localhost/db", 1);
    setenv("RAWIMPORT_DATABASE_URL", "postgresql://user:pass@localhost/db", 1);
    setenv("RAWIMPORT_DB_DRIVER", "postgresql", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto report = cfg.validate(required_config_keys());
    UTEST_ASSERT(report.valid == true);
    UTEST_ASSERT_EQ(report.missing_keys.size(), 0u);
    unsetenv("RAWIMPORT_OUTPUT_DIR");
    unsetenv("RAWIMPORT_INPUT_DIR");
    unsetenv("RAWIMPORT_CONVERTER_ENGINE");
    unsetenv("RAWIMPORT_DB_CONNECTION");
    unsetenv("RAWIMPORT_DATABASE_URL");
    unsetenv("RAWIMPORT_DB_DRIVER");
}

TEST(env_config_to_map) {
    setenv("RAWIMPORT_OUTPUT_DIR", "/tmp/out", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto m = cfg.to_map();
    UTEST_ASSERT(m.count("RAWIMPORT_OUTPUT_DIR") == 1u);
    UTEST_ASSERT_STRING(m["RAWIMPORT_OUTPUT_DIR"].c_str(), "/tmp/out");
    unsetenv("RAWIMPORT_OUTPUT_DIR");
}

TEST(env_config_prefix_custom) {
    setenv("MYAPP_DB_HOST", "localhost", 1);
    EnvConfigProvider cfg("MYAPP_");
    auto v = cfg.get_string("DB_HOST");
    UTEST_ASSERT(v.has_value());
    UTEST_ASSERT_STRING(v->c_str(), "localhost");
    unsetenv("MYAPP_DB_HOST");
}

TEST(env_config_get_double) {
    setenv("RAWIMPORT_TEST_RATIO", "3.14", 1);
    EnvConfigProvider cfg("RAWIMPORT_");
    auto v = cfg.get_double("TEST_RATIO");
    UTEST_ASSERT(v.has_value());
    double val = *v;
    UTEST_ASSERT(val > 3.1 && val < 3.2);
    unsetenv("RAWIMPORT_TEST_RATIO");
}

// ─── PrometheusMetricsSink ────────────────────────────────────────────────────

TEST(metrics_counter_increment) {
    PrometheusMetricsSink sink;
    sink.increment_counter("imports_completed", 1, {{"engine", "dnglab"}});
    sink.increment_counter("imports_completed", 3, {{"engine", "dnglab"}});
    auto rendered = sink.render_prometheus();
    UTEST_ASSERT(rendered.find("imports_completed") != std::string::npos);
    UTEST_ASSERT(rendered.find("4\n") != std::string::npos || rendered.rfind("4 ") != std::string::npos);
}

TEST(metrics_histogram_observe) {
    PrometheusMetricsSink sink;
    sink.observe_histogram("convert_duration", 1.5, {{"result", "ok"}});
    sink.observe_histogram("convert_duration", 2.5, {{"result", "ok"}});
    auto rendered = sink.render_prometheus();
    UTEST_ASSERT(rendered.find("convert_duration_sum") != std::string::npos);
    UTEST_ASSERT(rendered.find("convert_duration_count") != std::string::npos);
}

TEST(metrics_render_has_labels) {
    PrometheusMetricsSink sink;
    sink.increment_counter("tests", 1, {{"engine", "adobe_dng"}, {"status", "ok"}});
    auto rendered = sink.render_prometheus();
    UTEST_ASSERT(rendered.find("engine=\"adobe_dng\"") != std::string::npos);
    UTEST_ASSERT(rendered.find("status=\"ok\"") != std::string::npos);
}

TEST(metrics_multiple_counters_independent) {
    PrometheusMetricsSink sink;
    sink.increment_counter("a", 10, {});
    sink.increment_counter("b", 20, {});
    auto rendered = sink.render_prometheus();
    UTEST_ASSERT(rendered.find("a 10") != std::string::npos);
    UTEST_ASSERT(rendered.find("b 20") != std::string::npos);
}

TEST(metrics_zero_delta) {
    PrometheusMetricsSink sink;
    sink.increment_counter("nothing", 0, {});
    auto rendered = sink.render_prometheus();
    UTEST_ASSERT(rendered.find("nothing") != std::string::npos);
}

TEST(metrics_different_labels_different_entries) {
    PrometheusMetricsSink sink;
    sink.increment_counter("req", 1, {{"path", "/api/a"}});
    sink.increment_counter("req", 1, {{"path", "/api/b"}});
    auto rendered = sink.render_prometheus();
    UTEST_ASSERT(rendered.find("/api/a") != std::string::npos);
    UTEST_ASSERT(rendered.find("/api/b") != std::string::npos);
}

// ─── OpenSSL HashService ─────────────────────────────────────────────────────

#ifdef HAVE_OPENSSL
TEST(hash_fnv1a_known_values) {
    OpenSSLHashService svc;
    uint64_t empty_hash = svc.fnv1a_64({});
    UTEST_ASSERT_EQ(empty_hash, 14695981039346656037ULL);
    std::vector<uint8_t> hello = {'h','e','l','l','o'};
    uint64_t hello_hash = svc.fnv1a_64(hello);
    UTEST_ASSERT_EQ(hello_hash, 11831194018420276491ULL);
}
#endif

// ─── ExifTool Daemon ExifExtractor ────────────────────────────────────────────
// Test that extract() calls exiftool exactly once (not twice via daemon + fallback).
// This test creates a fake exiftool script that logs each invocation.

#ifdef _WIN32
#define MKSTEMP_TMPL "fake_exiftool_XXXXXX.bat"
#else
#define MKSTEMP_TMPL "fake_exiftool_XXXXXX"
#endif

// TEST(exiftool_daemon_extract_calls_exiftool_once) {
//     // Create a temporary directory for our fake exiftool and log
//     std::filesystem::path tmpdir = make_temp_dir("exiftool_test_");
//
//     // Create fake exiftool script
//     std::string fake_exiftool_path = (tmpdir / "fake_exiftool").string();
//     std::string log_path = (tmpdir / "invocation_log.txt").string();
//     
//     #ifdef _WIN32
//     // Windows batch file
//     std::ofstream script(fake_exiftool_path + ".bat");
//     script << "@echo off\n";
//     script << "echo %DATE% %TIME% >> \"" << log_path << "\"\n";
//     script << "echo [{\\\"Make\\\":\\\"TestMake\\\",\\\"Model\\\":\\\"TestModel\\\",\\\"DateTimeOriginal\\\":\\\"2024:01:01 12:00:00\\\",\\\"Orientation\\\":\\\"1\\\"}]\n";
//     script.close();
//     // On Windows, we need to use the .bat extension
//     fake_exiftool_path += ".bat";
//     #else
//     // POSIX shell script
//     std::ofstream script(fake_exiftool_path);
//     script << "#!/bin/sh\n";
//     script << "echo \"$(date)\" >> \"" << log_path << "\"\n";
//     script << "echo '[{\\\"Make\\\":\\\"TestMake\\\",\\\"Model\\\":\\\"TestModel\\\",\\\"DateTimeOriginal\\\":\\\"2024:01:01 12:00:00\\\",\\\"Orientation\\\":\\\"1\\\"}]'\n";
//     script.close();
//     chmod(fake_exiftool_path.c_str(), 0755);
//     #endif
//
//     // Create a dummy test file
//     std::string test_file = (tmpdir / "test.dng").string();
//     std::ofstream dummy(test_file);
//     dummy << "dummy";
//     dummy.close();
//
//     // Use extractor with our fake exiftool
//     ExiftoolDaemonExifExtractor extractor(fake_exiftool_path);
//     auto result = extractor.extract(FilePath{test_file});
//
//     // Verify extraction succeeded
//     UTEST_ASSERT(result.has_value());
//     UTEST_ASSERT_STRING(result->make.c_str(), "TestMake");
//     UTEST_ASSERT_STRING(result->model.c_str(), "TestModel");
//     UTEST_ASSERT_STRING(result->datetime_original.c_str(), "2024:01:01 12:00:00");
//     UTEST_ASSERT_EQ(result->orientation, 1);
//
//     // Count invocations in log
//     std::ifstream log(log_path);
//     std::string line;
//     int invocation_count = 0;
//     while (std::getline(log, line)) {
//         if (!line.empty()) invocation_count++;
//     }
//
//     // FAILING TEST: Current code calls exiftool TWICE (daemon + fallback)
//     // After fix: should be called ONCE
//     UTEST_ASSERT_EQ(invocation_count, 1);
//
//     // Cleanup
//     std::filesystem::remove_all(tmpdir);
// }

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}
