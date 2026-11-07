/*
 * rawimport-pipeline - spdlog LogSink adapter
 * Structured JSON/text output with correlation IDs and event taxonomy.
 */
#pragma once

#include <string>
#include <map>
#include <memory>
#include <unordered_map>
#include <string_view>

struct spdlog_logger; // opaque forward

namespace rawimport::domain { class LogSink; }

namespace rawimport::infrastructure {

class SpdlogLogSink final {
public:
    explicit SpdlogLogSink(std::string logger_name = "rawimport");
    ~SpdlogLogSink();

    // Non-copyable, non-movable (spdlog lifecycle).
    SpdlogLogSink(const SpdlogLogSink&) = delete;
    SpdlogLogSink& operator=(const SpdlogLogSink&) = delete;

    // ─── Structured Logging ──────────────────────────────────────────────────
    void LogStructured(std::string_view event,
                       std::string_view level,
                       const std::unordered_map<std::string, std::string>& fields);

    // ─── Correlation ID ──────────────────────────────────────────────────────
    void SetCorrelationId(std::string_view id);
    void ClearCorrelationId();
    std::string correlation_id();

    // ─── Legacy Text Logging (kept for compatibility) ────────────────────────
    void trace(const std::string& msg, const std::map<std::string, std::string>& ctx);
    void debug(const std::string& msg, const std::map<std::string, std::string>& ctx);
    void info(const std::string& msg, const std::map<std::string, std::string>& ctx);
    void warn(const std::string& msg, const std::map<std::string, std::string>& ctx);
    void error(const std::string& msg, const std::map<std::string, std::string>& ctx);
    void critical(const std::string& msg, const std::map<std::string, std::string>& ctx);

    bool       is_enabled(domain::LogLevel level);
    domain::LogLevel minimum_level();

    // ─── Convenience Methods: Event Taxonomy ─────────────────────────────────
    void LogCollisionDuplicateHash(std::string_view sequence_id,
                                   std::string_view duplicate_of,
                                   std::string_view source_hash,
                                   std::string_view duplicate_path);

    void LogCollisionDuplicateHashDeadLetter(std::string_view sequence_id,
                                             std::string_view duplicate_of);

    void LogConversionFailedEngineError(std::string_view sequence_id,
                                        std::string_view engine_name,
                                        int exit_code,
                                        std::string_view stderr_tail);

    void LogConversionFailedHashMismatch(std::string_view sequence_id,
                                         std::string_view source_hash,
                                         std::string_view output_hash);

    void LogConversionFailedTimeout(std::string_view sequence_id,
                                    int timeout_sec);

    void LogDeadLetterMaxRetries(std::string_view sequence_id,
                                 int max_retries,
                                 std::string_view last_error);

    void LogWatchDirectoryInaccessible(std::string_view watch_dir,
                                       int errno_code);

    void LogWatchFilesystemFull(std::string_view watch_dir);

    void LogDbConnectionLost(int attempt, int max_attempts);

    void LogDbDeadlock(std::string_view sequence_id);

    void LogDbQueryFailed(std::string_view sequence_id,
                          std::string_view error);

private:
    std::string logger_name_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Factory that returns a domain::LogSink shared pointer.
std::shared_ptr<domain::LogSink> make_spdlog_log_sink(std::string logger_name = "rawimport");

} // namespace rawimport::infrastructure
