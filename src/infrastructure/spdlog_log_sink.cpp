/*
 * rawimport-pipeline - spdlog LogSink adapter implementation
 * Structured JSON/text output with correlation IDs and event taxonomy.
 */
#include "infrastructure/spdlog_log_sink.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/formatters.h>
#include <spdlog/pattern_formatter.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace rawimport::infrastructure {

namespace {

// Thread-local correlation ID.
thread_local std::string g_correlation_id = "";

// Format helpers.
std::string to_spdlog_level(domain::LogLevel l) {
    switch (l) {
        case domain::LogLevel::Trace:   return "trace";
        case domain::LogLevel::Debug:   return "debug";
        case domain::LogLevel::Info:    return "info";
        case domain::LogLevel::Warn:    return "warn";
        case domain::LogLevel::Error:   return "error";
        case domain::LogLevel::Critical:return "critical";
    }
    return "info";
}

domain::LogLevel from_spdlog_level(const std::string& lvl) {
    if (lvl == "trace")   return domain::LogLevel::Trace;
    if (lvl == "debug")   return domain::LogLevel::Debug;
    if (lvl == "info")    return domain::LogLevel::Info;
    if (lvl == "warn")    return domain::LogLevel::Warn;
    if (lvl == "error")   return domain::LogLevel::Error;
    if (lvl == "critical")return domain::LogLevel::Critical;
    return domain::LogLevel::Info;
}

// JSON escaping for string values.
std::string json_escape(std::string_view s) {
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b";  break;
            case '\f': oss << "\\f";  break;
            case '\n': oss << "\\n";  break;
            case '\r': oss << "\\r";  break;
            case '\t': oss << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                } else {
                    oss << c;
                }
        }
    }
    oss << '"';
    return oss.str();
}

// Current timestamp in ISO 8601 UTC with milliseconds.
std::string now_iso8601() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    auto time = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}

// Read log format from env (json|text), default text.
std::string log_format() {
    if (const char* env = std::getenv("RAWIMPORT_LOG_FORMAT")) {
        std::string v = env;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (v == "json" || v == "text") return v;
    }
    return "text";
}

} // anon

class SpdlogLogSinkImpl : public domain::LogSink {
public:
    explicit SpdlogLogSinkImpl(std::string logger_name)
        : logger_name_(std::move(logger_name)), format_(log_format()) {
        if (format_ == "json") {
            setup_json_logger();
        } else {
            setup_text_logger();
        }
    }

    // ─── Structured Logging ──────────────────────────────────────────────────
    void LogStructured(std::string_view event,
                       std::string_view level,
                       const std::unordered_map<std::string, std::string>& fields) override {
        if (format_ == "json") {
            log_json_structured(event, level, fields);
        } else {
            log_text_structured(event, level, fields);
        }
    }

    // ─── Correlation ID ──────────────────────────────────────────────────────
    void SetCorrelationId(std::string_view id) override {
        g_correlation_id = id;
    }

    void ClearCorrelationId() override {
        g_correlation_id.clear();
    }

    std::string correlation_id() override {
        return g_correlation_id;
    }

    // ─── Legacy Text Logging ─────────────────────────────────────────────────
    void trace(const std::string& msg, const std::map<std::string, std::string>& ctx) override {
        log_text(spdlog::level::trace, msg, ctx);
    }
    void debug(const std::string& msg, const std::map<std::string, std::string>& ctx) override {
        log_text(spdlog::level::debug, msg, ctx);
    }
    void info(const std::string& msg, const std::map<std::string, std::string>& ctx) override {
        log_text(spdlog::level::info, msg, ctx);
    }
    void warn(const std::string& msg, const std::map<std::string, std::string>& ctx) override {
        log_text(spdlog::level::warn, msg, ctx);
    }
    void error(const std::string& msg, const std::map<std::string, std::string>& ctx) override {
        log_text(spdlog::level::err, msg, ctx);
    }
    void critical(const std::string& msg, const std::map<std::string, std::string>& ctx) override {
        log_text(spdlog::level::critical, msg, ctx);
    }

    bool is_enabled(domain::LogLevel level) override {
        return logger_->should_log(to_spdlog_level(level));
    }

    domain::LogLevel minimum_level() override {
        return from_spdlog_level(spdlog::level::to_string_view(logger_->level()).to_string());
    }

    // ─── Convenience Methods: Event Taxonomy ─────────────────────────────────

    // collision.duplicate_hash (INFO)
    void LogCollisionDuplicateHash(std::string_view sequence_id,
                                   std::string_view duplicate_of,
                                   std::string_view source_hash,
                                   std::string_view duplicate_path) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"duplicate_of", std::string(duplicate_of)},
            {"source_hash", std::string(source_hash)},
            {"action", "moved_to_duplicates"},
            {"duplicate_path", std::string(duplicate_path)}
        };
        LogStructured("collision.duplicate_hash", "INFO", fields);
    }

    // collision.duplicate_hash_dead_letter (WARN)
    void LogCollisionDuplicateHashDeadLetter(std::string_view sequence_id,
                                             std::string_view duplicate_of) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"duplicate_of", std::string(duplicate_of)},
            {"action", "dead_lettered_to_failed"}
        };
        LogStructured("collision.duplicate_hash_dead_letter", "WARN", fields);
    }

    // conversion.failed.engine_error (ERROR)
    void LogConversionFailedEngineError(std::string_view sequence_id,
                                        std::string_view engine_name,
                                        int exit_code,
                                        std::string_view stderr_tail) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"engine_name", std::string(engine_name)},
            {"exit_code", std::to_string(exit_code)},
            {"stderr_tail", std::string(stderr_tail)}
        };
        LogStructured("conversion.failed.engine_error", "ERROR", fields);
    }

    // conversion.failed.hash_mismatch (ERROR)
    void LogConversionFailedHashMismatch(std::string_view sequence_id,
                                         std::string_view source_hash,
                                         std::string_view output_hash) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"source_hash", std::string(source_hash)},
            {"output_hash", std::string(output_hash)},
            {"action", "dead_lettered"}
        };
        LogStructured("conversion.failed.hash_mismatch", "ERROR", fields);
    }

    // conversion.failed.timeout (ERROR)
    void LogConversionFailedTimeout(std::string_view sequence_id,
                                    int timeout_sec) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"timeout_sec", std::to_string(timeout_sec)},
            {"action", "dead_lettered"}
        };
        LogStructured("conversion.failed.timeout", "ERROR", fields);
    }

    // dead_letter.max_retries (WARN)
    void LogDeadLetterMaxRetries(std::string_view sequence_id,
                                 int max_retries,
                                 std::string_view last_error) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"max_retries", std::to_string(max_retries)},
            {"last_error", std::string(last_error)},
            {"action", "archived_to_failed"}
        };
        LogStructured("dead_letter.max_retries", "WARN", fields);
    }

    // watch.directory_inaccessible (WARN)
    void LogWatchDirectoryInaccessible(std::string_view watch_dir,
                                       int errno_code) override {
        std::unordered_map<std::string, std::string> fields = {
            {"watch_dir", std::string(watch_dir)},
            {"errno", std::to_string(errno_code)}
        };
        LogStructured("watch.directory_inaccessible", "WARN", fields);
    }

    // watch.filesystem_full (WARN)
    void LogWatchFilesystemFull(std::string_view watch_dir) override {
        std::unordered_map<std::string, std::string> fields = {
            {"watch_dir", std::string(watch_dir)}
        };
        LogStructured("watch.filesystem_full", "WARN", fields);
    }

    // db.connection_lost (ERROR)
    void LogDbConnectionLost(int attempt, int max_attempts) override {
        std::unordered_map<std::string, std::string> fields = {
            {"attempt", std::to_string(attempt)},
            {"max_attempts", std::to_string(max_attempts)}
        };
        LogStructured("db.connection_lost", "ERROR", fields);
    }

    // db.deadlock (ERROR)
    void LogDbDeadlock(std::string_view sequence_id) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)}
        };
        LogStructured("db.deadlock", "ERROR", fields);
    }

    // db.query_failed (ERROR)
    void LogDbQueryFailed(std::string_view sequence_id,
                          std::string_view error) override {
        std::unordered_map<std::string, std::string> fields = {
            {"sequence_id", std::string(sequence_id)},
            {"error", std::string(error)}
        };
        LogStructured("db.query_failed", "ERROR", fields);
    }

private:
    std::string logger_name_;
    std::string format_;
    std::shared_ptr<spdlog::logger> logger_;

    // ─── Logger Setup ────────────────────────────────────────────────────────

    void setup_text_logger() {
        logger_ = spdlog::get(logger_name_);
        if (!logger_) {
            auto sink = std::make_shared<spdlog::sinks::stdout_stderr_sink_mt>();
            logger_ = std::make_shared<spdlog::logger>(logger_name_, sink);
            logger_->set_level(spdlog::level::info);
            logger_->flush_on(spdlog::level::err);
            spdlog::register_logger(logger_);
        }
        // Text pattern: [corr_id] [level] event: message key=val ...
        logger_->set_pattern("%v");
    }

    void setup_json_logger() {
        logger_ = spdlog::get(logger_name_ + "_json");
        if (!logger_) {
            auto sink = std::make_shared<spdlog::sinks::stdout_stderr_sink_mt>();
            logger_ = std::make_shared<spdlog::logger>(logger_name_ + "_json", sink);
            logger_->set_level(spdlog::level::info);
            logger_->flush_on(spdlog::level::err);
            // Use raw formatter — we build full JSON ourselves in LogStructured
            logger_->set_pattern("%v");
            spdlog::register_logger(logger_);
        }
    }

    // ─── Text Logging Helpers ────────────────────────────────────────────────

    void log_text(spdlog::level::level_enum lvl,
                  const std::string& msg,
                  const std::map<std::string, std::string>& ctx) {
        std::ostringstream oss;
        if (!g_correlation_id.empty()) {
            oss << "[corr:" << g_correlation_id << "] ";
        }
        for (const auto& [k, v] : ctx) {
            oss << k << '=' << v << ' ';
        }
        oss << msg;
        logger_->log(lvl, "{}", oss.str());
    }

    void log_text_structured(std::string_view event,
                             std::string_view level,
                             const std::unordered_map<std::string, std::string>& fields) {
        std::ostringstream oss;
        if (!g_correlation_id.empty()) {
            oss << "[corr:" << g_correlation_id << "] ";
        }
        oss << '[' << level << "] " << event << ": ";
        bool first = false;
        for (const auto& [k, v] : fields) {
            if (!first) oss << ", ";
            oss << k << '=' << v;
            first = false;
        }
        // Map level string to spdlog level
        spdlog::level::level_enum lvl = spdlog::level::info;
        if (level == "TRACE") lvl = spdlog::level::trace;
        else if (level == "DEBUG") lvl = spdlog::level::debug;
        else if (level == "WARN") lvl = spdlog::level::warn;
        else if (level == "ERROR") lvl = spdlog::level::err;
        else if (level == "CRITICAL") lvl = spdlog::level::critical;
        logger_->log(lvl, "{}", oss.str());
    }

    // ─── JSON Logging ────────────────────────────────────────────────────────

    void log_json_structured(std::string_view event,
                             std::string_view level,
                             const std::unordered_map<std::string, std::string>& fields) {
        std::ostringstream oss;
        oss << '{';
        oss << "\"timestamp\":" << json_escape(now_iso8601()) << ',';
        oss << "\"level\":" << json_escape(std::string(level)) << ',';
        oss << "\"event\":" << json_escape(std::string(event));

        // Fields
        bool first = false;
        for (const auto& [k, v] : fields) {
            if (!first) oss << ',';
            oss << json_escape(k) << ':' << json_escape(v);
            first = false;
        }

        // Correlation ID
        if (!g_correlation_id.empty()) {
            if (!first) oss << ',';
            oss << "\"correlation_id\":" << json_escape(g_correlation_id);
        }

        oss << '}';

        // Map level string to spdlog level for filtering
        spdlog::level::level_enum lvl = spdlog::level::info;
        if (level == "TRACE") lvl = spdlog::level::trace;
        else if (level == "DEBUG") lvl = spdlog::level::debug;
        else if (level == "WARN") lvl = spdlog::level::warn;
        else if (level == "ERROR") lvl = spdlog::level::err;
        else if (level == "CRITICAL") lvl = spdlog::level::critical;
        logger_->log(lvl, "{}", oss.str());
    }
};

SpdlogLogSink::SpdlogLogSink(std::string logger_name)
    : impl_(std::make_unique<SpdlogLogSinkImpl>(std::move(logger_name))) {}

SpdlogLogSink::~SpdlogLogSink() = default;

void SpdlogLogSink::LogStructured(std::string_view event,
                                  std::string_view level,
                                  const std::unordered_map<std::string, std::string>& fields) {
    impl_->LogStructured(event, level, fields);
}

void SpdlogLogSink::SetCorrelationId(std::string_view id) {
    impl_->SetCorrelationId(id);
}

void SpdlogLogSink::ClearCorrelationId() {
    impl_->ClearCorrelationId();
}

std::string SpdlogLogSink::correlation_id() {
    return impl_->correlation_id();
}

void SpdlogLogSink::trace(const std::string& msg, const std::map<std::string, std::string>& ctx) {
    impl_->trace(msg, ctx);
}
void SpdlogLogSink::debug(const std::string& msg, const std::map<std::string, std::string>& ctx) {
    impl_->debug(msg, ctx);
}
void SpdlogLogSink::info(const std::string& msg, const std::map<std::string, std::string>& ctx) {
    impl_->info(msg, ctx);
}
void SpdlogLogSink::warn(const std::string& msg, const std::map<std::string, std::string>& ctx) {
    impl_->warn(msg, ctx);
}
void SpdlogLogSink::error(const std::string& msg, const std::map<std::string, std::string>& ctx) {
    impl_->error(msg, ctx);
}
void SpdlogLogSink::critical(const std::string& msg, const std::map<std::string, std::string>& ctx) {
    impl_->critical(msg, ctx);
}
bool SpdlogLogSink::is_enabled(domain::LogLevel level) { return impl_->is_enabled(level); }
domain::LogLevel SpdlogLogSink::minimum_level() { return impl_->minimum_level(); }

// Convenience methods — forward to impl
void SpdlogLogSink::LogCollisionDuplicateHash(std::string_view sequence_id,
                                              std::string_view duplicate_of,
                                              std::string_view source_hash,
                                              std::string_view duplicate_path) {
    impl_->LogCollisionDuplicateHash(sequence_id, duplicate_of, source_hash, duplicate_path);
}

void SpdlogLogSink::LogCollisionDuplicateHashDeadLetter(std::string_view sequence_id,
                                                        std::string_view duplicate_of) {
    impl_->LogCollisionDuplicateHashDeadLetter(sequence_id, duplicate_of);
}

void SpdlogLogSink::LogConversionFailedEngineError(std::string_view sequence_id,
                                                   std::string_view engine_name,
                                                   int exit_code,
                                                   std::string_view stderr_tail) {
    impl_->LogConversionFailedEngineError(sequence_id, engine_name, exit_code, stderr_tail);
}

void SpdlogLogSink::LogConversionFailedHashMismatch(std::string_view sequence_id,
                                                    std::string_view source_hash,
                                                    std::string_view output_hash) {
    impl_->LogConversionFailedHashMismatch(sequence_id, source_hash, output_hash);
}

void SpdlogLogSink::LogConversionFailedTimeout(std::string_view sequence_id,
                                               int timeout_sec) {
    impl_->LogConversionFailedTimeout(sequence_id, timeout_sec);
}

void SpdlogLogSink::LogDeadLetterMaxRetries(std::string_view sequence_id,
                                            int max_retries,
                                            std::string_view last_error) {
    impl_->LogDeadLetterMaxRetries(sequence_id, max_retries, last_error);
}

void SpdlogLogSink::LogWatchDirectoryInaccessible(std::string_view watch_dir,
                                                  int errno_code) {
    impl_->LogWatchDirectoryInaccessible(watch_dir, errno_code);
}

void SpdlogLogSink::LogWatchFilesystemFull(std::string_view watch_dir) {
    impl_->LogWatchFilesystemFull(watch_dir);
}

void SpdlogLogSink::LogDbConnectionLost(int attempt, int max_attempts) {
    impl_->LogDbConnectionLost(attempt, max_attempts);
}

void SpdlogLogSink::LogDbDeadlock(std::string_view sequence_id) {
    impl_->LogDbDeadlock(sequence_id);
}

void SpdlogLogSink::LogDbQueryFailed(std::string_view sequence_id,
                                     std::string_view error) {
    impl_->LogDbQueryFailed(sequence_id, error);
}

std::shared_ptr<domain::LogSink> make_spdlog_log_sink(std::string logger_name) {
    return std::make_shared<SpdlogLogSink>(std::move(logger_name));
}

} // namespace rawimport::infrastructure