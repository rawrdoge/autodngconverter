#pragma once
// api.h — REST API server for the RawImport portable build.
// Subset per action plan §4.1: GET /health, /ready, /metrics, /api/v1/stats,
// GET /api/v1/imports. No rotation, reconvert, alerts, or Lua-plugin routes.
#include <string>
#include <memory>
#include "config.h"
#include "db.h"

namespace rawimport {

// Starts the HTTP server on cfg.http_port. Run() starts the accept thread and
// returns; Stop() signals graceful shutdown.
class ApiServer {
public:
    ApiServer(const Config& cfg, Store& store);
    ~ApiServer();

    // Start the server loop. Returns immediately.
    void Run();

    // Signal graceful stop.
    void Stop();

    // api.cpp defines a free function handle() that needs access to Impl;
    // it is public for that reason only.
    struct Impl;

private:
    const Config& cfg_;
    Store& store_;
    std::unique_ptr<Impl> p_;
};

} // namespace rawimport