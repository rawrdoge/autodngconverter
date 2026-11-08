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

#include "domain/types.hpp"

using namespace rawimport::infrastructure;
using namespace rawimport::domain;

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
    UTEST_ASSERT(m.count("OUTPUT_DIR") == 1u);
    UTEST_ASSERT_STRING(m["OUTPUT_DIR"].c_str(), "/tmp/out");
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

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}
