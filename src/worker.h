#pragma once
// worker.h — parallel converter worker pool + poller for the portable build.
// Watches watch/, debounces, and runs a parallel conversion pipeline.
// Bounded in-memory queue (cfg.queue_size). Up to MAX_CONVERTER_WORKERS
// concurrent dnglab jobs. One-shot exiftool per file (no daemon).
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <memory>
#include <unordered_map>
#include <chrono>
#include "config.h"
#include "db.h"
#include "converter.h"

namespace rawimport {

class Worker {
public:
    Worker(const Config& cfg, Store& store, ConverterEngine* engine);
    ~Worker();

    // Start the poll loop + background threads. Non-blocking.
    void Start();

    // Signal graceful shutdown; joins threads.
    void Stop();

    // Approximate current queue depth (for /stats).
    int QueueDepth() const;

private:
    void PollLoop();
    void ProcessFile(const std::string& path, Store& store);
    void WorkerThread();
    void cleanup_seen_ttl();
    void evict_fp_cache();  // caller must hold p_->mtx
    void release_inflight(const std::string& path);
    void move_to_dead_letter(const std::string& path, Store& store);

    const Config& cfg_;
    Store& store_;
    ConverterEngine* engine_;
    std::atomic<bool> stop_{false};
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport
