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

    // ─── ExifTool Events ───────────────────────────────────────────────────────

    /// ExifTool: metadata extraction failed (crash, timeout, non-zero exit)
    virtual void LogExifExtractionFailed(std::string_view sequence_id,
                                         std::string_view source_path,
                                         std::string_view error,
                                         bool daemon_restarted) = 0;

    /// ExifTool: daemon restarted (health check fail, crash, OOM)
    virtual void LogExifToolDaemonRestarted(std::string_view reason,
                                             int restart_count) = 0;

    // ─── Converter Events ──────────────────────────────────────────────────────

    /// Converter: engine unavailable at startup or first use
    virtual void LogConverterEngineUnavailable(std::string_view engine_name,
                                               std::string_view reason) = 0;

    /// Converter: unsupported file format
    virtual void LogConverterUnsupportedFormat(std::string_view sequence_id,
                                               std::string_view source_path,
                                               std::string_view extension) = 0;

    // ─── Filesystem Events ─────────────────────────────────────────────────────

    /// Filesystem: read/stat/open failed
    virtual void LogFilesystemReadError(std::string_view sequence_id,
                                        std::string_view path,
                                        int errno_code,
                                        std::string_view operation) = 0;

    /// Filesystem: write/rename/mkdir/sync failed
    virtual void LogFilesystemWriteError(std::string_view sequence_id,
                                         std::string_view path,
                                         int errno_code,
                                         std::string_view operation) = 0;

    /// Filesystem: archive/move to failed/ failed
    virtual void LogFilesystemArchiveFailed(std::string_view sequence_id,
                                            std::string_view source,
                                            std::string_view dest,
                                            std::string_view error) = 0;

    // ─── Hash Events ───────────────────────────────────────────────────────────

    /// Hash: post-conversion output hash verification failed
    virtual void LogHashVerificationFailed(std::string_view sequence_id,
                                           std::string_view expected_hash,
                                           std::string_view actual_hash,
                                           std::string_view stage) = 0;

    // ─── Reconversion Events ───────────────────────────────────────────────────

    /// Reconversion: lock acquisition failed
    virtual void LogReconversionLockFailed(std::string_view import_id,
                                           std::string_view holder,
                                           int age_seconds) = 0;

    /// Reconversion: source RAW/DNG missing
    virtual void LogReconversionSourceMissing(std::string_view import_id,
                                              std::string_view source_dng_path) = 0;

    // ─── Rotation Events ───────────────────────────────────────────────────────

    /// Rotation: push to downstream sink (Immich, etc.) failed
    virtual void LogRotationPushFailed(std::string_view import_id,
                                       std::string_view sink,
                                       std::string_view error) = 0;

    // ─── Config Events ─────────────────────────────────────────────────────────

    /// Config: runtime validation failure (invalid port, path, DSN)
    virtual void LogConfigInvalidValue(std::string_view key,
                                       std::string_view value,
                                       std::string_view reason) = 0;

    // ─── Watch Events ──────────────────────────────────────────────────────────

    /// Watch: directory scan failed (permission, unmounted)
    virtual void LogWatchScanFailed(std::string_view watch_dir,
                                    std::string_view error) = 0;

    /// Watch: file stability timeout (incomplete copy)
    virtual void LogWatchStabilityTimeout(std::string_view sequence_id,
                                          std::string_view path,
                                          int checks) = 0;
};

} // namespace rawimport::domain
