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

// Helper for prepared statements
class PreparedStatement {
public:
    PreparedStatement(MYSQL* conn, const std::string& query) : conn_(conn), stmt_(mysql_stmt_init(conn)) {
        if (!stmt_) {
            SPDLOG_ERROR("[db] mysql_stmt_init failed: {}", mysql_error(conn_));
            return;
        }
        if (mysql_stmt_prepare(stmt_, query.c_str(), query.size()) != 0) {
            SPDLOG_ERROR("[db] mysql_stmt_prepare failed: {}", mysql_stmt_error(stmt_));
            mysql_stmt_close(stmt_);
            stmt_ = nullptr;
        }
    }
    
    ~PreparedStatement() {
        if (stmt_) mysql_stmt_close(stmt_);
    }
    
    bool valid() const { return stmt_ != nullptr; }
    MYSQL_STMT* get() { return stmt_; }
    
    bool bind_param(MYSQL_BIND* binds, unsigned long count) {
        if (!stmt_) return false;
        if (mysql_stmt_bind_param(stmt_, binds) != 0) {
            SPDLOG_ERROR("[db] mysql_stmt_bind_param failed: {}", mysql_stmt_error(stmt_));
            return false;
        }
        return true;
    }
    
    bool bind_result(MYSQL_BIND* binds, unsigned long count) {
        if (!stmt_) return false;
        if (mysql_stmt_bind_result(stmt_, binds) != 0) {
            SPDLOG_ERROR("[db] mysql_stmt_bind_result failed: {}", mysql_stmt_error(stmt_));
            return false;
        }
        return true;
    }
    
    bool execute() {
        if (!stmt_) return false;
        if (mysql_stmt_execute(stmt_) != 0) {
            SPDLOG_ERROR("[db] mysql_stmt_execute failed: {}", mysql_stmt_error(stmt_));
            return false;
        }
        return true;
    }
    
    bool fetch() {
        if (!stmt_) return false;
        int r = mysql_stmt_fetch(stmt_);
        return r == 0;
    }
    
    bool store_result() {
        if (!stmt_) return false;
        return mysql_stmt_store_result(stmt_) == 0;
    }
    
    my_ulonglong affected_rows() {
        if (!stmt_) return 0;
        return mysql_stmt_affected_rows(stmt_);
    }
    
    my_ulonglong insert_id() {
        if (!stmt_) return 0;
        return mysql_stmt_insert_id(stmt_);
    }
    
    void free_result() {
        if (stmt_) mysql_stmt_free_result(stmt_);
    }

private:
    MYSQL* conn_;
    MYSQL_STMT* stmt_ = nullptr;
};

namespace {
// Escape stopgap for the remaining string-concatenated queries (amendment
// ruling #4: hot writes use prepared statements; read-only paths escape every
// interpolated value via mysql_real_escape_string).
std::string sql_escape(MYSQL* conn, const std::string& in) {
    if (in.empty()) return in;
    std::string out(in.size() * 2 + 1, '\0');
    unsigned long n = mysql_real_escape_string(conn, out.data(), in.c_str(), in.size());
    out.resize(n);
    return out;
}
} // namespace

struct Store::Impl {
    MYSQL* conn = nullptr;
};

Store::Store() : p_(std::make_unique<Impl>()) {}

Store::Store(MYSQL* adopted_conn) : p_(std::make_unique<Impl>()) {
    p_->conn = adopted_conn;
}

Store::~Store() { Close(); }

bool Store::Open(const Config& cfg) {
    p_->conn = mysql_init(nullptr);
    if (!p_->conn) {
        SPDLOG_ERROR("[db] mysql_init failed");
        return false;
    }
    mysql_optionsv(p_->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    SPDLOG_INFO("[db] connecting to {}@{}:{}/{}", cfg.db_user, cfg.db_host, cfg.db_port, cfg.db_name);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (mysql_real_connect(p_->conn, cfg.db_host.c_str(),
                               cfg.db_user.c_str(), cfg.db_password.c_str(),
                               cfg.db_name.c_str(), cfg.db_port, nullptr, 0)) {
            SPDLOG_INFO("[db] connected successfully");
            return true;
        }
        SPDLOG_WARN("[db] connection attempt {} failed: {}", attempt + 1, mysql_error(p_->conn));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    SPDLOG_ERROR("[db] all connection attempts failed");
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
                           sql_escape(p_->conn, version) + "'";
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
        std::string ins = "INSERT INTO schema_migrations(version) VALUES('" +
                          sql_escape(p_->conn, version) + "')";
        mysql_query(p_->conn, ins.c_str());
    }
    return true;
}

std::pair<int64_t, std::string> Store::AllocateSequence() {
    // 1. Clean up any stale empty rows from previous crashes
    if (mysql_query(p_->conn, "DELETE FROM sequences WHERE name = ''") != 0) {
        SPDLOG_WARN("[db] cleanup stale empty sequence names failed: {}", mysql_error(p_->conn));
    }
    // Also clean up orphaned TMP_ rows from previous failed allocations
    if (mysql_query(p_->conn, "DELETE FROM sequences WHERE name LIKE 'TMP_%'") != 0) {
        SPDLOG_WARN("[db] cleanup stale TMP_ sequence names failed: {}", mysql_error(p_->conn));
    }

    // 2. Use a unique temporary placeholder instead of ''
    // 2. Use a unique temporary placeholder instead of ''
    // random_token() (mt19937) is fine here per PRD §4.2 — disposable temp
    // name, not security-relevant; and it fits VARCHAR(32): "TMP_"+16 = 20.
    // secure_random_token()'s 64-hex output overflowed the column and broke
    // every allocation ("Data too long for column 'name'").
    std::string tmp = "TMP_" + random_token();
    
    // 3. Execute in a transaction
    if (mysql_query(p_->conn, "START TRANSACTION") != 0) {
        SPDLOG_ERROR("[db] failed to start transaction: {}", mysql_error(p_->conn));
        return {0, ""};
    }

    // Use prepared statement for INSERT
    PreparedStatement stmt_ins(p_->conn, "INSERT INTO sequences(name) VALUES(?)");
    if (!stmt_ins.valid()) {
        mysql_query(p_->conn, "ROLLBACK");
        return {0, ""};
    }
    
    MYSQL_BIND bind[1] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = const_cast<char*>(tmp.c_str());
    bind[0].buffer_length = tmp.size();
    
    if (!stmt_ins.bind_param(bind, 1) || !stmt_ins.execute()) {
        mysql_query(p_->conn, "ROLLBACK");
        return {0, ""};
    }

    int64_t id = static_cast<int64_t>(stmt_ins.insert_id());
    if (id == 0) {
        SPDLOG_ERROR("[db] mysql_stmt_insert_id returned 0 after sequence insert");
        mysql_query(p_->conn, "ROLLBACK");
        return {0, ""};
    }

    std::string name = "IMG_" + std::to_string(id);
    
    // Use prepared statement for UPDATE
    PreparedStatement stmt_upd(p_->conn, "UPDATE sequences SET name=? WHERE id=?");
    if (!stmt_upd.valid()) {
        mysql_query(p_->conn, "ROLLBACK");
        return {0, ""};
    }
    
    MYSQL_BIND bind_upd[2] = {};
    bind_upd[0].buffer_type = MYSQL_TYPE_STRING;
    bind_upd[0].buffer = const_cast<char*>(name.c_str());
    bind_upd[0].buffer_length = name.size();
    bind_upd[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_upd[1].buffer = &id;
    
    if (!stmt_upd.bind_param(bind_upd, 2) || !stmt_upd.execute()) {
        mysql_query(p_->conn, "ROLLBACK");
        return {0, ""};
    }

    if (mysql_query(p_->conn, "COMMIT") != 0) {
        SPDLOG_ERROR("[db] failed to commit transaction: {}", mysql_error(p_->conn));
        mysql_query(p_->conn, "ROLLBACK");
        return {0, ""};
    }
    
    SPDLOG_DEBUG("[db] allocated sequence {} ({})", name, id);
    return {id, name};
}

int64_t Store::InsertImport(const ImportRecord& rec) {
    // status is bound (not hardcoded 'completed') so reconcile legacy
    // placeholders and failed imports persist their true status (D23).
    PreparedStatement stmt(p_->conn,
        "INSERT INTO imports (sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at) VALUES ("
        "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NOW(), NOW())");
    if (!stmt.valid()) return 0;
    
    MYSQL_BIND bind[12] = {};
    
    // sequence_id
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = const_cast<int64_t*>(&rec.sequence_id);
    
    // source_path
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = const_cast<char*>(rec.source_path.c_str());
    bind[1].buffer_length = rec.source_path.size();
    
    // source_hash
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(rec.source_hash.c_str());
    bind[2].buffer_length = rec.source_hash.size();
    
    // output_path
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = const_cast<char*>(rec.output_path.c_str());
    bind[3].buffer_length = rec.output_path.size();
    
    // output_hash
    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = const_cast<char*>(rec.output_hash.c_str());
    bind[4].buffer_length = rec.output_hash.size();
    
    // camera_model
    bind[5].buffer_type = MYSQL_TYPE_STRING;
    bind[5].buffer = const_cast<char*>(rec.camera_model.c_str());
    bind[5].buffer_length = rec.camera_model.size();
    
    // capture_date / capture_time (DATE / TIME columns, nullable).
    // Empty values must bind as SQL NULL — an empty string is rejected by
    // strict mode ("Incorrect date value: ''"), which silently dropped all
    // reconcile legacy placeholders (D22).
    my_bool date_null = rec.capture_date.empty() ? 1 : 0;
    my_bool time_null = rec.capture_time.empty() ? 1 : 0;
    
    // capture_date
    bind[6].buffer_type = MYSQL_TYPE_STRING;
    bind[6].buffer = const_cast<char*>(rec.capture_date.c_str());
    bind[6].buffer_length = static_cast<unsigned long>(rec.capture_date.size());
    bind[6].is_null = &date_null;
    
    // capture_time
    bind[7].buffer_type = MYSQL_TYPE_STRING;
    bind[7].buffer = const_cast<char*>(rec.capture_time.c_str());
    bind[7].buffer_length = static_cast<unsigned long>(rec.capture_time.size());
    bind[7].is_null = &time_null;
    
    // folder_schema
    bind[8].buffer_type = MYSQL_TYPE_STRING;
    bind[8].buffer = const_cast<char*>(rec.folder_schema.c_str());
    bind[8].buffer_length = rec.folder_schema.size();
    
    // conversion_settings
    bind[9].buffer_type = MYSQL_TYPE_STRING;
    bind[9].buffer = const_cast<char*>(rec.conversion_settings.c_str());
    bind[9].buffer_length = rec.conversion_settings.size();
    
    // status (bound — see D23 note above)
    const std::string status_str = to_string(rec.status);
    bind[10].buffer_type = MYSQL_TYPE_STRING;
    bind[10].buffer = const_cast<char*>(status_str.c_str());
    bind[10].buffer_length = static_cast<unsigned long>(status_str.size());
    
    // orientation
    bind[11].buffer_type = MYSQL_TYPE_LONG;
    bind[11].buffer = const_cast<int*>(&rec.orientation);
    
    if (!stmt.bind_param(bind, 12) || !stmt.execute()) {
        return 0;
    }
    
    return static_cast<int64_t>(stmt.insert_id());
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
        "s.name FROM imports i JOIN sequences s ON i.sequence_id=s.id WHERE s.name='"
        + sql_escape(p_->conn, seq) + "'";
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
        "WHERE source_hash='" + sql_escape(p_->conn, sha) +
        "' OR output_hash='" + sql_escape(p_->conn, sha) + "' LIMIT 1";
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
        "WHERE source_path='" + sql_escape(p_->conn, path) + "' LIMIT 1";
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
        "WHERE output_path='" + sql_escape(p_->conn, path) + "' LIMIT 1";
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
    std::string sql = "UPDATE imports SET output_hash='" + sql_escape(p_->conn, new_hash) +
                      "' WHERE id=" + std::to_string(id);
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
        + std::to_string(import_id) + ",'" + sql_escape(p_->conn, worker) + "','"
        + sql_escape(p_->conn, prev_hash) + "','" + sql_escape(p_->conn, new_hash) + "',"
        + std::to_string(width) + "," + std::to_string(height)
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
        + std::to_string(import_id) + ",'" + sql_escape(p_->conn, job.previous_output_hash) + "','"
        + sql_escape(p_->conn, settings_json) + "','" + sql_escape(p_->conn, job.reason) + "','pending', NOW())";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return 0;
    return static_cast<int64_t>(mysql_insert_id(p_->conn));
}

bool Store::UpdateReconversion(int64_t id, const std::string& new_hash, ImportStatus status) {
    std::string sql = "UPDATE reconversions SET new_output_hash='" + sql_escape(p_->conn, new_hash) +
        "', status='" + to_string(status) + "', completed_at=NOW() WHERE id=" + std::to_string(id);
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

bool Store::AcquireLock(int64_t import_id, const std::string& worker_id, int ttl_sec) {
    if (!p_->conn) return false;
    int64_t expires = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + ttl_sec;

    // Use secure random token for worker_id if not provided (PRD §4.2).
    std::string effective_worker_id = worker_id;
    if (effective_worker_id.empty()) {
        effective_worker_id = secure_random_token();
    }

    // --- SELECT current lock row (prepared statement, PRD §4.1) ---
    PreparedStatement sel_stmt(p_->conn, "SELECT worker_id, expires_at FROM processing_locks WHERE import_id=?");
    if (!sel_stmt.valid()) return false;

    MYSQL_BIND sel_bind[1];
    memset(sel_bind, 0, sizeof(sel_bind));
    sel_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    sel_bind[0].buffer = static_cast<void*>(&import_id);

    if (!sel_stmt.bind_param(sel_bind, 1)) return false;

    // Bind result buffers BEFORE execute/fetch.
    char cur_w_buf[256] = {0};
    unsigned long cur_w_len = 0;
    int64_t cur_exp = 0;
    MYSQL_BIND result_bind[2];
    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = cur_w_buf;
    result_bind[0].buffer_length = sizeof(cur_w_buf) - 1;
    result_bind[0].length = &cur_w_len;
    result_bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[1].buffer = static_cast<void*>(&cur_exp);
    if (!sel_stmt.bind_result(result_bind, 2)) return false;

    if (!sel_stmt.execute()) return false;
    if (!sel_stmt.store_result()) return false;

    bool held = false;
    std::string cur_w;
    if (sel_stmt.fetch()) {          // false when no rows
        cur_w.assign(cur_w_buf, cur_w_len);
        held = true;
    }
    sel_stmt.free_result();

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (held && cur_exp > now && cur_w != effective_worker_id) return false;

    // --- UPSERT lock row (prepared statement, PRD §4.1) ---
    PreparedStatement up_stmt(p_->conn,
        "INSERT INTO processing_locks (import_id, worker_id, expires_at) "
        "VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE worker_id=?, expires_at=?");
    if (!up_stmt.valid()) return false;

    MYSQL_BIND up_bind[5];
    memset(up_bind, 0, sizeof(up_bind));
    up_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    up_bind[0].buffer = static_cast<void*>(&import_id);
    up_bind[1].buffer_type = MYSQL_TYPE_STRING;
    up_bind[1].buffer = const_cast<char*>(effective_worker_id.c_str());
    up_bind[1].buffer_length = static_cast<unsigned long>(effective_worker_id.size());
    up_bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    up_bind[2].buffer = static_cast<void*>(&expires);
    up_bind[3] = up_bind[1];   // worker_id (ON DUPLICATE KEY)
    up_bind[4] = up_bind[2];   // expires_at (ON DUPLICATE KEY)
    if (!up_stmt.bind_param(up_bind, 5)) return false;
    return up_stmt.execute();
}

bool Store::ReleaseLock(int64_t import_id) {
    std::string sql = "DELETE FROM processing_locks WHERE import_id=" + std::to_string(import_id);
    return mysql_query(p_->conn, sql.c_str()) == 0;
}

bool Store::HasOutputHash(const std::string& hash) {
    std::string sql = "SELECT 1 FROM imports WHERE output_hash='" +
                      sql_escape(p_->conn, hash) + "' LIMIT 1";
    if (mysql_query(p_->conn, sql.c_str()) != 0) return false;
    MYSQL_RES* res = mysql_store_result(p_->conn);
    bool found = res && mysql_num_rows(res) > 0;
    if (res) mysql_free_result(res);
    return found;
}

bool Store::InsertAlert(const std::string& severity, const std::string& category,
                        const std::string& message, const std::string& ref_sequence) {
    if (!p_->conn) return false;
    PreparedStatement stmt(p_->conn,
        "INSERT INTO alerts (severity, category, message, ref_sequence) VALUES (?, ?, ?, ?)");
    if (!stmt.valid()) return false;

    MYSQL_BIND bind[4];
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = const_cast<char*>(severity.c_str());
    bind[0].buffer_length = static_cast<unsigned long>(severity.size());
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = const_cast<char*>(category.c_str());
    bind[1].buffer_length = static_cast<unsigned long>(category.size());
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = const_cast<char*>(message.c_str());
    bind[2].buffer_length = static_cast<unsigned long>(message.size());
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = const_cast<char*>(ref_sequence.c_str());
    bind[3].buffer_length = static_cast<unsigned long>(ref_sequence.size());

    if (!stmt.bind_param(bind, 4)) return false;
    return stmt.execute();
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
    if (!status_filter.empty())
        sql += " AND status='" + sql_escape(p_->conn, status_filter) + "'";
    if (!camera_filter.empty())
        sql += " AND camera_model='" + sql_escape(p_->conn, camera_filter) + "'";
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

bool Store::Ping() {
    if (!p_->conn) return false;
    return mysql_ping(p_->conn) == 0;
}

MYSQL* Store::CreateConnection(const Config& cfg) {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        SPDLOG_ERROR("[db] mysql_init failed for worker connection");
        return nullptr;
    }
    mysql_optionsv(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(conn, cfg.db_host.c_str(),
                            cfg.db_user.c_str(), cfg.db_password.c_str(),
                            cfg.db_name.c_str(), cfg.db_port, nullptr, 0)) {
        SPDLOG_ERROR("[db] worker connection failed: {}", mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }
    return conn;
}

} // namespace rawimport
