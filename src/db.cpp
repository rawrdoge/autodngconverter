#include "db.h"
#include "config.h"
#include "pipeline.h"
#include "util.h"

#include <mysql.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

namespace rawimport {

struct Store::Impl {
    MYSQL* conn = nullptr;
};

Store::Store() : p_(std::make_unique<Impl>()) {}

Store::~Store() { Close(); }

bool Store::Open(const Config& cfg) {
    p_->conn = mysql_init(nullptr);
    if (!p_->conn) return false;
    mysql_optionsv(p_->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (mysql_real_connect(p_->conn, cfg.db_host.c_str(),
                               cfg.db_user.c_str(), cfg.db_password.c_str(),
                               cfg.db_name.c_str(), cfg.db_port, nullptr, 0)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

bool Store::Migrate(const std::string& migrations_dir) {
    if (!p_->conn) return false;
    // ensure schema_migrations table
    mysql_query(p_->conn,
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "version VARCHAR(255) PRIMARY KEY, applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
    // NOTE: processing_locks is defined authoritatively in migrations/0001_init.sql
    // (keyed by import_id). Do NOT create it inline here — that caused schema
    // drift (LEAD L4). The migration runner below is the single source of truth.

    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(migrations_dir)) {
        if (e.path().extension() == ".sql") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::ifstream in(f);
        std::stringstream ss; ss << in.rdbuf();
        std::string sql = ss.str();
        std::string version = std::filesystem::path(f).filename().string();
        // already applied?
        std::string chk = "SELECT 1 FROM schema_migrations WHERE version='" +
                           version + "'";
        if (mysql_query(p_->conn, chk.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(p_->conn);
            if (res && mysql_num_rows(res) > 0) { mysql_free_result(res); continue; }
            if (res) mysql_free_result(res);
        }
        for (const auto& stmt : split_sql(sql)) {
            if (mysql_query(p_->conn, stmt.c_str()) != 0) {
                SPDLOG_ERROR("[db] migration '{}' failed: {}", version,
                             mysql_error(p_->conn));
                SPDLOG_ERROR("[db] failing statement: {}", stmt);
                return false;
            }
        }
        std::string ins = "INSERT INTO schema_migrations(version) VALUES('" + version + "')";
        mysql_query(p_->conn, ins.c_str());
    }
    return true;
}

std::pair<int64_t, std::string> Store::AllocateSequence() {
    if (mysql_query(p_->conn, "INSERT INTO sequences(name) VALUES('')") != 0)
        return {0, ""};
    int64_t id = static_cast<int64_t>(mysql_insert_id(p_->conn));
    std::string name = "IMG_" + std::to_string(id);
    std::string upd = "UPDATE sequences SET name='" + name + "' WHERE id=" + std::to_string(id);
    mysql_query(p_->conn, upd.c_str());
    return {id, name};
}

int64_t Store::InsertImport(const ImportRecord& rec) {
    std::string sql =
        "INSERT INTO imports (sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at) VALUES ("
        + std::to_string(rec.sequence_id) + ","
        + "'" + rec.source_path + "',"
        + "'" + rec.source_hash + "',"
        + "'" + rec.output_path + "',"
        + "'" + rec.output_hash + "',"
        + "'" + rec.camera_model + "',"
        + "'" + rec.capture_date + "',"
        + "'" + rec.capture_time + "',"
        + "'" + rec.folder_schema + "',"
        + "'" + rec.conversion_settings + "',"
        + "'completed',"
        + std::to_string(rec.orientation) + ","
        + "NOW(), NOW())";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return 0;
    return static_cast<int64_t>(mysql_insert_id(p_->conn));
}

static ImportRecord row_to_record(MYSQL_ROW row, unsigned long* lengths) {
    ImportRecord r;
    auto str = [&](int i) -> std::string {
        if (!row[i]) return "";
        return std::string(row[i], lengths[i]);
    };
    r.id = row[0] ? std::strtoll(row[0], nullptr, 10) : 0;
    r.sequence_id = row[1] ? std::strtoll(row[1], nullptr, 10) : 0;
    r.source_path = str(2);
    r.source_hash = str(3);
    r.output_path = str(4);
    r.output_hash = str(5);
    r.camera_model = str(6);
    r.capture_date = str(7);
    r.capture_time = str(8);
    r.folder_schema = str(9);
    r.conversion_settings = str(10);
    r.status = ImportStatus::Completed;
    std::string st = str(11);
    if (st == "pending") r.status = ImportStatus::Pending;
    else if (st == "converting") r.status = ImportStatus::Converting;
    else if (st == "failed") r.status = ImportStatus::Failed;
    else if (st == "restored") r.status = ImportStatus::Restored;
    else if (st == "legacy") r.status = ImportStatus::Legacy;
    r.orientation = row[12] ? std::atoi(row[12]) : 0;
    r.created_at = str(13);
    r.completed_at = str(14);
    r.sequence_name = str(15);
    return r;
}

std::optional<ImportRecord> Store::GetImportBySequence(const std::string& seq) {
    std::string sql = "SELECT i.id, i.sequence_id, i.source_path, i.source_hash, i.output_path, "
        "i.output_hash, i.camera_model, i.capture_date, i.capture_time, i.folder_schema, "
        "i.conversion_settings, i.status, i.orientation, i.created_at, i.completed_at, "
        "s.name FROM imports i JOIN sequences s ON i.sequence_id=s.id WHERE s.name='" + seq + "'";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return std::nullopt; }
    auto rec = row_to_record(row, mysql_fetch_lengths(res));
    mysql_free_result(res);
    return rec;
}

std::optional<ImportRecord> Store::GetImportById(int64_t id) {
    std::string sql = "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports "
        "WHERE id=" + std::to_string(id) + " LIMIT 1";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return std::nullopt; }
    auto rec = row_to_record(row, mysql_fetch_lengths(res));
    mysql_free_result(res);
    return rec;
}

std::optional<ImportRecord> Store::GetImportByHash(const std::string& sha) {
    std::string sql = "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports "
        "WHERE source_hash='" + sha + "' OR output_hash='" + sha + "' LIMIT 1";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return std::nullopt; }
    auto rec = row_to_record(row, mysql_fetch_lengths(res));
    mysql_free_result(res);
    return rec;
}

std::optional<ImportRecord> Store::GetImportBySourcePath(const std::string& path) {
    std::string sql = "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports "
        "WHERE source_path='" + path + "' LIMIT 1";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return std::nullopt; }
    auto rec = row_to_record(row, mysql_fetch_lengths(res));
    mysql_free_result(res);
    return rec;
}

std::optional<ImportRecord> Store::GetImportByOutputPath(const std::string& path) {
    std::string sql = "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports "
        "WHERE output_path='" + path + "' LIMIT 1";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return std::nullopt;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return std::nullopt; }
    auto rec = row_to_record(row, mysql_fetch_lengths(res));
    mysql_free_result(res);
    return rec;
}

bool Store::UpdateOutputHash(int64_t id, const std::string& new_hash) {
    std::string sql = "UPDATE imports SET output_hash='" + new_hash + "' WHERE id=" + std::to_string(id);
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

bool Store::UpdateOrientation(int64_t id, int orientation) {
    std::string sql = "UPDATE imports SET orientation=" + std::to_string(orientation) +
                       " WHERE id=" + std::to_string(id);
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

bool Store::RecordPreviewEdit(int64_t import_id, const std::string& worker,
                              const std::string& prev_hash, const std::string& new_hash,
                              int width, int height, int quality) {
    std::string sql = "INSERT INTO preview_edits (import_id, worker, previous_output_hash, "
        "new_output_hash, preview_width, preview_height, preview_quality, edited_at) VALUES ("
        + std::to_string(import_id) + ",'" + worker + "','" + prev_hash + "','"
        + new_hash + "'," + std::to_string(width) + "," + std::to_string(height)
        + "," + std::to_string(quality) + ", NOW())";
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

int64_t Store::InsertReconversion(int64_t import_id, const ReconversionJob& job) {
    // conversion_settings is a JSON column; serialize the settings struct to a
    // valid JSON object (bare strings are rejected by MariaDB JSON validation).
    std::string settings_json = "{\"compression\":\"" + job.settings.compression + "\","
        "\"preview_medium\":\"" + job.settings.preview_medium + "\","
        "\"preview_full\":\"" + job.settings.preview_full + "\","
        "\"version\":\"" + job.settings.version + "\","
        "\"jpeg_quality\":" + std::to_string(job.settings.jpeg_quality) + ","
        "\"linear\":" + (job.settings.linear ? "true" : "false") + "}";
    std::string sql = "INSERT INTO reconversions (import_id, previous_output_hash, "
        "conversion_settings, reason, status, triggered_at) VALUES ("
        + std::to_string(import_id) + ",'" + job.previous_output_hash + "','"
        + settings_json + "','" + job.reason + "','pending', NOW())";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return 0;
    return static_cast<int64_t>(mysql_insert_id(p_->conn));
}

bool Store::UpdateReconversion(int64_t id, const std::string& new_hash, ImportStatus status) {
    std::string sql = "UPDATE reconversions SET new_output_hash='" + new_hash +
        "', status='" + to_string(status) + "', completed_at=NOW() WHERE id=" + std::to_string(id);
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

bool Store::AcquireLock(int64_t import_id, const std::string& worker_id, int ttl_sec) {
    int64_t expires = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + ttl_sec;
    std::string sel = "SELECT worker_id, expires_at FROM processing_locks WHERE import_id="
        + std::to_string(import_id);
    if (mysql_query(p_->conn, sel.c_str()) == 0) {
        MYSQL_RES* res = mysql_store_result(p_->conn);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row) {
                int64_t cur_exp = row[1] ? std::strtoll(row[1], nullptr, 10) : 0;
                std::string cur_w = row[0] ? row[0] : "";
                mysql_free_result(res);
                int64_t now = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                if (cur_exp > now && cur_w != worker_id) return false;
            } else {
                mysql_free_result(res);
            }
        }
    }
    std::string ups = "INSERT INTO processing_locks (import_id, worker_id, expires_at) VALUES ("
        + std::to_string(import_id) + ",'" + worker_id + "'," + std::to_string(expires)
        + ") ON DUPLICATE KEY UPDATE worker_id='" + worker_id + "', expires_at=" + std::to_string(expires);
    return mysql_query(p_->conn, ups.c_str()) == 0;
}

bool Store::ReleaseLock(int64_t import_id) {
    std::string sql = "DELETE FROM processing_locks WHERE import_id=" + std::to_string(import_id);
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

bool Store::HasOutputHash(const std::string& hash) {
    std::string sql = "SELECT 1 FROM imports WHERE output_hash='" + hash + "' LIMIT 1";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return false;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    bool found = res && mysql_num_rows(res) > 0;
    if (res) mysql_free_result(res);
    return found;
}

Store::Stats Store::GetStats() {
    Stats s;
    auto count = [&](const std::string& q) -> int64_t {
        if (mysql_query(p_->conn, q.c_str()) != 0) return 0;
        MYSQL_RES* res = mysql_store_result(p_->conn);
        int64_t v = 0;
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row) v = std::strtoll(row[0], nullptr, 10);
            mysql_free_result(res);
        }
        return v;
    };
    s.total = count("SELECT COUNT(*) FROM imports");
    s.completed = count("SELECT COUNT(*) FROM imports WHERE status='completed'");
    s.failed = count("SELECT COUNT(*) FROM imports WHERE status='failed'");
    return s;
}

std::vector<ImportRecord> Store::ListImports(int page, int limit,
                                             const std::string& status_filter,
                                             const std::string& camera_filter) {
    std::vector<ImportRecord> out;
    std::string sql = "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports WHERE 1=1";
    if (!status_filter.empty()) sql += " AND status='" + status_filter + "'";
    if (!camera_filter.empty()) sql += " AND camera_model='" + camera_filter + "'";
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit) +
           " OFFSET " + std::to_string((page - 1) * limit);
    if (mysql_query(p_->conn, sql.c_str()) != 0) return out;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    if (!res) return out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        out.push_back(row_to_record(row, mysql_fetch_lengths(res)));
    }
    mysql_free_result(res);
    return out;
}

void Store::Close() {
    if (p_->conn) { mysql_close(p_->conn); p_->conn = nullptr; }
}

} // namespace rawimport