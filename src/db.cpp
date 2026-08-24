// db.cpp — SQLite-backed Store implementation for the portable branch.
// Concurrency model (action plan §5 Phase 1): one sqlite3 connection per
// thread against the same WAL-mode file; busy_timeout serializes writers;
// AllocateSequence uses BEGIN IMMEDIATE so AUTOINCREMENT stays monotonic.
#include "db.h"
#include "util.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

namespace rawimport {

namespace {

// Embedded schema — keep in sync with migrations/0001_init_sqlite.sql
// (source of truth). Compiled in so the release ZIP ships no migrations dir.
const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version TEXT PRIMARY KEY,
    applied_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS sequences (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS imports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sequence_id INTEGER NOT NULL,
    source_path TEXT NOT NULL,
    source_hash TEXT NOT NULL,
    output_path TEXT NOT NULL,
    output_hash TEXT NOT NULL,
    camera_model TEXT,
    capture_date TEXT,
    capture_time TEXT,
    folder_schema TEXT,
    conversion_settings TEXT,
    status TEXT DEFAULT 'pending',
    date_source TEXT DEFAULT 'exif',
    orientation INTEGER,
    created_at TEXT DEFAULT (datetime('now')),
    completed_at TEXT,
    error_message TEXT,
    FOREIGN KEY (sequence_id) REFERENCES sequences(id)
);
CREATE INDEX IF NOT EXISTS idx_imports_source_hash ON imports(source_hash);
CREATE INDEX IF NOT EXISTS idx_imports_output_hash ON imports(output_hash);
CREATE INDEX IF NOT EXISTS idx_imports_capture_date ON imports(capture_date);
CREATE INDEX IF NOT EXISTS idx_imports_status_created ON imports(status, created_at);
CREATE INDEX IF NOT EXISTS idx_imports_sequence_id ON imports(sequence_id);
CREATE TABLE IF NOT EXISTS reconversions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    import_id INTEGER NOT NULL,
    previous_output_hash TEXT NOT NULL,
    new_output_hash TEXT,
    conversion_settings TEXT NOT NULL,
    triggered_at TEXT DEFAULT (datetime('now')),
    completed_at TEXT,
    status TEXT DEFAULT 'pending',
    error_message TEXT,
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS processing_locks (
    import_id INTEGER PRIMARY KEY,
    worker_id TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    created_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS preview_edits (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    import_id INTEGER NOT NULL,
    worker TEXT,
    width INTEGER,
    height INTEGER,
    quality INTEGER,
    created_at TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE
);
)SQL";

bool sql_exec(sqlite3* db, const char* sql, const char* ctx = "exec") {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        SPDLOG_ERROR("[db] {} failed: {} ({})", ctx, err ? err : "unknown", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Run a SELECT with bound text params; return rows as string matrices.
std::vector<std::vector<std::string>> query_rows(sqlite3* db, const std::string& sql,
                                                 const std::vector<std::string>& params = {}) {
    std::vector<std::vector<std::string>> rows;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        SPDLOG_ERROR("[db] prepare failed: {} ({})", sqlite3_errmsg(db), sql);
        return rows;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(st, static_cast<int>(i + 1), params[i].c_str(),
                          static_cast<int>(params[i].size()), SQLITE_TRANSIENT);
    }
    int rc = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int n = sqlite3_column_count(st);
        std::vector<std::string> row(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const unsigned char* txt = sqlite3_column_text(st, i);
            if (txt) row[static_cast<size_t>(i)] =
                reinterpret_cast<const char*>(txt);
        }
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
        SPDLOG_ERROR("[db] step failed: {} ({})", sqlite3_errmsg(db), sql);
    }
    sqlite3_finalize(st);
    return rows;
}

// Column order shared by all ImportRecord selects:
// [0]=id [1]=sequence_id [2]=source_path [3]=source_hash [4]=output_path
// [5]=output_hash [6]=camera_model [7]=capture_date [8]=capture_time
// [9]=folder_schema [10]=conversion_settings [11]=status [12]=orientation
// [13]=created_at [14]=completed_at [15]=sequence_name
ImportRecord vec_to_record(const std::vector<std::string>& v) {
    ImportRecord r;
    auto str = [&v](int i) -> std::string {
        return i < static_cast<int>(v.size()) ? v[i] : std::string{};
    };
    r.id = v.size() > 0 ? std::strtoll(v[0].c_str(), nullptr, 10) : 0;
    r.sequence_id = v.size() > 1 ? std::strtoll(v[1].c_str(), nullptr, 10) : 0;
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
    r.orientation = v.size() > 12 ? std::atoi(v[12].c_str()) : 0;
    r.created_at = str(13);
    r.completed_at = str(14);
    r.sequence_name = str(15);
    return r;
}

void apply_pragmas(sqlite3* db) {
    // journal_mode=WAL returns a result row; sqlite3_exec discards it fine.
    sql_exec(db, "PRAGMA journal_mode=WAL;", "pragma wal");
    sql_exec(db, "PRAGMA busy_timeout=5000;", "pragma busy_timeout");
    sql_exec(db, "PRAGMA foreign_keys=ON;", "pragma foreign_keys");
}

} // namespace

struct Store::Impl {
    sqlite3* db = nullptr;
};

Store::Store() : p_(std::make_unique<Impl>()) {}

Store::Store(sqlite3* adopted_db) : p_(std::make_unique<Impl>()) {
    p_->db = adopted_db;
}

Store::~Store() { Close(); }

bool Store::Open(const Config& cfg) {
    if (!cfg.db_name.empty() && cfg.db_name != ":memory:") {
        std::error_code ec;
        std::filesystem::path parent =
            std::filesystem::path(cfg.db_name).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
            ensure_dir(parent.string());
        }
    }
    if (p_->db) { sqlite3_close_v2(p_->db); p_->db = nullptr; }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(cfg.db_name.c_str(), &p_->db, flags, nullptr) != SQLITE_OK) {
        SPDLOG_ERROR("[db] open failed: {} ({})",
                     p_->db ? sqlite3_errmsg(p_->db) : "unknown", cfg.db_name);
        if (p_->db) { sqlite3_close_v2(p_->db); p_->db = nullptr; }
        return false;
    }
    apply_pragmas(p_->db);
    SPDLOG_INFO("[db] opened {} (WAL mode)", cfg.db_name);
    return true;
}

bool Store::Migrate() {
    if (!p_->db) return false;
    if (!sql_exec(p_->db, kSchemaSql, "schema")) return false;
    if (!sql_exec(p_->db,
        "INSERT OR IGNORE INTO schema_migrations(version) VALUES('0001_init_sqlite');",
        "record migration")) return false;
    SPDLOG_INFO("[db] schema up to date (0001_init_sqlite)");
    return true;
}

sqlite3* Store::CreateConnection(const Config& cfg) {
    sqlite3* db = nullptr;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(cfg.db_name.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        SPDLOG_ERROR("[db] worker connection failed: {}",
                     db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close_v2(db);
        return nullptr;
    }
    apply_pragmas(db);
    return db;
}

std::pair<int64_t, std::string> Store::AllocateSequence() {
    if (!p_->db) return {0, ""};
    // BEGIN IMMEDIATE grabs SQLite's writer lock up front: concurrent worker
    // threads serialize here and AUTOINCREMENT guarantees a monotonic id.
    if (!sql_exec(p_->db, "BEGIN IMMEDIATE;", "begin")) return {0, ""};
    if (!sql_exec(p_->db,
        "INSERT INTO sequences(name) VALUES('');", "seq insert")) {
        sql_exec(p_->db, "ROLLBACK;", "rollback");
        return {0, ""};
    }
    int64_t id = sqlite3_last_insert_rowid(p_->db);
    std::string name = "IMG_" + std::to_string(id);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, "UPDATE sequences SET name=? WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, name.c_str(), static_cast<int>(name.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(st, 2, id) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_DONE) {
        SPDLOG_ERROR("[db] seq rename failed: {}", sqlite3_errmsg(p_->db));
        sqlite3_finalize(st);
        sql_exec(p_->db, "ROLLBACK;", "rollback");
        return {0, ""};
    }
    sqlite3_finalize(st);
    if (!sql_exec(p_->db, "COMMIT;", "commit")) {
        sql_exec(p_->db, "ROLLBACK;", "rollback");
        return {0, ""};
    }
    SPDLOG_DEBUG("[db] allocated sequence {} ({})", name, id);
    return {id, name};
}

int64_t Store::InsertImport(const ImportRecord& rec) {
    if (!p_->db) return 0;
    const char* sql =
        "INSERT INTO imports (sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, date_source, orientation, created_at, completed_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), datetime('now'));";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, sql, -1, &st, nullptr) != SQLITE_OK) {
        SPDLOG_ERROR("[db] insert prepare failed: {}", sqlite3_errmsg(p_->db));
        return 0;
    }
    auto bind_text_or_null = [&](int idx, const std::string& val) {
        if (val.empty())
            return sqlite3_bind_null(st, idx);
        return sqlite3_bind_text(st, idx, val.c_str(),
                                 static_cast<int>(val.size()), SQLITE_TRANSIENT);
    };
    sqlite3_bind_int64(st, 1, rec.sequence_id);
    bind_text_or_null(2, rec.source_path);
    bind_text_or_null(3, rec.source_hash);
    bind_text_or_null(4, rec.output_path);
    bind_text_or_null(5, rec.output_hash);
    // Nullable columns: empty string binds as SQL NULL (D22 semantics).
    bind_text_or_null(6, rec.camera_model);
    bind_text_or_null(7, rec.capture_date);
    bind_text_or_null(8, rec.capture_time);
    bind_text_or_null(9, rec.folder_schema);
    bind_text_or_null(10, rec.conversion_settings);
    bind_text_or_null(11, to_string(rec.status));
    bind_text_or_null(12, rec.date_source == DateSource::Mtime ? "mtime" : "exif");
    sqlite3_bind_int(st, 13, rec.orientation);
    if (sqlite3_step(st) != SQLITE_DONE) {
        SPDLOG_ERROR("[db] insert failed: {}", sqlite3_errmsg(p_->db));
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(p_->db);
}

std::optional<ImportRecord> Store::GetImportBySequence(const std::string& seq) {
    auto rows = query_rows(p_->db,
        "SELECT i.id, i.sequence_id, i.source_path, i.source_hash, i.output_path, "
        "i.output_hash, i.camera_model, i.capture_date, i.capture_time, i.folder_schema, "
        "i.conversion_settings, i.status, i.orientation, i.created_at, i.completed_at, "
        "s.name FROM imports i JOIN sequences s ON i.sequence_id=s.id WHERE s.name=? LIMIT 1",
        {seq});
    if (rows.empty()) return std::nullopt;
    return vec_to_record(rows[0]);
}

std::optional<ImportRecord> Store::GetImportByHash(const std::string& sha) {
    auto rows = query_rows(p_->db,
        "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports "
        "WHERE source_hash=? OR output_hash=? LIMIT 1",
        {sha, sha});
    if (rows.empty()) return std::nullopt;
    return vec_to_record(rows[0]);
}

Store::Stats Store::GetStats() {
    Stats s;
    if (!p_->db) return s;
    auto scalar = [this](const char* sql) -> int64_t {
        auto rows = query_rows(p_->db, sql);
        if (rows.empty() || rows[0].empty()) return 0;
        return std::strtoll(rows[0][0].c_str(), nullptr, 10);
    };
    s.total = scalar("SELECT COUNT(*) FROM imports;");
    s.completed = scalar("SELECT COUNT(*) FROM imports WHERE status='completed';");
    s.failed = scalar("SELECT COUNT(*) FROM imports WHERE status='failed';");
    return s;
}

std::vector<ImportRecord> Store::ListImports(int page, int limit,
                                             const std::string& status_filter,
                                             const std::string& camera_filter) {
    std::vector<ImportRecord> out;
    if (!p_->db) return out;
    // Optional filters are bound parameters; LIMIT/OFFSET are sanitized ints.
    std::string sql =
        "SELECT id, sequence_id, source_path, source_hash, output_path, "
        "output_hash, camera_model, capture_date, capture_time, folder_schema, "
        "conversion_settings, status, orientation, created_at, completed_at, "
        "(SELECT name FROM sequences WHERE id=sequence_id) FROM imports WHERE 1=1";
    std::vector<std::string> params;
    if (!status_filter.empty()) { sql += " AND status=?"; params.push_back(status_filter); }
    if (!camera_filter.empty()) { sql += " AND camera_model=?"; params.push_back(camera_filter); }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit) +
           " OFFSET " + std::to_string((page - 1) * limit) + ";";
    for (auto& row : query_rows(p_->db, sql, params)) out.push_back(vec_to_record(row));
    return out;
}

void Store::Close() {
    if (p_->db) {
        sqlite3_close_v2(p_->db);
        p_->db = nullptr;
    }
}

bool Store::Ping() {
    if (!p_->db) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(p_->db, "SELECT 1;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return ok;
}

} // namespace rawimport
