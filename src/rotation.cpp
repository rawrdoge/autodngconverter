#include "rotation.h"
#include "config.h"
#include "db.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>
#include <unordered_map>

namespace rawimport {

struct RotationManager::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::unordered_map<int64_t, PendingRot> pending;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::string exiftool_bin = "exiftool";
    int grace_ms = 1500;
    std::string immich_url;
    bool digikam_rescan = false;
};

RotationManager::RotationManager(const Config& cfg, Store& store)
    : cfg_(cfg), store_(store), p_(std::make_unique<Impl>()) {
    p_->exiftool_bin = cfg.exiftool_bin.empty() ? "exiftool" : cfg.exiftool_bin;
    p_->grace_ms = cfg.rotation_grace_ms > 0 ? cfg.rotation_grace_ms : 1500;
    p_->immich_url = cfg.immich_url;
    p_->digikam_rescan = (cfg.digikam_rescan == "dbus");
    p_->worker = std::thread([this]() {
        while (!p_->stop.load()) {
            std::unique_lock<std::mutex> lk(p_->mtx);
            if (p_->pending.empty()) {
                p_->cv.wait(lk, [this]() { return p_->stop.load() || !p_->pending.empty(); });
                continue;
            }
            // find the soonest absolute deadline among pending
            auto soonest_tp = std::chrono::steady_clock::time_point::max();
            for (auto& kv : p_->pending) {
                if (kv.second.deadline < soonest_tp) soonest_tp = kv.second.deadline;
            }
            // wait until that deadline (or stop/notify)
            p_->cv.wait_until(lk, soonest_tp, [this]() { return p_->stop.load(); });
            // dispatch all that are due (deadline <= now)
            auto now = std::chrono::steady_clock::now();
            for (auto it = p_->pending.begin(); it != p_->pending.end(); ) {
                if (it->second.deadline <= now) {
                    int64_t id = it->first;
                    PendingRot rot = it->second;
                    it = p_->pending.erase(it);
                    lk.unlock();
                    Dispatch(id, rot);
                    lk.lock();
                } else {
                    ++it;
                }
            }
        }
    });
}

RotationManager::~RotationManager() { Stop(); }

void RotationManager::Queue(int64_t import_id, const std::string& dng_path,
                            int orientation, const std::string& client_id) {
    std::unique_lock<std::mutex> lk(p_->mtx);
    PendingRot r;
    r.orientation = orientation;
    r.dng_path = dng_path;
    r.client_id = client_id;
    r.deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(p_->grace_ms);
    p_->pending[import_id] = r;
    lk.unlock();
    p_->cv.notify_all();
    SPDLOG_INFO("[rotation] queued import_id={} orient={} client={}", import_id, orientation, client_id);
}

void RotationManager::Dispatch(int64_t import_id, const PendingRot& rot) {
    if (!store_.AcquireLock(import_id, "rotation-mgr", 30)) {
        SPDLOG_WARN("[rotation] lock busy for import_id={}, skip", import_id);
        return;
    }
    // write EXIF orientation via exiftool
    std::string cmd = p_->exiftool_bin + " -Orientation=" + std::to_string(rot.orientation) +
        " -n -overwrite_original_in_place " + rot.dng_path;
#ifdef _WIN32
    int r = system(cmd.c_str());
#else
    int r = std::system(cmd.c_str());
#endif
    if (r != 0) {
        SPDLOG_ERROR("[rotation] exiftool failed for {} (rc={})", rot.dng_path, r);
    } else {
        store_.UpdateOrientation(import_id, rot.orientation);
        SPDLOG_INFO("[rotation] wrote orientation {} to {}", rot.orientation, rot.dng_path);
    }
    if (!p_->immich_url.empty()) {
        // best-effort Immich webhook (ignore errors)
        std::string post = "curl -s -o /dev/null -X POST \"" + p_->immich_url +
            "/api/jobs\" -H \"Content-Type: application/json\" -d '{\"type\":\"sidecar\"}'";
#ifdef _WIN32
        system(post.c_str());
#else
        std::system(post.c_str());
#endif
    }
    if (p_->digikam_rescan) {
        // best-effort: touch a rescan trigger file
        std::string touch = "touch /tmp/digikam_rescan.trigger";
#ifdef _WIN32
        system(touch.c_str());
#else
        std::system(touch.c_str());
#endif
    }
    store_.ReleaseLock(import_id);
}

void RotationManager::Stop() {
    if (p_->stop.exchange(true)) return;
    p_->cv.notify_all();
    if (p_->worker.joinable()) p_->worker.join();
    // dispatch remaining pending immediately
    std::unordered_map<int64_t, PendingRot> remaining;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        remaining = std::move(p_->pending);
        p_->pending.clear();
    }
    for (auto& kv : remaining) {
        PendingRot rot = kv.second;
        if (store_.AcquireLock(kv.first, "rotation-mgr", 30)) {
            std::string cmd = p_->exiftool_bin + " -Orientation=" + std::to_string(rot.orientation) +
                " -n -overwrite_original_in_place " + rot.dng_path;
#ifdef _WIN32
            system(cmd.c_str());
#else
            std::system(cmd.c_str());
#endif
            store_.UpdateOrientation(kv.first, rot.orientation);
            store_.ReleaseLock(kv.first);
        }
    }
    SPDLOG_INFO("[rotation] stopped, {} pending dispatched", remaining.size());
}

} // namespace rawimport