#pragma once
// rotation.h — Cross-App Rotation Sync manager for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §5.
#include <cstdint>
#include <chrono>
#include <string>
#include <memory>
#include "config.h"
#include "db.h"

namespace rawimport {

// RotationManager coalesces spammy rotation intents via a per-image grace timer,
// then dispatches exactly one job that writes EXIF Orientation to the DNG and
// propagates to Immich/digiKam under a processing_locks guard.
class RotationManager {
public:
    RotationManager(const Config& cfg, Store& store);
    ~RotationManager();

    // Queue an intent for an import. Resets the grace timer for that import.
    // orientation must be 1-8.
    void Queue(int64_t import_id, const std::string& dng_path, int orientation,
               const std::string& client_id);

    // Stop all timers + dispatch pending (used on shutdown).
    void Stop();

private:
    struct PendingRot {
        int orientation = 1;
        std::string dng_path;
        std::string client_id;
        std::chrono::steady_clock::time_point deadline;
    };
    // Internal dispatch (runs on timer fire): coalesced single job.
    void Dispatch(int64_t import_id, const PendingRot& rot);

    const Config& cfg_;
    Store& store_;
    // pImpl to hide std::mutex / std::unordered_map / timer state from agents.
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport