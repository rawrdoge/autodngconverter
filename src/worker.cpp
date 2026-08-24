#include "worker.h"
#include "config.h"
#include "db.h"
#include "converter.h"
#include "pipeline.h"
#include "util.h"
#include "metrics.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <queue>
#include <spdlog/spdlog.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>

namespace rawimport {
namespace fs = std::filesystem;

namespace {

// One-shot EXIF extraction (action plan §5 Phase 4). No daemon: run
// `exiftool -DateTimeOriginal -S <path>` per file, parse the strict
// "YYYY:MM:DD HH:MM:SS" shape, and fall back to stat() mtime otherwise.
ExifResult exif_one_shot(const std::string& path, const std::string& exiftool_bin) {
    ExifResult r = exif_from_mtime(path);

    std::string cmd = "\"" + exiftool_bin + "\" -DateTimeOriginal -S \"" + path + "\"";
#ifdef _WIN32
    FILE* f = _popen(cmd.c_str(), "r");
#else
    FILE* f = popen(cmd.c_str(), "r");
#endif
    if (!f) return r;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        // format: "DateTimeOriginal: 2026:08:09 18:23:08"
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        std::string val = trim(s.substr(colon + 1));
        if (val.size() >= 19 && val[4] == ':' && val[7] == ':' && val[10] == ' ' &&
            val[13] == ':' && val[16] == ':') {
            r.date = val.substr(0, 4) + "-" + val.substr(5, 2) + "-" + val.substr(8, 2);
            r.time = val.substr(11, 8);
            r.source = DateSource::Exif;
            break;
        }
    }
#ifdef _WIN32
    _pclose(f);
#else
    pclose(f);
#endif
    return r;
}

} // namespace

struct Worker::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::string> files;
    // TTL map: path -> last_seen_time (evicted at 2 x poll_interval, PRD §6.2).
    // Failure counts live in fail_counts below so they SURVIVE TTL eviction;
    // keeping them here reset the dead-letter counter every eviction window
    // and made §6.3's infinite-retry bug survive the original fix.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> seen;
    // Persistent per-file failure counter (PRD §6.3). Erased only on success
    // or when the file is dead-lettered / disappears from /watch.
    std::unordered_map<std::string, int> fail_counts;
    // Files currently owned by a worker thread (D24): the poller must not
    // requeue them mid-conversion.
    std::unordered_set<std::string> inflight;
    // Fast-fingerprint cache (PRD §5.3 / amendment ruling #2): path -> last
    // known (size, mtime, FNV-1a-of-first-4KB) plus the SHA-256 computed at
    // that time. A matching fingerprint proves the file is byte-identical to
    // what we already hashed, so the full SHA-256 read can be skipped.
    struct FpEntry {
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint64_t fnv = 0;
        std::string sha256;
        std::chrono::steady_clock::time_point last_seen;
    };
    std::unordered_map<std::string, FpEntry> fp_cache;
    std::thread poll_thread;
    std::vector<std::thread> worker_threads;
    std::atomic<int> queue_depth{0};
    std::atomic<int> active_workers{0};
};

Worker::Worker(const Config& cfg, Store& store, ConverterEngine* engine)
    : cfg_(cfg), store_(store), engine_(engine), stop_(false),
      p_(std::make_unique<Impl>()) {}

Worker::~Worker() { Stop(); }

void Worker::Start() {
    p_->poll_thread = std::thread([this]() { PollLoop(); });

    // Determine number of converter workers
    int max_workers = cfg_.max_converter_workers;
    if (max_workers <= 0) {
        max_workers = std::thread::hardware_concurrency();
        if (max_workers <= 0) max_workers = 4;
    }
    // Cap at 8
    if (max_workers > 8) max_workers = 8;

    SPDLOG_INFO("[worker] starting {} converter worker threads", max_workers);

    // Each thread creates its own DB connection inside WorkerThread() (PRD §7.1)
    for (int i = 0; i < max_workers; ++i) {
        p_->worker_threads.emplace_back([this]() { WorkerThread(); });
    }
}

void Worker::Stop() {
    stop_.store(true);
    p_->cv.notify_all();

    if (p_->poll_thread.joinable()) p_->poll_thread.join();

    for (auto& t : p_->worker_threads) {
        if (t.joinable()) t.join();
    }
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
                int scanned = 0, queued = 0;
                for (const auto& e : fs::directory_iterator(cfg_.watch_dir, ec)) {
                    if (!e.is_regular_file()) continue;
                    ++scanned;
                    std::string ext = e.path().extension().string();
                    for (char& c : ext) c = static_cast<char>(tolower(c));
                    if (ext != ".nrw" && ext != ".nef" && ext != ".cr2" && ext != ".arw")
                        continue;
                    std::string name = e.path().filename().string();
                    // Skip DigiKam in-progress temp files
                    if (name.find(".digikamtempfile.") != std::string::npos) continue;
                    // Skip .part, .tmp, .download
                    if (name.size() >= 4 && (name.substr(name.size() - 4) == ".part" ||
                        name.substr(name.size() - 4) == ".tmp")) continue;
                    if (name.size() >= 9 && name.substr(name.size() - 9) == ".download") continue;
                    // debounce: skip if modified within debounce window
                    auto mtime = fs::last_write_time(e.path(), ec);
                    auto mnow = fs::file_time_type::clock::now();
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(mnow - mtime).count();
                    if (age < cfg_.debounce_sec) continue;
                    {
                        std::lock_guard<std::mutex> lk(p_->mtx);
                        std::string path = e.path().string();
                        // Skip files a worker is actively processing (D24):
                        // without this, slow conversions get requeued
                        // mid-flight and a second thread races on the source.
                        if (p_->inflight.count(path)) continue;
                        auto it = p_->seen.find(path);
                        if (it != p_->seen.end()) {
                            // Already known: do NOT refresh last_seen.
                            // Refreshing here would keep entries alive forever
                            // for files that stay in /watch (e.g. after a
                            // failure), starving the dead-letter retry cycle.
                            // TTL expiry is what triggers the next retry.
                            continue;
                        }
                        p_->seen.emplace(path, now);
                        p_->files.push(path);
                        p_->queue_depth.fetch_add(1);
                        ++queued;
                        SPDLOG_DEBUG("[worker] queued {}", path);
                    }
                }
                if (scanned > 0) {
                    SPDLOG_DEBUG("[worker] scan complete: {} files scanned, {} queued, {} skipped", scanned, queued, scanned - queued);
                }
            } else {
                SPDLOG_WARN("[worker] watch dir does not exist or not accessible: {} (err: {})", cfg_.watch_dir, ec.message());
            }
            p_->cv.notify_all();
        }
        
        // Cleanup TTL entries periodically
        cleanup_seen_ttl();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Worker::cleanup_seen_ttl() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(p_->mtx);
    auto ttl = std::chrono::seconds(2 * cfg_.poll_interval_sec);
    for (auto it = p_->seen.begin(); it != p_->seen.end();) {
        if (now - it->second > ttl) {
            it = p_->seen.erase(it);
        } else {
            ++it;
        }
    }
}

void Worker::WorkerThread() {
    // Each worker thread owns its own DB connection: SQLite connections are
    // not safe for concurrent use across threads by default.
    sqlite3* conn = Store::CreateConnection(cfg_);
    if (!conn) {
        SPDLOG_ERROR("[worker] per-worker DB connection failed; worker thread exiting");
        return;
    }
    Store worker_store(conn);  // adopts connection; closed on destruction

    while (!stop_.load()) {
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
            p_->active_workers.fetch_add(1);
            p_->inflight.insert(path);
        }
        
        ProcessFile(path, worker_store);
        
        p_->active_workers.fetch_sub(1);
    }
}

void Worker::ProcessFile(const std::string& path, Store& store) {
    SPDLOG_INFO("[worker] processing {}", path);
    Metrics::instance().inc_files_detected(1);
    
    // Stages 1+2: fast-fingerprint gate (PRD §5.3), then SHA-256 only when
    // needed. A fingerprint hit reuses the previously computed hash, skipping
    // the ~50–100 ms full-file read for unchanged files (amendment §7.2).
    auto fp_start = std::chrono::steady_clock::now();
    FastFingerprint fp;             // zeroed unless FAST_FINGERPRINT is enabled
    bool need_sha = true;
    std::string src_hash;
    if (cfg_.fast_fingerprint) {
        fp = compute_fast_fingerprint(path);
        {
            std::lock_guard<std::mutex> lk(p_->mtx);
            auto it = p_->fp_cache.find(path);
            if (it != p_->fp_cache.end() && it->second.size == fp.size &&
                it->second.mtime == fp.mtime && it->second.fnv == fp.fnv1a_4k) {
                src_hash = it->second.sha256;
                it->second.last_seen = std::chrono::steady_clock::now();
                need_sha = false;
                SPDLOG_DEBUG("[worker] fingerprint hit, reusing sha256 for {}", path);
            }
        }
    }
    double fp_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - fp_start).count();
    Metrics::instance().observe_fast_fingerprint_duration(fp_dur);

    if (need_sha) {
        auto hash_start = std::chrono::steady_clock::now();
        src_hash = sha256_file(path);
        double hash_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - hash_start).count();
        Metrics::instance().observe_hash_duration(hash_dur);

        if (!src_hash.empty() && cfg_.fast_fingerprint) {
            std::lock_guard<std::mutex> lk(p_->mtx);
            auto& e = p_->fp_cache[path];
            e.size = fp.size;
            e.mtime = fp.mtime;
            e.fnv = fp.fnv1a_4k;
            e.sha256 = src_hash;
            e.last_seen = std::chrono::steady_clock::now();
            evict_fp_cache();
        }
    }

    if (src_hash.empty()) {
        SPDLOG_ERROR("[worker] hash fail {}", path);
        move_to_dead_letter(path, store);
        return;
    }
    
    // Duplicate check
    auto dup = store.GetImportByHash(src_hash);
    if (dup) {
        SPDLOG_INFO("[worker] duplicate skipped {}", path);
        {
            // a dedup hit also resolves any prior failure history (A4)
            std::lock_guard<std::mutex> lk(p_->mtx);
            p_->fail_counts.erase(path);
        }
        // Archive the original instead of deleting it (data-loss fix, A6).
        std::string dup_dir = cfg_.archive_dir + "/duplicates";
        ensure_dir(dup_dir);
        std::string dup_path = dup_dir + "/" + fs::path(path).filename().string();
        if (move_file(path, dup_path)) {
            SPDLOG_INFO("[worker] duplicate archived {}", dup_path);
        } else {
            SPDLOG_WARN("[worker] duplicate left in place (archive move failed): {}", path);
        }
        release_inflight(path);
        return;
    }
    
    // Stage 3: EXIF extraction (one-shot exiftool, mtime fallback inside).
    auto exif_start = std::chrono::steady_clock::now();
    ExifResult ex = exif_one_shot(path, cfg_.exiftool_bin);
    double exif_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - exif_start).count();
    Metrics::instance().observe_exif_duration(exif_dur);
    
    // Stage 4: Sequence allocation
    auto seq_start = std::chrono::steady_clock::now();
    auto [seq_id, seq_name] = store.AllocateSequence();
    double seq_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - seq_start).count();
    Metrics::instance().observe_db_duration(seq_dur);
    
    if (seq_id == 0) {
        SPDLOG_ERROR("[worker] seq alloc fail");
        move_to_dead_letter(path, store);
        return;
    }
    
    std::string folder = build_folder_schema(cfg_.folder_schema, ex.date);
    std::string out_dir = cfg_.output_dir + "/" + folder;
    ensure_dir(out_dir);
    std::string dst = out_dir + "/" + seq_name + ".dng";
    
    ConversionSettings settings;
    settings.compression = cfg_.def_compression;
    
    // Stage 5: Conversion
    auto conv_start = std::chrono::steady_clock::now();
    bool conv_ok = false;
    if (engine_ && engine_->Convert(path, dst, settings)) {
        conv_ok = true;
    }
    double conv_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - conv_start).count();
    Metrics::instance().observe_conversion_duration(conv_dur);
    
    if (conv_ok) {
        {   // success clears any prior failure history (A4)
            std::lock_guard<std::mutex> lk(p_->mtx);
            p_->fail_counts.erase(path);
        }
        // Stage 6: Output hash
        auto out_hash_start = std::chrono::steady_clock::now();
        std::string out_hash = sha256_file(dst);
        double out_hash_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - out_hash_start).count();
        Metrics::instance().observe_hash_duration(out_hash_dur);
        
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
        
        // Stage 7: DB insert
        auto db_start = std::chrono::steady_clock::now();
        store.InsertImport(rec);
        double db_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - db_start).count();
        Metrics::instance().observe_db_duration(db_dur);
        
        // Optional thumbnail
        if (cfg_.gen_thumb_jpeg) {
            std::string thumb = out_dir + "/" + seq_name + ".thumb.jpg";
            std::string got = extract_thumbnail(dst, thumb);
            if (got.empty()) SPDLOG_WARN("[worker] thumbnail extract failed {}", dst);
            else SPDLOG_INFO("[worker] wrote thumbnail {}", thumb);
        }
        
        // Stage 8: Archive move
        auto arch_start = std::chrono::steady_clock::now();
        std::string arch_dir = cfg_.archive_dir + "/" + folder;
        ensure_dir(arch_dir);
        move_file(path, arch_dir + "/" + fs::path(path).filename().string());
        double arch_dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - arch_start).count();
        Metrics::instance().observe_archive_duration(arch_dur);
        
        SPDLOG_INFO("[worker] converted {} -> {}", path, dst);
        Metrics::instance().inc_conversions_completed("completed", 1);
    } else {
        SPDLOG_ERROR("[worker] conversion failed {}", path);
        Metrics::instance().inc_conversions_completed("failed", 1);
        move_to_dead_letter(path, store);
    }
    release_inflight(path);
}

void Worker::release_inflight(const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->mtx);
    p_->inflight.erase(path);
}

void Worker::evict_fp_cache() {
    // Caller must hold p_->mtx. Bounds memory by evicting least-recently-seen
    // entries (simple LRU; ruling #2 keeps this in-memory only).
    constexpr size_t kMaxEntries = 10000;
    while (p_->fp_cache.size() > kMaxEntries) {
        auto oldest = p_->fp_cache.begin();
        for (auto it = p_->fp_cache.begin(); it != p_->fp_cache.end(); ++it) {
            if (it->second.last_seen < oldest->second.last_seen) oldest = it;
        }
        p_->fp_cache.erase(oldest);
    }
}

void Worker::move_to_dead_letter(const std::string& path, Store& store) {
    (void)store;  // alerts table removed in portable build; logging only
    // This failure path is terminal for this pass: release the in-flight
    // marker so the poller may requeue (retry) the file.
    release_inflight(path);

    // Increment the persistent failure counter outside the TTL map so the
    int new_retry = 0;
    bool should_dead_letter = false;
    {
        std::lock_guard<std::mutex> lk(p_->mtx);
        auto& count = p_->fail_counts[path];
        ++count;
        new_retry = count;
        should_dead_letter = (new_retry >= cfg_.dead_letter_max_retries);
        if (should_dead_letter) p_->fail_counts.erase(path);
    }

    if (!should_dead_letter) {
        // Will be retried on next poll
        SPDLOG_WARN("[worker] conversion failed, retry {}/{}: {}",
                    new_retry, cfg_.dead_letter_max_retries, path);
        return;
    }

    std::string filename = fs::path(path).filename().string();
    std::string dead_letter_dir = cfg_.archive_dir + "/failed";
    ensure_dir(dead_letter_dir);
    std::string dead_path = dead_letter_dir + "/" + filename + ".failcount." + std::to_string(new_retry);

    // move_file() gives rename-when-possible and durable copy+fsync fallback
    // across devices (PRD §6.4), unlike a bare fs::rename.
    if (!move_file(path, dead_path)) {
        SPDLOG_ERROR("[worker] failed to move to dead letter: {} -> {}", path, dead_path);
        return;
    }
    SPDLOG_WARN("[worker] moved to dead letter after {} retries: {}", new_retry, dead_path);
}

} // namespace rawimport