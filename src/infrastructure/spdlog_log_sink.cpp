/*
 * rawimport-pipeline - spdlog LogSink adapter implementation
 * Structured JSON/text output with correlation IDs and event taxonomy.
 * Compatible with spdlog 1.9+
 */
#include "infrastructure/spdlog_log_sink.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
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

// Convert domain LogLevel to spdlog level.
spdlog::level::level_enum to_spdlog_level(domain::LogLevel l) {
    switch (l) {
        case domain::LogLevel::Trace:   return spdlog::level::trace;
        case domain::LogLevel::Debug:   return spdlog::level::debug;
        case domain::LogLevel::Info:    return spdlog::level::info;
        case domain::LogLevel::Warn:    return spdlog::level::warn;
        case domain::LogLevel::Error:   return spdlog::level::err;
        case domain::LogLevel::Critical:return spdlog::level::critical;
    }
    return spdlog::level::info;
}

domain::LogLevel from_spdlog_level(spdlog::level::level_enum lvl) {
    switch (lvl) {
        case spdlog::level::trace: return domain::LogLevel::Trace;
        case spdlog::level::debug: return domain::LogLevel::Debug;
        case spdlog::level::info:  return domain::LogLevel::Info;
        case spdlog::level::warn:  return domain::LogLevel::Warn;
        case spdlog::level::err:   return domain::LogLevel::Error;
        case spdlog::level::critical: return domain::LogLevel::Critical;
        default: return domain::LogLevel::Info;
    }
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
        if (v == "json") return "json";
    }
    return "text";
}

} // anonymous namespace

// ── Impl (Pimpl) ──────────────────────────────────────────────────────────────
class SpdlogLogSink::Impl {
public:
    explicit Impl(std::string logger_name)
        : logger_(spdlog::get(logger_name)) {
        if (!logger_) {
            // Create logger with stdout + stderr sinks (compatible with spdlog 1.9+)
            auto stdout_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
            auto stderr_sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();

            std::vector<spdlog::sink_ptr> sinks = {stdout_sink, stderr_sink};

            // Add file sink if path specified
            if (const char* path = std::getenv("RAWIMPORT_LOG_FILE")) {
                try {
                    sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true));
                } catch (...) {
                    // Ignore file sink errors
                }
            }

            logger_ = std::make_shared<spdlog::logger>(logger_name, sinks.begin(), sinks.end());
            spdlog::register_logger(logger_);
        }

        // Set pattern based on format
        std::string fmt = log_format();
        if (fmt == "json") {
            logger_->set_pattern("%v"); // JSON formatter handles full output
        } else {
            logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
        }

        logger_->set_level(spdlog::level::trace);
        logger_->flush_on(spdlog::level::warn);
    }

    ~Impl() {
        if (logger_) {
            logger_->flush();
            spdlog::drop(logger_->name());
        }
    }

    void log_json(std::string_view event, std::string_view level,
                  const std::unordered_map<std::string, std::string>& fields) {
        std::ostringstream oss;
        oss << '{'
            << "\"timestamp\":\"" << now_iso8601() << "\","
            << "\"level\":\"" << level << "\","
            << "\"event\":\"" << event << "\"";

        if (!g_correlation_id.empty()) {
            oss << ",\"correlation_id\":\"" << g_correlation_id << "\"";
        }

        for (const auto& [k, v] : fields) {
            oss << ",\"" << k << "\":" << json_escape(v);
        }
        oss << '}';
        logger_->log(to_spdlog_level(from_string_level(level)), oss.str());
    }

    void log_text(spdlog::level::level_enum lvl, const std::string& msg,
                  const std::map<std::string, std::string>& ctx) {
        if (!ctx.empty()) {
            std::ostringstream oss;
            oss << msg << " [";
            bool first = true;
            for (const auto& [k, v] : ctx) {
                if (!first) oss << ", ";
                oss << k << "=" << v;
                first = false;
            }
            oss << "]";
            logger_->log(lvl, oss.str());
        } else {
            logger_->log(lvl, msg);
        }
    }

    // Helpers
    static spdlog::level::level_enum from_string_level(std::string_view lvl) {
        if (lvl == "trace") return spdlog::level::trace;
        if (lvl == "debug") return spdlog::level::debug;
        if (lvl == "info")  return spdlog::level::info;
        if (lvl == "warn")  return spdlog::level::warn;
        if (lvl == "error") return spdlog::level::err;
        if (lvl == "critical") return spdlog::level::critical;
        return spdlog::level::info;
    }

    std::shared_ptr<spdlog::logger> logger_;
};

// ── SpdlogLogSink ─────────────────────────────────────────────────────────────
SpdlogLogSink::SpdlogLogSink(std::string logger_name)
    : impl_(std::make_unique<Impl>(std::move(logger_name))) {}

SpdlogLogSink::~SpdlogLogSink() = default;

void SpdlogLogSink::LogStructured(std::string_view event,
                                   std::string_view level,
                                   const std::unordered_map<std::string, std::string>& fields) {
    impl_->log_json(event, level, fields);
}

void SpdlogLogSink::SetCorrelationId(std::string_view id) {
    g_correlation_id = std::string(id);
}

void SpdlogLogSink::ClearCorrelationId() {
    g_correlation_id.clear();
}

std::string SpdlogLogSink::correlation_id() {
    return g_correlation_id;
}

void SpdlogLogSink::trace(const std::string& msg,
                          const std::map<std::string, std::string>& ctx) {
    impl_->log_text(spdlog::level::trace, msg, ctx);
}
void SpdlogLogSink::debug(const std::string& msg,
                          const std::map<std::string, std::string>& ctx) {
    impl_->log_text(spdlog::level::debug, msg, ctx);
}
void SpdlogLogSink::info(const std::string& msg,
                         const std::map<std::string, std::string>& ctx) {
    impl_->log_text(spdlog::level::info, msg, ctx);
}
void SpdlogLogSink::warn(const std::string& msg,
                         const std::map<std::string, std::string>& ctx) {
    impl_->log_text(spdlog::level::warn, msg, ctx);
}
void SpdlogLogSink::error(const std::string& msg,
                          const std::map<std::string, std::string>& ctx) {
    impl_->log_text(spdlog::level::err, msg, ctx);
}
void SpdlogLogSink::critical(const std::string& msg,
                             const std::map<std::string, std::string>& ctx) {
    impl_->log_text(spdlog::level::critical, msg, ctx);
}

bool SpdlogLogSink::is_enabled(domain::LogLevel level) {
    return impl_->logger_->should_log(to_spdlog_level(level));
}

domain::LogLevel SpdlogLogSink::minimum_level() {
    return from_spdlog_level(impl_->logger_->level());
}

// ─── Convenience Methods: Event Taxonomy ──────────────────────────────────────
void SpdlogLogSink::LogCollisionDuplicateHash(std::string_view sequence_id,
                                               std::string_view duplicate_of,
                                               std::string_view source_hash,
                                               std::string_view duplicate_path) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"duplicate_of", std::string(duplicate_of)},
        {"source_hash", std::string(source_hash)},
        {"action", "moved_to_duplicates"},
        {"duplicate_path", std::string(duplicate_path)}
    };
    LogStructured("collision.duplicate_hash", "INFO", fields);
}

void SpdlogLogSink::LogCollisionDuplicateHashDeadLetter(std::string_view sequence_id,
                                                         std::string_view duplicate_of) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"duplicate_of", std::string(duplicate_of)},
        {"action", "dead_lettered_to_failed"}
    };
    LogStructured("collision.duplicate_hash_dead_letter", "WARN", fields);
}

void SpdlogLogSink::LogConversionFailedEngineError(std::string_view sequence_id,
                                                    std::string_view engine_name,
                                                    int exit_code,
                                                    std::string_view stderr_tail) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"engine_name", std::string(engine_name)},
        {"exit_code", std::to_string(exit_code)},
        {"stderr_tail", std::string(stderr_tail)}
    };
    LogStructured("conversion.failed.engine_error", "ERROR", fields);
}

void SpdlogLogSink::LogImportFailed(std::string_view sequence_id,
                                     std::string_view reason) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"reason", std::string(reason)}
    };
    LogStructured("import.failed", "ERROR", fields);
}

void SpdlogLogSink::LogCollisionDuplicateFound(std::string_view sequence_id,
                                                std::string_view existing_sequence_id,
                                                const domain::SHA256Hash& source_hash,
                                                std::string_view duplicate_path) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"existing_sequence_id", std::string(existing_sequence_id)},
        {"source_hash", domain::sha256_hex(source_hash)},
        {"duplicate_path", std::string(duplicate_path)}
    };
    LogStructured("collision.duplicate_found", "WARN", fields);
}

void SpdlogLogSink::LogConversionFailedHashMismatch(std::string_view sequence_id,
                                                     std::string_view source_hash,
                                                     std::string_view output_hash) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"source_hash", std::string(source_hash)},
        {"output_hash", std::string(output_hash)},
        {"action", "dead_lettered"}
    };
    LogStructured("conversion.failed.hash_mismatch", "ERROR", fields);
}

void SpdlogLogSink::LogConversionFailedTimeout(std::string_view sequence_id,
                                                int timeout_seconds) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"timeout_seconds", std::to_string(timeout_seconds)},
        {"action", "dead_lettered"}
    };
    LogStructured("conversion.failed.timeout", "ERROR", fields);
}

void SpdlogLogSink::LogDeadLetterMaxRetries(std::string_view sequence_id,
                                             int max_retries,
                                             std::string_view reason) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"max_retries", std::to_string(max_retries)},
        {"reason", std::string(reason)}
    };
    LogStructured("dead_letter.max_retries", "ERROR", fields);
}

void SpdlogLogSink::LogWatchDirectoryInaccessible(std::string_view path,
                                                   int error_code) {
    std::unordered_map<std::string, std::string> fields = {
        {"path", std::string(path)},
        {"error_code", std::to_string(error_code)}
    };
    LogStructured("watch.directory_inaccessible", "ERROR", fields);
}

void SpdlogLogSink::LogWatchFilesystemFull(std::string_view path) {
    std::unordered_map<std::string, std::string> fields = {
        {"path", std::string(path)}
    };
    LogStructured("watch.filesystem_full", "ERROR", fields);
}

void SpdlogLogSink::LogDbConnectionLost(int consecutive_failures,
                                         int max_retries) {
    std::unordered_map<std::string, std::string> fields = {
        {"consecutive_failures", std::to_string(consecutive_failures)},
        {"max_retries", std::to_string(max_retries)}
    };
    LogStructured("db.connection_lost", "ERROR", fields);
}

void SpdlogLogSink::LogDbDeadlock(std::string_view query) {
    std::unordered_map<std::string, std::string> fields = {
        {"query", std::string(query)}
    };
    LogStructured("db.deadlock", "ERROR", fields);
}

void SpdlogLogSink::LogDbQueryFailed(std::string_view query,
                                      std::string_view error) {
    std::unordered_map<std::string, std::string> fields = {
        {"query", std::string(query)},
        {"error", std::string(error)}
    };
    LogStructured("db.query_failed", "ERROR", fields);
}

void SpdlogLogSink::LogExifExtractionFailed(std::string_view sequence_id,
                                             std::string_view source_path,
                                             std::string_view engine,
                                             bool is_retry) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"source_path", std::string(source_path)},
        {"engine", std::string(engine)},
        {"is_retry", is_retry ? "true" : "false"}
    };
    LogStructured("exif.extraction_failed", "ERROR", fields);
}

void SpdlogLogSink::LogExifToolDaemonRestarted(std::string_view sequence_id,
                                                int restart_count) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"restart_count", std::to_string(restart_count)}
    };
    LogStructured("exif.tool_daemon_restarted", "WARN", fields);
}

void SpdlogLogSink::LogConverterEngineUnavailable(std::string_view sequence_id,
                                                   std::string_view engine) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"engine", std::string(engine)}
    };
    LogStructured("converter.engine_unavailable", "ERROR", fields);
}

void SpdlogLogSink::LogConverterUnsupportedFormat(std::string_view sequence_id,
                                                   std::string_view source_path,
                                                   std::string_view format) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"source_path", std::string(source_path)},
        {"format", std::string(format)}
    };
    LogStructured("converter.unsupported_format", "ERROR", fields);
}

void SpdlogLogSink::LogFilesystemReadError(std::string_view sequence_id,
                                            std::string_view path,
                                            int error_code,
                                            std::string_view operation) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"path", std::string(path)},
        {"error_code", std::to_string(error_code)},
        {"operation", std::string(operation)}
    };
    LogStructured("filesystem.read_error", "ERROR", fields);
}

void SpdlogLogSink::LogFilesystemWriteError(std::string_view sequence_id,
                                             std::string_view path,
                                             int error_code,
                                             std::string_view operation) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"path", std::string(path)},
        {"error_code", std::to_string(error_code)},
        {"operation", std::string(operation)}
    };
    LogStructured("filesystem.write_error", "ERROR", fields);
}

void SpdlogLogSink::LogFilesystemArchiveFailed(std::string_view sequence_id,
                                                std::string_view source,
                                                std::string_view dest,
                                                std::string_view error) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"source", std::string(source)},
        {"dest", std::string(dest)},
        {"error", std::string(error)}
    };
    LogStructured("filesystem.archive_failed", "ERROR", fields);
}

void SpdlogLogSink::LogHashVerificationFailed(std::string_view sequence_id,
                                               std::string_view expected,
                                               std::string_view actual,
                                               std::string_view file) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"expected_hash", std::string(expected)},
        {"actual_hash", std::string(actual)},
        {"file", std::string(file)}
    };
    LogStructured("hash.verification_failed", "ERROR", fields);
}

void SpdlogLogSink::LogReconversionLockFailed(std::string_view sequence_id,
                                               std::string_view holder,
                                               int retry_count) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"holder", std::string(holder)},
        {"retry_count", std::to_string(retry_count)}
    };
    LogStructured("reconversion.lock_failed", "WARN", fields);
}

void SpdlogLogSink::LogReconversionSourceMissing(std::string_view sequence_id,
                                                  std::string_view source_path) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"source_path", std::string(source_path)}
    };
    LogStructured("reconversion.source_missing", "ERROR", fields);
}

void SpdlogLogSink::LogRotationPushFailed(std::string_view sequence_id,
                                           std::string_view target,
                                           std::string_view error) {
    std::unordered_map<std::string, std::string> fields = {
        {"sequence_id", std::string(sequence_id)},
        {"target", std::string(target)},
        {"error", std::string(error)}
    };
    LogStructured("rotation.push_failed", "ERROR", fields);
}

std::shared_ptr<domain::LogSink> make_spdlog_log_sink(std::string logger_name) {
    return std::make_shared<SpdlogLogSink>(std::move(logger_name));
}

} // namespace rawimport::infrastructure