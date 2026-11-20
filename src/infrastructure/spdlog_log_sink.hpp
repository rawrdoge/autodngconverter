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

#include "domain/ports/log_sink.hpp"

struct spdlog_logger; // opaque forward

namespace rawimport::infrastructure {

class SpdlogLogSink final : public domain::LogSink {
public:
    explicit SpdlogLogSink(std::string logger_name = "rawimport");
    ~SpdlogLogSink() override;

    // Non-copyable, non-movable (spdlog lifecycle).
    SpdlogLogSink(const SpdlogLogSink&) = delete;
    SpdlogLogSink& operator=(const SpdlogLogSink&) = delete;

    // ─── Structured Logging ──────────────────────────────────────────────────
    void LogStructured(std::string_view event,
                       std::string_view level,
                       const std::unordered_map<std::string, std::string>& fields) override;

    // ─── Correlation ID ──────────────────────────────────────────────────────
    void SetCorrelationId(std::string_view id) override;
    void ClearCorrelationId() override;
    std::string correlation_id() override;

    // ─── Legacy Text Logging (kept for compatibility) ────────────────────────
    void trace(const std::string& msg, const std::map<std::string, std::string>& ctx) override;
    void debug(const std::string& msg, const std::map<std::string, std::string>& ctx) override;
    void info(const std::string& msg, const std::map<std::string, std::string>& ctx) override;
    void warn(const std::string& msg, const std::map<std::string, std::string>& ctx) override;
    void error(const std::string& msg, const std::map<std::string, std::string>& ctx) override;
    void critical(const std::string& msg, const std::map<std::string, std::string>& ctx) override;

    bool       is_enabled(domain::LogLevel level) override;
    domain::LogLevel minimum_level() override;

    // ─── Convenience Methods: Event Taxonomy ─────────────────────────────────
    void LogCollisionDuplicateHash(std::string_view sequence_id,
                                   std::string_view duplicate_of,
                                   std::string_view source_hash,
                                   std::string_view duplicate_path) override;

    void LogCollisionDuplicateHashDeadLetter(std::string_view sequence_id,
                                             std::string_view duplicate_of) override;

    void LogConversionFailedEngineError(std::string_view sequence_id,
                                        std::string_view engine_name,
                                        int exit_code,
                                        std::string_view stderr_tail) override;

    void LogImportFailed(std::string_view sequence_id,
                         std::string_view reason) override;

    void LogCollisionDuplicateFound(std::string_view sequence_id,
                                    std::string_view existing_sequence_id,
                                    const domain::SHA256Hash& source_hash,
                                    std::string_view duplicate_path) override;

    void LogConversionFailedHashMismatch(std::string_view sequence_id,
                                         std::string_view source_hash,
                                         std::string_view output_hash) override;

    void LogConversionFailedTimeout(std::string_view sequence_id,
                                    int timeout_sec) override;

    void LogDeadLetterMaxRetries(std::string_view sequence_id,
                                 int max_retries,
                                 std::string_view last_error) override;

    void LogWatchDirectoryInaccessible(std::string_view watch_dir,
                                       int errno_code) override;

    void LogWatchFilesystemFull(std::string_view watch_dir) override;

    void LogDbConnectionLost(int attempt, int max_attempts) override;

    void LogDbDeadlock(std::string_view sequence_id) override;

    void LogDbQueryFailed(std::string_view sequence_id,
                          std::string_view error) override;

    // ─── ExifTool Events ───────────────────────────────────────────────────────
    void LogExifExtractionFailed(std::string_view sequence_id,
                                 std::string_view source_path,
                                 std::string_view error,
                                 bool daemon_restarted) override;

    void LogExifToolDaemonRestarted(std::string_view reason,
                                     int restart_count) override;

    // ─── Converter Events ──────────────────────────────────────────────────────
    void LogConverterEngineUnavailable(std::string_view engine_name,
                                       std::string_view reason) override;

    void LogConverterUnsupportedFormat(std::string_view sequence_id,
                                       std::string_view source_path,
                                       std::string_view extension) override;

    // ─── Filesystem Events ─────────────────────────────────────────────────────
    void LogFilesystemReadError(std::string_view sequence_id,
                                std::string_view path,
                                int errno_code,
                                std::string_view operation) override;

    void LogFilesystemWriteError(std::string_view sequence_id,
                                 std::string_view path,
                                 int errno_code,
                                 std::string_view operation) override;

    void LogFilesystemArchiveFailed(std::string_view sequence_id,
                                    std::string_view source,
                                    std::string_view dest,
                                    std::string_view error) override;

    // ─── Hash Events ───────────────────────────────────────────────────────────
    void LogHashVerificationFailed(std::string_view sequence_id,
                                   std::string_view expected_hash,
                                   std::string_view actual_hash,
                                   std::string_view stage) override;

    // ─── Reconversion Events ───────────────────────────────────────────────────
    void LogReconversionLockFailed(std::string_view import_id,
                                   std::string_view holder,
                                   int age_seconds) override;

    void LogReconversionSourceMissing(std::string_view import_id,
                                      std::string_view source_dng_path) override;

    // ─── Rotation Events ───────────────────────────────────────────────────────
    void LogRotationPushFailed(std::string_view import_id,
                               std::string_view sink,
                               std::string_view error) override;

    // ─── Config Events ─────────────────────────────────────────────────────────
    void LogConfigInvalidValue(std::string_view key,
                               std::string_view value,
                               std::string_view reason) override;

    // ─── Watch Events ──────────────────────────────────────────────────────────
    void LogWatchScanFailed(std::string_view watch_dir,
                            std::string_view error) override;

    void LogWatchStabilityTimeout(std::string_view sequence_id,
                                  std::string_view path,
                                  int checks) override;

private:
    std::string logger_name_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Factory that returns a domain::LogSink shared pointer.
std::shared_ptr<domain::LogSink> make_spdlog_log_sink(std::string logger_name = "rawimport");

} // namespace rawimport::infrastructure
