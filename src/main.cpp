// main.cpp — RawImport Pipeline C++20 rewrite entry point.
// Phase 3 serial integration (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md.
#include "config.h"
#include "db.h"
#include "converter.h"
#include "cct_engine.h"
#include "cct_worker.h"
#include "rotation.h"
#include "reconcile.h"
#include "api.h"
#include "worker.h"
#include "util.h"

#include <atomic>
#include <csignal>
#include <thread>
#include <spdlog/spdlog.h>

namespace rawimport {

static std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true); }

} // namespace rawimport

int main(int argc, char** argv) {
    using namespace rawimport;

    // load .env if present (best-effort)
    load_dotenv(".env");

    Config cfg = LoadConfig();
    SPDLOG_INFO("RawImport C++ starting (http_port={})", cfg.http_port);
    SPDLOG_INFO("[main] config: watch_dir={}, output_dir={}, archive_dir={}, db_host={}, poll_interval={}s, debounce={}s",
                cfg.watch_dir, cfg.output_dir, cfg.archive_dir, cfg.db_host, cfg.poll_interval_sec, cfg.debounce_sec);

    Store store;
    if (!store.Open(cfg)) {
        SPDLOG_ERROR("DB open failed; exiting");
        return 1;
    }
    if (!store.Migrate(cfg.db_dir.empty() ? "migrations" : cfg.db_dir + "/migrations")) {
        SPDLOG_ERROR("DB migrate failed; exiting");
        return 1;
    }
    SPDLOG_INFO("[main] database migration complete");

    // reconcile existing library (legacy placeholders + sequence reserve)
    ReconcileLibrary(cfg, store);
    SPDLOG_INFO("[main] library reconciliation complete");

    // build engine + embedder
    ConverterEngine* engine = MakeConverter(cfg);
    if (engine && !engine->Available()) {
        SPDLOG_WARN("converter engine '{}' not available; conversions will fail",
                     engine->Name());
    } else if (engine) {
        SPDLOG_INFO("[main] converter engine '{}' available", engine->Name());
    }

    PreviewEmbedder* embedder = MakeEmbedder(cfg);
    if (embedder) {
        SPDLOG_INFO("[main] preview embedder '{}' available", embedder->Name());
    }

    // CCT analysis plugin (PRD-CCT-001 §11): optional. Degrades gracefully —
    // unknown engine name or missing python3/rawpy simply disables /api/v1/cct/*.
    CctEngine* cct_engine = nullptr;
    CctWorker* cct_worker = nullptr;
    if (cfg.cct_enabled) {
        cct_engine = MakeCctEngine(cfg.cct_engine);
        if (!cct_engine) {
            SPDLOG_ERROR("[main] unknown CCT_ENGINE '{}'; CCT disabled",
                         cfg.cct_engine);
        } else if (!cct_engine->Available()) {
            SPDLOG_WARN("[main] CCT engine '{}' unavailable "
                        "(python3 with rawpy/colour/numpy required); CCT disabled",
                        cfg.cct_engine);
            delete cct_engine;
            cct_engine = nullptr;
        } else {
            SPDLOG_INFO("[main] CCT engine '{}' available", cfg.cct_engine);
        }
    }
    if (cct_engine) cct_worker = new CctWorker(store, cct_engine);

    RotationManager rotation(cfg, store);
    ApiServer api(cfg, store, &rotation, cct_worker);
    Worker worker(cfg, store, engine, embedder);

    // signal handling for graceful shutdown
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    worker.Start();
    SPDLOG_INFO("[main] worker started");
    if (cct_worker) {
        cct_worker->Start();
        SPDLOG_INFO("[main] cct worker started");
    }
    api.Run();
    SPDLOG_INFO("[main] API server running on port {}", cfg.http_port);

    SPDLOG_INFO("RawImport C++ running. Ctrl-C to stop.");
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SPDLOG_INFO("Shutting down...");
    api.Stop();
    SPDLOG_INFO("[main] API server stopped");
    worker.Stop();
    SPDLOG_INFO("[main] worker stopped");
    rotation.Stop();
    SPDLOG_INFO("[main] rotation manager stopped");
    if (cct_worker) {
        cct_worker->Stop();
        delete cct_worker;
        SPDLOG_INFO("[main] cct worker stopped");
    }
    delete cct_engine;
    store.Close();
    SPDLOG_INFO("[main] database connection closed");
    delete engine;
    SPDLOG_INFO("Done.");
    return 0;
}
