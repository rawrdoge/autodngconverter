/*
 * rawimport-pipeline - FileSystem port interface
 */
#pragma once

#include <string>
#include <vector>
#include <optional>

#include "domain/types.hpp"

namespace rawimport::domain {

class FileSystem {
public:
    virtual ~FileSystem() = default;

    virtual HealthReport  health_check() = 0;

    // Move/rename a file; returns new path. Cross-device fallback applies.
    virtual std::optional<FilePath> move_file(const FilePath& src, const FilePath& dst) = 0;

    // Copy a file; returns true on success.
    virtual bool copy_file(const FilePath& src, const FilePath& dst) = 0;

    // Delete a file; returns true if it existed.
    virtual bool delete_file(const FilePath& path) = 0;

    // SHA-256 of a file (delegates to HashService).
    virtual std::optional<SHA256Hash> sha256_file(const FilePath& path) = 0;

    // Fast fingerprint: size + mtime + FNV-1a of first 4 KiB.
    virtual FastFingerprint compute_fast_fingerprint(const FilePath& path) = 0;

    // List directory entries matching extension filter (e.g. ".NEF").
    // Empty filter matches everything.
    virtual std::vector<FilePath> list_directory(const std::string& dir_path,
                                                  const std::string& extension_filter = "") = 0;

    // Check existence.
    virtual bool exists(const FilePath& path) = 0;

    // Create directory (including parents).
    virtual bool create_directories(const FilePath& path) = 0;
};

} // namespace rawimport::domain
