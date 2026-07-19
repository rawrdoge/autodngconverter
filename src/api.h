#pragma once
// api.h — REST API server for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §8.
#include <string>
#include <memory>
#include "config.h"
#include "db.h"

namespace rawimport {

// Starts the HTTP server (drogon or cpp-httplib) on cfg.http_port and blocks.
// Routes (PRD §8):
//   GET  /health
//   GET  /api/v1/imports
//   GET  /api/v1/imports/:seq
//   GET  /api/v1/imports/hash/:sha
//   POST /api/v1/imports/:seq/reconvert
//   GET  /api/v1/stats
//   GET  /api/v1/alerts
//   GET  /api/v1/imports/by-source?path=
//   POST /api/v1/imports/by-path/preview-updated
//   POST /api/v1/imports/by-source/rotation-updated   (NEW, rotation sync)
//
// rotation_mgr may be nullptr if rotation sync is disabled; the endpoint then
// returns 503.
class ApiServer {
public:
    ApiServer(const Config& cfg, Store& store, class RotationManager* rotation_mgr);
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