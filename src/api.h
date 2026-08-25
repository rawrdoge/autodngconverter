#pragma once
// api.h — REST API server for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §8.
#include <string>
#include <memory>
#include "config.h"
#include "db.h"

namespace rawimport {

class CctWorker;
class RotationManager;

// Routes (PRD §8 + PRD-CCT-001 §8):
//   GET  /health
//   GET  /ready
//   GET  /metrics
//   GET  /api/v1/stats
//   GET  /api/v1/alerts
//   GET  /api/v1/imports            (+ :seq, /hash/:sha, by-source variants)
//   POST /api/v1/imports/:seq/reconvert
//   POST /api/v1/imports/by-path/preview-updated
//   POST /api/v1/imports/by-source/rotation-updated
//   POST /api/v1/cct/analyze        (requires cct_worker)
//   GET  /api/v1/cct/result         (requires cct_worker)
//   GET  /api/v1/cct/list           (requires cct_worker)
//
// cct_worker may be nullptr when CCT is disabled/undeployable; the /cct/*
// endpoints then answer 503.
class ApiServer {
public:
    ApiServer(const Config& cfg, Store& store,
              RotationManager* rotation_mgr = nullptr,
              CctWorker* cct_worker = nullptr);
    ~ApiServer();

    // Run the server loop (blocking). Returns on stop signal.
    void Run();

    // Signal graceful stop.
    void Stop();

    // api.cpp defines a free function handle() that needs access to Impl.
    // Impl is public so the anonymous-namespace handle() in api.cpp can use it.
    struct Impl;

private:
    const Config& cfg_;
    Store& store_;
    RotationManager* rotation_mgr_;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport