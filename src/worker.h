#pragma once
// worker.h — parallel converter worker pool + poller for the C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §3.1, §6.4.
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
#include "exiftool_daemon.h"

namespace rawimport {

// Watches /watch, debounces, and runs a parallel conversion pipeline.
// Bounded in-memory queue (cfg.queue_size). Up to MAX_CONVERTER_WORKERS
// concurrent jobs for dnglab engine; forced to 1 for adobedng.
// Re-conversion drained in background.
class Worker {
public:
    Worker(const Config& cfg, Store& store, ConverterEngine* engine, PreviewEmbedder* embedder);
    ~Worker();

    // Start the poll loop + background threads. Non-blocking.
    void Start();

    // Signal graceful shutdown; joins threads.
    void Stop();

    // Queue a reconversion job (drained by background thread).
    void QueueReconvert(int64_t import_id, const ReconversionJob& job);

    // Approximate current queue depth (for /stats).
    int QueueDepth() const;

private:
    void PollLoop();
    void ProcessFile(const std::string& path, Store& store);
    void ReconvertDrain();
    void WorkerThread();
    void cleanup_seen_ttl();
    void evict_fp_cache();  // caller must hold p_->mtx
    void move_to_dead_letter(const std::string& path, Store& store);

    const Config& cfg_;
    Store& store_;
    ConverterEngine* engine_;
    PreviewEmbedder* embedder_;
    std::unique_ptr<ExifToolDaemon> exif_daemon_;
    std::atomic<bool> stop_{false};
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport
