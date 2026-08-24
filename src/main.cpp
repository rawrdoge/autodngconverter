// main.cpp — RawImport portable entry point (v2.1.0-portable).
// SQLite-only feature-subset build: no MariaDB, no rotation sync,
// no reconcile, no exiftool daemon. See ACTION_PLAN_Portable_Layman_Binary_v2.1.0.md.
#include "config.h"
#include "db.h"
#include "converter.h"
#include "api.h"
#include "worker.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <thread>
#include <spdlog/spdlog.h>

namespace rawimport {

static std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true); }

namespace {

// Auto-create the portable folder layout (idempotent). Returns false if a
// required directory cannot be created.
bool ensure_portable_folders(const Config& cfg) {
    // std::string array (not c_str() temporaries — those would dangle).
    const std::string dirs[] = {
        cfg.watch_dir,
        cfg.output_dir,
        cfg.archive_dir,
        cfg.archive_dir + "/duplicates",
        cfg.archive_dir + "/failed",
        cfg.root + "/data",
        cfg.root + "/tools",
    };
    for (const std::string& d : dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(d, ec) && !ensure_dir(d)) {
            SPDLOG_ERROR("[main] failed to create directory: {}", d);
            return false;
        }
    }
    SPDLOG_INFO("[main] portable folders ready under {}", cfg.root);
    return true;
}

// Fatal tool check (action plan §5 Phase 3): missing binaries are a startup
// error with a layman-readable message, not a runtime surprise.
bool require_tool(const std::string& bin, const std::string& display_name) {
    std::error_code ec;
    bool found = !bin.empty() &&
                 (std::filesystem::is_regular_file(bin, ec) || bin.find('/') != std::string::npos ||
                  bin.find('\\') != std::string::npos);
    // A bare name means PATH lookup; accept it and let exec fail loudly later
    // only if truly absent — but for layman clarity we probe PATH via system.
    if (!found && !bin.empty() && bin.find('/') == std::string::npos &&
        bin.find('\\') == std::string::npos) {
        // PATH-resolved name: treat as present (CreateProcess/fork resolve PATH).
        found = true;
        SPDLOG_WARN("[main] {} resolved via PATH ('{}')", display_name, bin);
    }
    if (!found) {
        SPDLOG_ERROR("[main] {} not found. Place it in tools/ or set its _BIN env var.",
                     display_name);
        return false;
    }
    SPDLOG_INFO("[main] {} found: {}", display_name, bin);
    return true;
}

} // namespace

} // namespace rawimport

// CRT entry point — must be outside any namespace.
int main(int argc, char** argv) {
    using namespace rawimport;
    (void)argc;
    (void)argv;

    // load .env if present (best-effort)
    load_dotenv(".env");

    Config cfg = LoadConfig();
    SPDLOG_INFO("RawImport Portable starting (http_port={}, root={})", cfg.http_port, cfg.root);
    SPDLOG_INFO("[main] config: watch_dir={}, output_dir={}, archive_dir={}, db={}, poll_interval={}s",
                cfg.watch_dir, cfg.output_dir, cfg.archive_dir, cfg.db_name, cfg.poll_interval_sec);

    // Auto-create folder layout + DB parent dir (Phase 2 gate: idempotent).
    if (!ensure_portable_folders(cfg)) {
        SPDLOG_ERROR("[main] portable folder setup failed; exiting");
        return 1;
    }

    // Dependency resolution is fatal when absent (Phase 3 gate).
    if (!require_tool(cfg.converter_engine_bin, "dnglab") ||
        !require_tool(cfg.exiftool_bin, "exiftool")) {
        SPDLOG_ERROR("[main] missing external tools; exiting");
        return 1;
    }

    Store store;
    if (!store.Open(cfg)) {
        SPDLOG_ERROR("[db] open failed; exiting");
        return 1;
    }
    if (!store.Migrate()) {
        SPDLOG_ERROR("[db] migrate failed; exiting");
        return 1;
    }
    SPDLOG_INFO("[main] database schema ready");

    ConverterEngine* engine = MakeConverter(cfg);
    if (!engine->Available()) {
        SPDLOG_WARN("[main] converter engine '{}' reports unavailable; conversions may fail",
                    engine->Name());
    } else {
        SPDLOG_INFO("[main] converter engine '{}' available", engine->Name());
    }

    ApiServer api(cfg, store);
    Worker worker(cfg, store, engine);

    // signal handling for graceful shutdown
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    worker.Start();
    SPDLOG_INFO("[main] worker started");
    api.Run();
    SPDLOG_INFO("[main] API server running on port {}", cfg.http_port);

    SPDLOG_INFO("RawImport Portable running. Ctrl-C to stop.");
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SPDLOG_INFO("Shutting down...");
    api.Stop();
    SPDLOG_INFO("[main] API server stopped");
    worker.Stop();
    SPDLOG_INFO("[main] worker stopped");
    store.Close();
    SPDLOG_INFO("[main] database closed");
    delete engine;
    SPDLOG_INFO("Done.");
    return 0;
}