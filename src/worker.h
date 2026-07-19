#pragma once
// worker.h — single serial converter worker + poller for the C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §3.1, §6.4.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <memory>
#include "config.h"
#include "db.h"
#include "converter.h"

namespace rawimport {

// Watches /watch, debounces, and runs a single serial conversion pipeline.
// Bounded in-memory queue (cfg.queue_size). One job at a time (dnglab/Wine
// not concurrency-safe). Re-conversion drained in background.
class Worker {
public:
    Worker(const Config& cfg, Store& store, ConverterEngine* engine);
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
    void ProcessFile(const std::string& path);
    void ReconvertDrain();

    const Config& cfg_;
    Store& store_;
    ConverterEngine* engine_;
    std::atomic<bool> stop_{false};
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport