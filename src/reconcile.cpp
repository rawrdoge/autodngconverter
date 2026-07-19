#include "reconcile.h"
#include "config.h"
#include "db.h"
#include "pipeline.h"

#include <filesystem>
#include <regex>
#include <spdlog/spdlog.h>

namespace rawimport {
namespace fs = std::filesystem;

bool ReconcileLibrary(const Config& cfg, Store& store) {
    std::regex img_re("^IMG_(\\d+)\\.dng$", std::regex::icase);
    int64_t max_n = 0;
    int found = 0;
    int inserted = 0;

    auto scan_dir = [&](const std::string& root) {
        if (root.empty()) return;
        std::error_code ec;
        if (!fs::exists(root, ec)) return;
        for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!e.is_regular_file()) continue;
            std::string name = e.path().filename().string();
            std::smatch m;
            if (!std::regex_match(name, m, img_re)) continue;
            int64_t n = std::strtoll(m[1].str().c_str(), nullptr, 10);
            if (n > max_n) max_n = n;
            ++found;
            std::string abs = fs::absolute(e.path()).string();
            // skip if already known by output_path
            auto existing = store.GetImportBySourcePath(abs);
            if (existing) continue;
            ImportRecord rec;
            rec.sequence_id = n;
            rec.sequence_name = "IMG_" + std::to_string(n);
            rec.source_path = "";
            // Match Go reference: synthetic non-SHA256 source hash unique per
            // sequence (legacy:IMG_{n}), never collides with real 64-hex SHA-256.
            rec.source_hash = "legacy:IMG_" + std::to_string(n);
            rec.output_path = abs;
            rec.output_hash = sha256_file(abs);
            rec.camera_model = "legacy";
            rec.capture_date = "";
            rec.capture_time = "";
            // folder_schema = relative parent path from root
            std::string parent = e.path().parent_path().string();
            if (parent.size() > root.size())
                rec.folder_schema = parent.substr(root.size() + 1);
            rec.conversion_settings = "{}";
            rec.status = ImportStatus::Legacy;
            rec.orientation = 0;
            rec.created_at = "";
            rec.completed_at = "";
            store.InsertImport(rec);
            ++inserted;
        }
    };

    scan_dir(cfg.output_dir);
    scan_dir(cfg.archive_dir);

    // Reserve sequence counter past max_n so new imports never collide.
    if (max_n > 0) {
        for (int64_t i = 0; i < max_n; ++i) {
            auto [id, name] = store.AllocateSequence();
            if (id == 0) { SPDLOG_ERROR("[reconcile] sequence alloc failed"); break; }
            if (id >= max_n) break;
        }
    }

    SPDLOG_INFO("[reconcile] scanned={} dngs, inserted={} legacy placeholders, max_n={}",
                 found, inserted, max_n);
    return true;
}

} // namespace rawimport