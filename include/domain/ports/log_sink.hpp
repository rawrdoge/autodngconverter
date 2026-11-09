/*
 * rawimport-pipeline - LogSink port interface
 * Structured event taxonomy for failure/duplicate logging with correlation IDs.
 */
#pragma once

#include <string>
#include <map>
#include <unordered_map>
#include <string_view>
#include <optional>
#include <cstdint>

#include "domain/types.hpp"

namespace rawimport::domain {

enum class LogLevel : uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Critical = 5 };

class LogSink {
public:
    virtual ~LogSink() = default;

    // ─── Structured Logging ──────────────────────────────────────────────────
    /// Log a structured event with arbitrary key-value fields.
    /// The implementation adds timestamp, level, event name, and correlation_id automatically.
    virtual void LogStructured(std::string_view event,
                               std::string_view level,
                               const std::unordered_map<std::string, std::string>& fields) = 0;

    // ─── Correlation ID ──────────────────────────────────────────────────────
    /// Set thread-local correlation ID (e.g., UUID per import/request).
    virtual void SetCorrelationId(std::string_view id) = 0;
    /// Clear thread-local correlation ID.
    virtual void ClearCorrelationId() = 0;
    /// Get current thread-local correlation ID.
    virtual std::string correlation_id() = 0;

    // ─── Legacy Text Logging (kept for compatibility) ────────────────────────
    virtual void trace(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) = 0;
    virtual void debug(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) = 0;
    virtual void info(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) = 0;
    virtual void warn(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) = 0;
    virtual void error(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) = 0;
    virtual void critical(const std::string& msg, const std::map<std::string, std::string>& ctx = {}) = 0;

    virtual bool       is_enabled(LogLevel level) = 0;
    virtual LogLevel   minimum_level() = 0;

    // ─── Convenience Methods: Event Taxonomy ─────────────────────────────────
    // These call LogStructured internally with the correct event name, level, and fields.

    /// Collision: duplicate by hash, moved to duplicates/
    virtual void LogCollisionDuplicateHash(std::string_view sequence_id,
                                           std::string_view duplicate_of,
                                           std::string_view source_hash,
                                           std::string_view duplicate_path) = 0;

    /// Collision: duplicate by hash, archive failed → dead-lettered to failed/
    virtual void LogCollisionDuplicateHashDeadLetter(std::string_view sequence_id,
                                                     std::string_view duplicate_of) = 0;

    /// Conversion failed: engine error
    virtual void LogConversionFailedEngineError(std::string_view sequence_id,
                                                std::string_view engine_name,
                                                int exit_code,
                                                std::string_view stderr_tail) = 0;

    /// Import failed: conversion or processing error (with human-readable reason)
    virtual void LogImportFailed(std::string_view sequence_id,
                                 std::string_view reason) = 0;

    /// Collision: duplicate by hash (non-fatal, existing record returned)
    virtual void LogCollisionDuplicateFound(std::string_view sequence_id,
                                            std::string_view existing_sequence_id,
                                            const SHA256Hash& source_hash,
                                            std::string_view duplicate_path) = 0;

    /// Conversion failed: hash mismatch (corrupt output)
    virtual void LogConversionFailedHashMismatch(std::string_view sequence_id,
                                                 std::string_view source_hash,
                                                 std::string_view output_hash) = 0;

    /// Conversion failed: timeout
    virtual void LogConversionFailedTimeout(std::string_view sequence_id,
                                            int timeout_sec) = 0;

    /// Dead letter after max retries
    virtual void LogDeadLetterMaxRetries(std::string_view sequence_id,
                                         int max_retries,
                                         std::string_view last_error) = 0;

    /// Watch: directory inaccessible
    virtual void LogWatchDirectoryInaccessible(std::string_view watch_dir,
                                               int errno_code) = 0;

    /// Watch: filesystem full (ENOSPC)
    virtual void LogWatchFilesystemFull(std::string_view watch_dir) = 0;

    /// Database: connection lost
    virtual void LogDbConnectionLost(int attempt, int max_attempts) = 0;

    /// Database: deadlock
    virtual void LogDbDeadlock(std::string_view sequence_id) = 0;

    /// Database: query failed
    virtual void LogDbQueryFailed(std::string_view sequence_id,
                                  std::string_view error) = 0;
};

} // namespace rawimport::domain
