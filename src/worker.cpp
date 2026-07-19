#include "worker.h"
#include "config.h"
#include "db.h"
#include "converter.h"
#include "pipeline.h"
#include "util.h"
#include "metrics.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <queue>
#include <spdlog/spdlog.h>
#include <thread>
#include <unordered_set>

namespace rawimport {
namespace fs = std::filesystem;

struct Worker::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::string> files;
    std::queue<ReconversionJob> reconverts;
    std::unordered_set<std::string> seen;
    std::thread poll_thread;
    std::thread reconvert_thread;
    std::atomic<int> queue_depth{0};
};

Worker::Worker(const Config& cfg, Store& store, ConverterEngine* engine)
    : cfg_(cfg), store_(store), engine_(engine), stop_(false),
      p_(std::make_unique<Impl>()) {}

Worker::~Worker() { Stop(); }

void Worker::Start() {
    p_->poll_thread = std::thread([this]() { PollLoop(); });
    p_->reconvert_thread = std::thread([this]() { ReconvertDrain(); });
}

void Worker::Stop() {
    stop_.store(true);
    p_->cv.notify_all();
    if (p_->poll_thread.joinable()) p_->poll_thread.join();
    if (p_->reconvert_thread.joinable()) p_->reconvert_thread.join();
}

void Worker::QueueReconvert(int64_t import_id, const ReconversionJob& job) {
    std::lock_guard<std::mutex> lk(p_->mtx);
    p_->reconverts.push(job);
    p_->cv.notify_all();
}

int Worker::QueueDepth() const { return p_->queue_depth.load(); }

void Worker::PollLoop() {
    auto last_scan = std::chrono::steady_clock::now() - std::chrono::hours(1);
    while (!stop_.load()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_scan).count();
        if (elapsed >= cfg_.poll_interval_sec) {
            last_scan = now;
            std::error_code ec;
            if (fs::exists(cfg_.watch_dir, ec)) {
                for (const auto& e : fs::directory_iterator(cfg_.watch_dir, ec)) {
                    if (!e.is_regular_file()) continue;
                    std::string ext = e.path().extension().string();
                    for (char& c : ext) c = static_cast<char>(tolower(c));
                    if (ext != ".nrw" && ext != ".nef" && ext != ".cr2" && ext != ".arw")
                        continue;
                    std::string name = e.path().filename().string();
                    if (name.size() >= 4 && (name.substr(name.size() - 4) == ".part" ||
                        name.substr(name.size() - 4) == ".tmp")) continue;
                    // debounce: skip if modified within debounce window
                    auto mtime = fs::last_write_time(e.path(), ec);
                    auto mnow = fs::file_time_type::clock::now();
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(mnow - mtime).count();
                    if (age < cfg_.debounce_sec) continue;
                    {
                        std::lock_guard<std::mutex> lk(p_->mtx);
                        if (p_->seen.count(e.path().string())) continue;
                        p_->seen.insert(e.path().string());
                        p_->files.push(e.path().string());
                        p_->queue_depth.fetch_add(1);
                    }
                }
            }
            p_->cv.notify_all();
        }
        // process one file at a time (serial)
        std::string path;
        {
            std::unique_lock<std::mutex> lk(p_->mtx);
            if (p_->files.empty()) {
                p_->cv.wait_for(lk, std::chrono::seconds(1),
                    [this]() { return stop_.load() || !p_->files.empty(); });
                continue;
            }
            path = p_->files.front();
            p_->files.pop();
            p_->queue_depth.fetch_sub(1);
        }
        ProcessFile(path);
    }
}

void Worker::ProcessFile(const std::string& path) {
    SPDLOG_INFO("[worker] processing {}", path);
    Metrics::instance().inc_files_detected(1);
    auto conv_start = std::chrono::steady_clock::now();
    std::string src_hash = sha256_file(path);
    if (src_hash.empty()) { SPDLOG_ERROR("[worker] hash fail {}", path); return; }
    // duplicate check
    auto dup = store_.GetImportByHash(src_hash);
    if (dup) { SPDLOG_INFO("[worker] duplicate skipped {}", path); return; }

    auto [seq_id, seq_name] = store_.AllocateSequence();
    if (seq_id == 0) { SPDLOG_ERROR("[worker] seq alloc fail"); return; }

    ExifResult ex = extract_exif_date(path, cfg_.exiftool_bin);
    std::string folder = build_folder_schema(cfg_.folder_schema, ex.date);
    std::string out_dir = cfg_.output_dir + "/" + folder;
    ensure_dir(out_dir);
    std::string dst = out_dir + "/" + seq_name + ".dng";

    ConversionSettings settings;
    settings.compression = cfg_.def_compression;
    if (engine_ && engine_->Convert(path, dst, settings)) {
        std::string out_hash = sha256_file(dst);
        ImportRecord rec;
        rec.sequence_id = seq_id;
        rec.sequence_name = seq_name;
        rec.source_path = path;
        rec.source_hash = src_hash;
        rec.output_path = dst;
        rec.output_hash = out_hash;
        rec.camera_model = "unknown";
        rec.capture_date = ex.date;
        rec.capture_time = ex.time;
        rec.folder_schema = folder;
        rec.conversion_settings = "{\"compression\":\"" + settings.compression + "\"}";
        rec.status = ImportStatus::Completed;
        rec.orientation = 0;
        store_.InsertImport(rec);
        // optional sidecar thumbnail (PRD §3.1.7, gated by GEN_THUMB_JPEG)
        if (cfg_.gen_thumb_jpeg) {
            std::string thumb = out_dir + "/" + seq_name + ".thumb.jpg";
            std::string got = extract_thumbnail(dst, thumb);
            if (got.empty()) SPDLOG_WARN("[worker] thumbnail extract failed {}", dst);
            else SPDLOG_INFO("[worker] wrote thumbnail {}", thumb);
        }
        // move source to archive
        std::string arch_dir = cfg_.archive_dir + "/" + folder;
        ensure_dir(arch_dir);
        move_file(path, arch_dir + "/" + fs::path(path).filename().string());
        SPDLOG_INFO("[worker] converted {} -> {}", path, dst);
        auto conv_end = std::chrono::steady_clock::now();
        double dur = std::chrono::duration<double>(conv_end - conv_start).count();
        Metrics::instance().observe_conversion_duration(dur);
        Metrics::instance().inc_conversions_completed("completed", 1);
    } else {
        SPDLOG_ERROR("[worker] conversion failed {}", path);
        Metrics::instance().inc_conversions_completed("failed", 1);
    }
}

void Worker::ReconvertDrain() {
    while (!stop_.load()) {
        ReconversionJob job;
        {
            std::unique_lock<std::mutex> lk(p_->mtx);
            if (p_->reconverts.empty()) {
                p_->cv.wait_for(lk, std::chrono::seconds(1),
                    [this]() { return stop_.load() || !p_->reconverts.empty(); });
                continue;
            }
            job = p_->reconverts.front();
            p_->reconverts.pop();
        }
        // best-effort: re-run conversion on the archived source
        auto rec = store_.GetImportById(job.import_id);
        if (!rec) { SPDLOG_WARN("[worker] reconvert: import {} not found", job.import_id); continue; }
        if (!engine_) continue;
        ConversionSettings s = job.settings;
        if (engine_->Convert(rec->source_path, rec->output_path, s)) {
            std::string h = sha256_file(rec->output_path);
            store_.UpdateOutputHash(rec->id, h);
            store_.UpdateReconversion(job.id, h, ImportStatus::Completed);
            SPDLOG_INFO("[worker] reconverted {}", rec->sequence_name);
        } else {
            store_.UpdateReconversion(job.id, "", ImportStatus::Failed);
            SPDLOG_ERROR("[worker] reconvert failed {}", rec->sequence_name);
        }
    }
}

} // namespace rawimport