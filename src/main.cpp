// main.cpp — RawImport Pipeline C++20 rewrite entry point.
// Phase 3 serial integration (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md.
#include "config.h"
#include "db.h"
#include "converter.h"
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

    Store store;
    if (!store.Open(cfg)) {
        SPDLOG_ERROR("DB open failed; exiting");
        return 1;
    }
    if (!store.Migrate(cfg.db_dir.empty() ? "migrations" : cfg.db_dir + "/migrations")) {
        SPDLOG_ERROR("DB migrate failed; exiting");
        return 1;
    }

    // reconcile existing library (legacy placeholders + sequence reserve)
    ReconcileLibrary(cfg, store);

    // build engine + embedder
    ConverterEngine* engine = MakeConverter(cfg.converter_engine);
    if (engine && !engine->Available()) {
        SPDLOG_WARN("converter engine '{}' not available; conversions will fail",
                     engine->Name());
    }

    RotationManager rotation(cfg, store);
    ApiServer api(cfg, store, &rotation);
    Worker worker(cfg, store, engine);

    // signal handling for graceful shutdown
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    worker.Start();
    api.Run();

    SPDLOG_INFO("RawImport C++ running. Ctrl-C to stop.");
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SPDLOG_INFO("Shutting down...");
    api.Stop();
    worker.Stop();
    rotation.Stop();
    store.Close();
    delete engine;
    SPDLOG_INFO("Done.");
    return 0;
}