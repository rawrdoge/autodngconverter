#pragma once
// cct_worker.h — asynchronous CCT analysis job consumer (PRD-CCT-001 §7).
// Single background drain thread; bounded queue; graceful stop. Jobs are
// persisted in cct_results and transition pending → running → completed|failed.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "db.h"

namespace rawimport {

class CctEngine;

class CctWorker {
public:
    struct Job {
        int64_t id = 0;
        int64_t import_id = 0;
        std::string sequence_name;
        std::string algorithm;
        std::string sampled_area;
        std::string raw_path;
        std::string dng_path;
    };

    // engine must outlive the worker; ownership stays with the caller
    // (main creates it via MakeCctEngine and deletes it at shutdown).
    CctWorker(Store& store, CctEngine* engine);
    ~CctWorker();

    void Start();
    void Stop();   // joins the drain thread after finishing the current job

    // Queue a job for analysis. Returns false if the queue is full.
    bool Queue(const Job& job);

private:
    void Drain();
    void Process(const Job& job);

    Store& store_;
    CctEngine* engine_;
    size_t max_queue_;
    std::queue<Job> jobs_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace rawimport