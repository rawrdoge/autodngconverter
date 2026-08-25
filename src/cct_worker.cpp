// cct_worker.cpp — CCT analysis job consumer implementation (PRD-CCT-001 §7.3).
// Mirrors the Worker pattern: mutex + condition variable queue, single drain
// thread, atomic stop flag. Analysis runs on this thread only.
#include "cct_worker.h"

#include "cct_engine.h"
#include "db.h"
#include "metrics.h"

#include <chrono>
#include <spdlog/spdlog.h>

namespace rawimport {

CctWorker::CctWorker(Store& store, CctEngine* engine)
    : store_(store), engine_(engine), max_queue_(50) {}

CctWorker::~CctWorker() { Stop(); }

void CctWorker::Start() {
    stop_.store(false);
    if (!thread_.joinable()) {
        thread_ = std::thread([this]() { Drain(); });
        SPDLOG_INFO("[cct] analysis worker started (engine: {})",
                    engine_ ? engine_->Name() : "none");
    }
}

void CctWorker::Stop() {
    stop_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    SPDLOG_INFO("[cct] analysis worker stopped");
}

bool CctWorker::Queue(const Job& job) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (jobs_.size() >= max_queue_) {
            SPDLOG_WARN("[cct] queue full ({}); dropping job for {}",
                        max_queue_, job.sequence_name);
            return false;
        }
        jobs_.push(job);
    }
    cv_.notify_one();
    Metrics::instance().set_queue_depth(static_cast<int64_t>(jobs_.size()));
    return true;
}

void CctWorker::Drain() {
    while (!stop_.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::seconds(1),
                         [this]() { return stop_.load() || !jobs_.empty(); });
            if (stop_.load() && jobs_.empty()) break;
            if (jobs_.empty()) continue;
            job = jobs_.front();
            jobs_.pop();
            Metrics::instance().set_queue_depth(
                static_cast<int64_t>(jobs_.size()));
        }
        Process(job);
    }
}

void CctWorker::Process(const Job& job) {
    // 1. Mark running.
    Store::CctResult running;
    running.id = job.id;
    running.status = "running";
    if (!store_.UpdateCctResult(running.id, running)) {
        SPDLOG_WARN("[cct] could not mark job {} running", job.id);
    }

    if (!engine_) {
        Store::CctResult failed;
        failed.id = job.id;
        failed.status = "failed";
        failed.error_msg = "no CCT engine available";
        store_.UpdateCctResult(failed.id, failed);
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    // 2. Run analysis (blocking; rawpy handles decode internally).
    CctOutput out = engine_->Analyze({job.raw_path, job.dng_path,
                                      job.sampled_area, job.algorithm});
    double dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // 3. Persist completed or failed.
    Store::CctResult rec;
    rec.id = job.id;
    rec.import_id = job.import_id;
    rec.sequence_name = job.sequence_name;
    rec.algorithm = job.algorithm;
    rec.sampled_area = job.sampled_area;
    if (out.error.empty()) {
        rec.status = "completed";
        rec.cct_kelvin = out.cct_kelvin;
        rec.tint = out.tint;
        rec.xy_x = out.xy_x;
        rec.xy_y = out.xy_y;
        rec.hue = out.hue;
        rec.chroma = out.chroma;
        rec.confidence = out.confidence;
        SPDLOG_INFO("[cct] {} -> {:.0f}K tint={:.2f} conf={:.2f} ({:.1f}s)",
                    job.sequence_name, out.cct_kelvin, out.tint,
                    out.confidence, dur);
    } else {
        rec.status = "failed";
        rec.error_msg = out.error;
        SPDLOG_ERROR("[cct] {} failed: {}", job.sequence_name, out.error);
    }
    if (!store_.UpdateCctResult(rec.id, rec)) {
        SPDLOG_ERROR("[cct] failed to persist result for {}", job.sequence_name);
    }
}

} // namespace rawimport