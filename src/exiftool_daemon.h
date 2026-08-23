#pragma once
// exiftool_daemon.h — Persistent ExifTool subprocess for fast EXIF extraction.
// Uses -stay_open -@ - protocol with plain text tag extraction.

#include <cstdint>
#include <cstdio>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include "pipeline.h"

namespace rawimport {

class ExifToolDaemon {
public:
    ExifToolDaemon(const std::string& exiftool_bin);
    ~ExifToolDaemon();
    
    // Non-copyable, movable
    ExifToolDaemon(const ExifToolDaemon&) = delete;
    ExifToolDaemon& operator=(const ExifToolDaemon&) = delete;
    ExifToolDaemon(ExifToolDaemon&&) = default;
    ExifToolDaemon& operator=(ExifToolDaemon&&) = default;
    
    // Extract DateTimeOriginal from a file using the daemon.
    // Falls back to mtime if daemon unavailable or extraction fails.
    ExifResult extract_date(const std::string& path);
    
    // Check if daemon is running and responsive.
    bool healthy() const { return running_.load(); }
    
    // Shutdown the daemon.
    void stop();

private:
    std::string exiftool_bin_;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    FILE* stdout_f_ = nullptr;  // single long-lived buffered stream over stdout_fd_
    std::int64_t child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    
    bool start();
    std::string send_command(const std::string& cmd);
};

} // namespace rawimport
