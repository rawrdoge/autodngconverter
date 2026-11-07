/*
 * rawimport-pipeline - Converter Factory unit tests
 * Tests the factory logic with mocked ConfigProvider and LogSink.
 */
#include "utest.h"

#include <memory>
#include <string>
#include <unordered_map>

#include "domain/ports/config_provider.hpp"
#include "domain/ports/log_sink.hpp"
#include "domain/types.hpp"

using namespace rawimport::domain;

// ─── Mock ConfigProvider ──────────────────────────────────────────────────────

class MockConfigProvider final : public ConfigProvider {
public:
    explicit MockConfigProvider(std::initializer_list<std::pair<const std::string, std::string>> values)
        : values_(values) {}

    std::optional<std::string> get_string(const std::string& key) override {
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
        return std::nullopt;
    }
    std::optional<int> get_int(const std::string&) override { return std::nullopt; }
    std::optional<bool> get_bool(const std::string&) override { return std::nullopt; }
    std::optional<double> get_double(const std::string&) override { return std::nullopt; }
    std::map<std::string, std::string> to_map() override { return values_; }
    ValidationReport validate(const std::vector<RequiredConfigKey>&) override {
        ValidationReport r; r.valid = true; return r;
    }

private:
    std::map<std::string, std::string> values_;
};

// ─── Mock LogSink ─────────────────────────────────────────────────────────────

class MockLogSink final : public LogSink {
public:
    struct Entry { LogLevel level; std::string msg; };
    std::vector<Entry> entries;

    // Structured logging
    void LogStructured(std::string_view event, std::string_view level,
                       const std::unordered_map<std::string, std::string>& fields) override {
        entries.push_back({LogLevel::Info, std::string(event) + " [" + std::string(level) + "]"});
        (void)fields;
    }

    // Correlation ID
    void SetCorrelationId(std::string_view id) override { correlation_id_ = std::string(id); }
    void ClearCorrelationId() override { correlation_id_.clear(); }
    std::string correlation_id() override { return correlation_id_.empty() ? "test-correlation" : correlation_id_; }

    // Legacy text logging
    void trace(const std::string& msg, const std::map<std::string, std::string>&) override { entries.push_back({LogLevel::Trace, msg}); }
    void debug(const std::string& msg, const std::map<std::string, std::string>&) override { entries.push_back({LogLevel::Debug, msg}); }
    void info(const std::string& msg, const std::map<std::string, std::string>&) override { entries.push_back({LogLevel::Info, msg}); }
    void warn(const std::string& msg, const std::map<std::string, std::string>&) override { entries.push_back({LogLevel::Warn, msg}); }
    void error(const std::string& msg, const std::map<std::string, std::string>&) override { entries.push_back({LogLevel::Error, msg}); }
    void critical(const std::string& msg, const std::map<std::string, std::string>&) override { entries.push_back({LogLevel::Critical, msg}); }

    bool is_enabled(LogLevel) override { return true; }
    LogLevel minimum_level() override { return LogLevel::Trace; }

    // Convenience methods (no-op for testing)
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

private:
    std::string correlation_id_;
};

// ─── Include factory implementation for testing ───────────────────────────────
// We include the .cpp directly to test the internal logic without needing the adapters.
#include "../../src/infrastructure/factories/converter_factory.cpp"

using namespace rawimport::infrastructure;

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST(converter_factory_default_dnglab) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "dnglab"}});
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_DNGLAB
    // With WITH_DNGLAB, should create DnglabConverterEngine
    auto engine = MakeConverterEngine(cfg, log);
    UTEST_ASSERT(engine != nullptr);
    UTEST_ASSERT(engine->health_check().status != HealthStatus::Unhealthy);
#else
    // Without WITH_DNGLAB, should throw or try Adobe DNG fallback
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
#endif
}

TEST(converter_factory_explicit_dnglab) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "DNGLAB"}});
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_DNGLAB
    auto engine = MakeConverterEngine(cfg, log);
    UTEST_ASSERT(engine != nullptr);
#else
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
#endif
}

TEST(converter_factory_case_insensitive) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "DnGlAb"}});
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_DNGLAB
    auto engine = MakeConverterEngine(cfg, log);
    UTEST_ASSERT(engine != nullptr);
#else
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
#endif
}

TEST(converter_factory_adobe_explicit_request) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "adobedng"}});
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_ADOBE_DNG
    // Adobe DNG explicitly requested - will fail health check (no binary)
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error& e) {
        threw = true;
        // Should fail fast with clear error
        UTEST_ASSERT(std::string(e.what()).find("Adobe DNG Converter") != std::string::npos);
    }
    UTEST_ASSERT(threw);
#else
    // WITH_ADOBE_DNG not enabled - should throw compile-time error
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error& e) {
        threw = true;
        UTEST_ASSERT(std::string(e.what()).find("WITH_ADOBE_DNG not enabled") != std::string::npos);
    }
    UTEST_ASSERT(threw);
#endif
}

TEST(converter_factory_adobe_alias) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "adobe"}});
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_ADOBE_DNG
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
#else
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error& e) {
        threw = true;
        UTEST_ASSERT(std::string(e.what()).find("WITH_ADOBE_DNG not enabled") != std::string::npos);
    }
    UTEST_ASSERT(threw);
#endif
}

TEST(converter_factory_unknown_engine) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "unknown_engine"}});
    auto log = std::make_shared<MockLogSink>();

    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::invalid_argument& e) {
        threw = true;
        UTEST_ASSERT(std::string(e.what()).find("Unknown converter engine") != std::string::npos);
    }
    UTEST_ASSERT(threw);
}

TEST(converter_factory_empty_engine_defaults_to_dnglab) {
    MockConfigProvider cfg({}); // No CONVERTER_ENGINE set
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_DNGLAB
    auto engine = MakeConverterEngine(cfg, log);
    UTEST_ASSERT(engine != nullptr);
#else
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
#endif
}

TEST(converter_factory_log_sink_receives_messages) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "dnglab"}});
    auto log = std::make_shared<MockLogSink>();

#ifdef WITH_DNGLAB
    auto engine = MakeConverterEngine(cfg, log);
    UTEST_ASSERT(engine != nullptr);
    // Should have logged at least info messages
    bool has_info = false;
    for (const auto& e : log->entries) {
        if (e.level == LogLevel::Info) { has_info = true; break; }
    }
    UTEST_ASSERT(has_info);
#else
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
    // Should have logged error
    bool has_error = false;
    for (const auto& e : log->entries) {
        if (e.level == LogLevel::Error) { has_error = true; break; }
    }
    UTEST_ASSERT(has_error);
#endif
}

TEST(converter_factory_null_log_sink) {
    MockConfigProvider cfg({{"CONVERTER_ENGINE", "dnglab"}});
    std::shared_ptr<LogSink> log = nullptr; // null log sink

#ifdef WITH_DNGLAB
    // Should not crash with null log
    auto engine = MakeConverterEngine(cfg, log);
    UTEST_ASSERT(engine != nullptr);
#else
    bool threw = false;
    try {
        auto engine = MakeConverterEngine(cfg, log);
        (void)engine;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    UTEST_ASSERT(threw);
#endif
}

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}