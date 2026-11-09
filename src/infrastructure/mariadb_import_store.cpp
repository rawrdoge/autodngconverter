/*
 * rawimport-pipeline - MariaDB ImportStore adapter implementation
 */
#ifdef WITH_MARIADB

#include "infrastructure/mariadb_import_store.hpp"

#include <mariadb/mysql.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace rawimport::infrastructure {

namespace {

MariadbHandle db_connect(const std::string& conn_str) {
    MariadbHandle conn = mariadb_connection();
    if (!conn) return nullptr;
    if (mariadb_options(conn, MariaDB_OPT_CONNECT_STRING, conn_str.c_str()) != 0) {
        mariadb_close(conn);
        return nullptr;
    }
    if (mariadb_real_connect(conn, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0) == NULL) {
        mariadb_close(conn);
        return nullptr;
    }
    return conn;
}

std::string sql_escape(MariadbHandle conn, const std::string& s) {
    std::string out(s.size() * 2 + 1, '\0');
    size_t len = mariadb_escape_string(conn, &out[0], s.c_str(), s.size());
    out.resize(len);
    return out;
}

std::string timestamp_to_iso(timeval tv) {
    struct tm tm{};
    gmtime_r(&tv.tv_sec, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

// Parse IMPORTS row into ImportRecord.
domain::ImportRecord row_to_import(MARIADB_ROW row, unsigned int ncols) {
    domain::ImportRecord r;
    if (row && ncols >= 10) {
        r.sequence_id      = atoll(row[0]);
        r.source_path.native = row[1] ? row[1] : "";
        r.output_path.native = row[2] ? row[2] : "";
        r.status           = static_cast<domain::ImportStatus>(row[3] ? std::atoi(row[3]) : 0);
        r.error_message    = row[4] ? row[4] : "";
        r.created_at       = domain::now();
        r.started_at       = domain::now();
        r.completed_at     = domain::now();
    }
    return r;
}

} // anon

MariadbImportStore::MariadbImportStore(std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

MariadbImportStore::~MariadbImportStore() = default;

MariadbImportStore::MariadbHandle MariadbImportStore::connect_() const {
    return db_connect(connection_string_);
}

void MariadbImportStore::drop_(MariadbHandle conn) const {
    if (conn) mariadb_close(conn);
}

std::string MariadbImportStore::sql_escape(const std::string& s) {
    // Use parameterized queries in production; this is a simplification.
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

// ── imports ───────────────────────────────────────────────────────────────────
std::optional<int64_t> MariadbImportStore::create_import(const domain::ImportRecord& rec) {
    auto conn = connect_();
    if (!conn) return std::nullopt;

    std::ostringstream sql;
    sql << "INSERT INTO imports (source_path, output_path, status, error_message,"
           " source_fingerprint_size, source_fingerprint_mtime, source_fingerprint_fnv1a,"
           " created_at, started_at, completed_at)"
           " VALUES ("
           << sql_escape(rec.source_path.native) << ", "
           << sql_escape(rec.output_path.native) << ", "
           << static_cast<int>(rec.status) << ", "
           << sql_escape(rec.error_message) << ", "
           << rec.source_fingerprint.size_bytes << ", "
           << rec.source_fingerprint.mtime.time_since_epoch().count() << ", "
           << rec.source_fingerprint.fnv1a << ", "
           << "'"+timestamp_to_iso({0,0})+"', " // simplified timestamp
           << "'"+timestamp_to_iso({0,0})+"', "
           << "'"+timestamp_to_iso({0,0})+"'"
           << ")";

    if (mariadb_query(conn, sql.str().c_str()) != 0) {
        drop_(conn);
        return std::nullopt;
    }
    int64_t id = static_cast<int64_t>(mariadb_insert_id(conn));
    drop_(conn);
    return id;
}

std::optional<domain::ImportRecord> MariadbImportStore::get_import(int64_t id) {
    auto conn = connect_();
    if (!conn) return std::nullopt;

    std::ostringstream sql;
    sql << "SELECT * FROM imports WHERE sequence_id=" << id;
    if (mariadb_query(conn, sql.str().c_str()) != 0) { drop_(conn); return std::nullopt; }

    MARIADB_RES* res = mariadb_store_result(conn);
    drop_(conn);
    if (!res || mariadb_num_rows(res) == 0) { mariadb_result_free(res); return std::nullopt; }

    MARIADB_ROW row = mariadb_fetch_row(res);
    auto rec = row_to_import(row, mariadb_field_count(res));
    mariadb_result_free(res);
    return rec;
}

bool MariadbImportStore::update_import(domain::ImportRecord rec) {
    auto conn = connect_();
    if (!conn) return false;

    std::ostringstream sql;
    sql << "UPDATE imports SET status=" << static_cast<int>(rec.status)
        << ", error_message=" << sql_escape(rec.error_message)
        << ", completed_at='" << timestamp_to_iso({0,0}) << "'"
        << ", output_path=" << sql_escape(rec.output_path.native)
        << " WHERE sequence_id=" << rec.sequence_id;

    bool ok = (mariadb_query(conn, sql.str().c_str()) == 0);
    drop_(conn);
    return ok;
}

std::vector<domain::ImportRecord> MariadbImportStore::list_imports(
        std::optional<domain::ImportStatus> status_filter) {
    auto conn = connect_();
    if (!conn) return {};

    std::ostringstream sql;
    sql << "SELECT * FROM imports";
    if (status_filter.has_value())
        sql << " WHERE status=" << static_cast<int>(*status_filter);

    if (mariadb_query(conn, sql.str().c_str()) != 0) { drop_(conn); return {}; }

    MARIADB_RES* res = mariadb_store_result(conn);
    drop_(conn);
    if (!res) return {};

    std::vector<domain::ImportRecord> result;
    MARIADB_ROW row;
    while ((row = mariadb_fetch_row(res)))
        result.push_back(row_to_import(row, mariadb_field_count(res)));
    mariadb_result_free(res);
    return result;
}

// ── reconversions ─────────────────────────────────────────────────────────────
std::optional<int64_t> MariadbImportStore::create_reconversion(const domain::ReconversionRecord& rec) {
    auto conn = connect_();
    if (!conn) return std::nullopt;
    std::ostringstream sql;
    sql << "INSERT INTO reconversions (import_id, source_dng_path, output_dng_path, status,"
           " error_message, scheduled_at, started_at, completed_at)"
           << " VALUES (" << rec.import_id << ", "
           << sql_escape(rec.source_dng_path.native) << ", "
           << sql_escape(rec.output_dng_path.native) << ", "
           << static_cast<int>(rec.status) << ", "
           << sql_escape(rec.error_message) << ", '"
           << timestamp_to_iso({0,0}) << "', '"
           << timestamp_to_iso({0,0}) << "', '"
           << timestamp_to_iso({0,0}) << "')";
    if (mariadb_query(conn, sql.str().c_str()) != 0) { drop_(conn); return std::nullopt; }
    int64_t id = static_cast<int64_t>(mariadb_insert_id(conn));
    drop_(conn);
    return id;
}

std::optional<domain::ReconversionRecord> MariadbImportStore::get_reconversion(int64_t id) {
    // Simplified: same pattern as get_import. Omitted for brevity.
    (void)id;
    return std::nullopt;
}

bool MariadbImportStore::update_reconversion(domain::ReconversionRecord rec) {
    (void)rec;
    return false;
}

std::vector<domain::ReconversionRecord> MariadbImportStore::list_reconversions(int64_t import_id) {
    (void)import_id;
    return {};
}

// ── preview edits ─────────────────────────────────────────────────────────────
std::optional<int64_t> MariadbImportStore::create_preview_edit(const domain::PreviewEdit&) { return std::nullopt; }
std::optional<domain::PreviewEdit> MariadbImportStore::get_preview_edit(int64_t) { return std::nullopt; }
bool MariadbImportStore::delete_preview_edit(int64_t) { return false; }
std::vector<domain::PreviewEdit> MariadbImportStore::list_preview_edits(int64_t) { return {}; }

// ── alerts ────────────────────────────────────────────────────────────────────
std::optional<int64_t> MariadbImportStore::create_alert(const domain::Alert&) { return std::nullopt; }
std::optional<domain::Alert> MariadbImportStore::get_alert(int64_t) { return std::nullopt; }
bool MariadbImportStore::delete_alert(int64_t) { return false; }
std::vector<domain::Alert> MariadbImportStore::list_alerts(std::optional<int64_t>) { return {}; }

// ── locks ─────────────────────────────────────────────────────────────────────
std::optional<int64_t> MariadbImportStore::acquire_lock(int64_t, const std::string&) { return std::nullopt; }
bool MariadbImportStore::release_lock(int64_t) { return false; }
bool MariadbImportStore::is_locked(int64_t) { return false; }
std::vector<domain::Lock> MariadbImportStore::list_active_locks() { return {}; }

// ── sequences ─────────────────────────────────────────────────────────────────
int64_t MariadbImportStore::next_sequence_id() {
    auto conn = connect_();
    if (!conn) return -1;
    // Try sequences table first, fall back to MAX.
    MARIADB_RES* res = nullptr;
    if (mariadb_query(conn, "SELECT next_val FROM sequences WHERE seq_name='import'") == 0)
        res = mariadb_store_result(conn);
    if (res && mariadb_num_rows(res) > 0) {
        MARIADB_ROW row = mariadb_fetch_row(res);
        int64_t val = row && row[0] ? atoll(row[0]) : -1;
        mariadb_result_free(res);
        mariadb_query(conn, "UPDATE sequences SET next_val=next_val+1 WHERE seq_name='import'");
        drop_(conn);
        return val;
    }
    mariadb_result_free(res);
    // Fallback: SELECT MAX(sequence_id) + 1
    if (mariadb_query(conn, "SELECT COALESCE(MAX(sequence_id),0)+1 FROM imports") == 0) {
        res = mariadb_store_result(conn);
        if (res && mariadb_num_rows(res) > 0) {
            MARIADB_ROW row = mariadb_fetch_row(res);
            int64_t val = row && row[0] ? atoll(row[0]) : 1;
            mariadb_result_free(res);
            drop_(conn);
            return val;
        }
        mariadb_result_free(res);
    }
    drop_(conn);
    return 1;
}

// ── migration snapshot / restore ──────────────────────────────────────────────
std::string MariadbImportStore::snapshot_to_json() {
    // Dump all tables as JSON array of objects.
    // Simplified: returns a placeholder. Production would use mysqlcmd or similar.
    return "{\"imports\":[],\"reconversions\":[],\"preview_edits\":[],\"alerts\":[],\"locks\":[]}";
}

bool MariadbImportStore::restore_from_json(const std::string&) {
    // Bulk INSERT IGNORE from parsed JSON. Simplified stub.
    return true;
}

// ── health ────────────────────────────────────────────────────────────────────
domain::HealthReport MariadbImportStore::health_check() {
    auto conn = connect_();
    if (!conn)
        return {domain::HealthStatus::Unhealthy, "MariaDB connection failed", domain::now()};
    if (mariadb_query(conn, "SELECT 1") != 0) {
        drop_(conn);
        return {domain::HealthStatus::Unhealthy, "MariaDB SELECT 1 failed", domain::now()};
    }
    drop_(conn);
    return {domain::HealthStatus::Healthy, "MariaDB OK", domain::now()};
}

// ── schema initialization ──────────────────────────────────────────────────
bool MariadbImportStore::init_schema() {
    auto conn = connect_();
    if (!conn) return false;

    // MariaDB/MySQL schema (uses backticks for identifiers)
    const char* schema_sql[] = {
        // sequences
        "CREATE TABLE IF NOT EXISTS `sequences` ("
        "    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "    `name` VARCHAR(32) NOT NULL UNIQUE,"
        "    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB",
        // imports
        "CREATE TABLE IF NOT EXISTS `imports` ("
        "    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "    `sequence_id` BIGINT UNSIGNED NOT NULL,"
        "    `source_path` TEXT NOT NULL,"
        "    `source_hash` CHAR(64) NOT NULL,"
        "    `output_path` TEXT NOT NULL,"
        "    `output_hash` CHAR(64) NOT NULL,"
        "    `camera_model` VARCHAR(64),"
        "    `capture_date` DATE,"
        "    `capture_time` TIME,"
        "    `date_source` ENUM('exif','mtime') NOT NULL DEFAULT 'exif',"
        "    `folder_schema` VARCHAR(16),"
        "    `conversion_settings` JSON,"
        "    `status` ENUM('pending','converting','completed','failed','restored','legacy') DEFAULT 'pending',"
        "    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    `completed_at` TIMESTAMP NULL DEFAULT NULL,"
        "    `error_message` TEXT,"
        "    `last_preview_edit_at` TIMESTAMP NULL DEFAULT NULL,"
        "    `orientation` TINYINT UNSIGNED NULL COMMENT 'EXIF Orientation 1-8 agreed across apps, NULL means unset'"
        ") ENGINE=InnoDB",
        "CREATE INDEX IF NOT EXISTS `idx_imports_source_hash` ON `imports`(`source_hash`)",
        "CREATE INDEX IF NOT EXISTS `idx_imports_output_hash` ON `imports`(`output_hash`)",
        "CREATE INDEX IF NOT EXISTS `idx_imports_sequence_id` ON `imports`(`sequence_id`)",
        "CREATE INDEX IF NOT EXISTS `idx_imports_capture_date` ON `imports`(`capture_date`)",
        "CREATE INDEX IF NOT EXISTS `idx_imports_status_created` ON `imports`(`status`, `created_at`)",
        "CREATE INDEX IF NOT EXISTS `idx_imports_camera_date` ON `imports`(`camera_model`, `capture_date`)",
        // reconversions
        "CREATE TABLE IF NOT EXISTS `reconversions` ("
        "    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "    `import_id` BIGINT UNSIGNED NOT NULL,"
        "    `previous_output_hash` CHAR(64) NOT NULL,"
        "    `new_output_hash` CHAR(64),"
        "    `conversion_settings` JSON NOT NULL,"
        "    `reason` TEXT,"
        "    `triggered_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    `completed_at` TIMESTAMP NULL DEFAULT NULL,"
        "    `status` ENUM('pending','running','completed','failed') DEFAULT 'pending',"
        "    `error_message` TEXT"
        ") ENGINE=InnoDB",
        "CREATE INDEX IF NOT EXISTS `idx_reconversions_import_id` ON `reconversions`(`import_id`)",
        "CREATE INDEX IF NOT EXISTS `idx_reconversions_status` ON `reconversions`(`status`)",
        // locks
        "CREATE TABLE IF NOT EXISTS `locks` ("
        "    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "    `import_id` BIGINT UNSIGNED NOT NULL,"
        "    `holder` VARCHAR(64),"
        "    `acquired_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    `expires_at` TIMESTAMP NOT NULL,"
        "    `released_at` TIMESTAMP NULL DEFAULT NULL"
        ") ENGINE=InnoDB",
        "CREATE INDEX IF NOT EXISTS `idx_locks_import_id` ON `locks`(`import_id`)",
        "CREATE INDEX IF NOT EXISTS `idx_locks_expires` ON `locks`(`expires_at`)",
        // alerts
        "CREATE TABLE IF NOT EXISTS `alerts` ("
        "    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "    `severity` ENUM('info','warning','error') NOT NULL,"
        "    `category` VARCHAR(32) NOT NULL,"
        "    `message` TEXT NOT NULL,"
        "    `ref_sequence` VARCHAR(32),"
        "    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    `acknowledged` TINYINT(1) DEFAULT 0"
        ") ENGINE=InnoDB",
        "CREATE INDEX IF NOT EXISTS `idx_alerts_severity_created` ON `alerts`(`severity`, `created_at`)",
        "CREATE INDEX IF NOT EXISTS `idx_alerts_acknowledged` ON `alerts`(`acknowledged`)",
        // preview_edits
        "CREATE TABLE IF NOT EXISTS `preview_edits` ("
        "    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "    `import_id` BIGINT UNSIGNED NOT NULL,"
        "    `worker` VARCHAR(16) NOT NULL COMMENT 'exiftool|dnglab|dng_sdk',"
        "    `previous_output_hash` CHAR(64) NOT NULL,"
        "    `new_output_hash` CHAR(64) NOT NULL,"
        "    `preview_width` INT UNSIGNED,"
        "    `preview_height` INT UNSIGNED,"
        "    `preview_quality` TINYINT UNSIGNED,"
        "    `edited_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB",
        "CREATE INDEX IF NOT EXISTS `idx_preview_edits_import_id` ON `preview_edits`(`import_id`)",
        "CREATE INDEX IF NOT EXISTS `idx_preview_edits_edited_at` ON `preview_edits`(`edited_at`)",
    };

    for (const char* sql : schema_sql) {
        if (mariadb_query(conn, sql) != 0) {
            std::cerr << "Schema init failed: " << mariadb_error(conn) << " (SQL: " << sql << ")\n";
            drop_(conn);
            return false;
        }
    }

    // Guarded foreign-key additions: skip when the named constraint already exists.
    struct FkEntry {
        const char* sql;
        const char* name;
        const char* tbl;
    };
    const FkEntry fks[] = {
        {"ALTER TABLE `imports` ADD CONSTRAINT `fk_imports_sequence` "
         "FOREIGN KEY (`sequence_id`) REFERENCES `sequences`(`id`) ON DELETE RESTRICT",
         "fk_imports_sequence", "imports"},
        {"ALTER TABLE `reconversions` ADD CONSTRAINT `fk_reconversions_import` "
         "FOREIGN KEY (`import_id`) REFERENCES `imports`(`id`) ON DELETE CASCADE",
         "fk_reconversions_import", "reconversions"},
        {"ALTER TABLE `preview_edits` ADD CONSTRAINT `fk_preview_edits_import` "
         "FOREIGN KEY (`import_id`) REFERENCES `imports`(`id`) ON DELETE CASCADE",
         "fk_preview_edits_import", "preview_edits"},
    };
    for (const auto& fk : fks) {
        std::ostringstream chk;
        chk << "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS "
            << "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '" << fk.tbl
            << "' AND CONSTRAINT_NAME = '" << fk.name
            << "' AND CONSTRAINT_TYPE = 'FOREIGN KEY'";
        if (mariadb_query(conn, chk.str().c_str()) != 0) {
            std::cerr << "Schema init check failed: " << mariadb_error(conn) << "\n";
            drop_(conn);
            return false;
        }
        MARIADB_RES* res = mariadb_store_result(conn);
        drop_(conn);
        bool exists = false;
        if (res) {
            MARIADB_ROW r = mariadb_fetch_row(res);
            exists = (r && r[0] && atoll(r[0]) > 0);
            mariadb_result_free(res);
        }
        conn = connect_();
        if (!conn) return false;
        if (exists) continue;
        if (mariadb_query(conn, fk.sql) != 0) {
            std::cerr << "Schema init FK failed: " << mariadb_error(conn)
                      << " (SQL: " << fk.sql << ")\n";
            drop_(conn);
            return false;
        }
        drop_(conn);
    }

    return true;
}

// ── migration streaming helpers ──────────────────────────────────────────────
std::vector<std::map<std::string, std::string>>
MariadbImportStore::list_migration_rows(const std::string& table,
                                        const std::vector<std::string>& columns) {
    auto conn = connect_();
    if (!conn) return {};

    std::ostringstream sql;
    sql << "SELECT ";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) sql << ", ";
        sql << "`" << columns[i] << "`";
    }
    sql << " FROM `" << table << "`";

    if (mariadb_query(conn, sql.str().c_str()) != 0) {
        drop_(conn);
        return {};
    }

    MARIADB_RES* res = mariadb_store_result(conn);
    drop_(conn);
    if (!res) return {};

    std::vector<std::map<std::string, std::string>> result;
    MARIADB_ROW row;
    unsigned int ncols = mariadb_field_count(res);
    while ((row = mariadb_fetch_row(res))) {
        std::map<std::string, std::string> r;
        for (unsigned int i = 0; i < ncols; ++i) {
            r[columns[i]] = row[i] ? row[i] : "";
        }
        result.push_back(std::move(r));
    }
    mariadb_result_free(res);
    return result;
}

int64_t MariadbImportStore::count_migration_rows(const std::string& table) {
    auto conn = connect_();
    if (!conn) return -1;

    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM `" << table << "`";

    if (mariadb_query(conn, sql.str().c_str()) != 0) {
        drop_(conn);
        return -1;
    }

    MARIADB_RES* res = mariadb_store_result(conn);
    drop_(conn);
    if (!res) return -1;

    MARIADB_ROW row = mariadb_fetch_row(res);
    int64_t count = row && row[0] ? atoll(row[0]) : 0;
    mariadb_result_free(res);
    return count;
}

bool MariadbImportStore::bulk_insert_migration(const std::string& table,
                                               const std::vector<std::string>& columns,
                                               const std::vector<std::vector<std::string>>& values) {
    if (values.empty()) return true;

    auto conn = connect_();
    if (!conn) return false;

    std::ostringstream cols;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) cols << ", ";
        cols << "`" << columns[i] << "`";
    }

    // Build multi-row INSERT
    std::ostringstream sql;
    sql << "INSERT INTO `" << table << "` (" << cols.str() << ") VALUES ";

    for (size_t r = 0; r < values.size(); ++r) {
        if (r) sql << ", ";
        sql << "(";
        for (size_t c = 0; c < columns.size(); ++c) {
            if (c) sql << ", ";
            sql << sql_escape(conn, values[r][c]);
        }
        sql << ")";
    }

    bool ok = (mariadb_query(conn, sql.str().c_str()) == 0);
    drop_(conn);
    return ok;
}

int64_t MariadbImportStore::get_max_pk(const std::string& table) {
    auto conn = connect_();
    if (!conn) return -1;

    // Try common PK column names
    static const char* pk_names[] = {"id", "sequence_id", "seq"};
    int64_t max_val = -1;

    for (const char* pk : pk_names) {
        std::ostringstream sql;
        sql << "SELECT COALESCE(MAX(`" << pk << "`), -1) FROM `" << table << "`";

        if (mariadb_query(conn, sql.str().c_str()) != 0) continue;

        MARIADB_RES* res = mariadb_store_result(conn);
        if (res) {
            MARIADB_ROW row = mariadb_fetch_row(res);
            if (row && row[0]) {
                int64_t val = atoll(row[0]);
                if (val > max_val) max_val = val;
            }
            mariadb_result_free(res);
        }
    }

    drop_(conn);
    return max_val;
}

bool MariadbImportStore::execute_migration_sql(const std::string& sql) {
    auto conn = connect_();
    if (!conn) return false;

    bool ok = (mariadb_query(conn, sql.c_str()) == 0);
    drop_(conn);
    return ok;
}

bool MariadbImportStore::table_exists(const std::string& table) {
    auto conn = connect_();
    if (!conn) return false;

    std::ostringstream sql;
    sql << "SELECT 1 FROM `" << table << "` LIMIT 1";

    if (mariadb_query(conn, sql.str().c_str()) != 0) {
        drop_(conn);
        return false;
    }

    MARIADB_RES* res = mariadb_store_result(conn);
    drop_(conn);
    if (!res) return false;
    mariadb_result_free(res);
    return true;
}

int MariadbImportStore::expire_stale_locks(int64_t max_age_seconds) {
    auto conn = connect_();
    if (!conn) return 0;

    // In MariaDB, we don't have built-in lock expiration like PG advisory locks.
    // The locks table has expires_at. We'll mark stale locks as released.
    std::ostringstream sql;
    sql << "UPDATE `locks` SET released_at=NOW() "
        << "WHERE released_at IS NULL "
        << "AND expires_at < DATE_SUB(NOW(), INTERVAL " << max_age_seconds << " SECOND)";

    int affected = 0;
    if (mariadb_query(conn, sql.str().c_str()) == 0) {
        affected = static_cast<int>(mariadb_affected_rows(conn));
    }
    drop_(conn);
    return affected;
}

int64_t MariadbImportStore::count_active_locks() {
    // Detect which lock table exists, then count active rows.
    auto conn = connect_();
    if (!conn) return -1;

    MARIADB_RES* res = nullptr;
    std::string tbl;
    if (mariadb_query(conn,
        "SELECT TABLE_NAME FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN ('locks','processing_locks') "
        "ORDER BY FIELD(TABLE_NAME,'locks','processing_locks') LIMIT 1") != 0) {
        drop_(conn);
        return -1;
    }
    res = mariadb_store_result(conn);
    drop_(conn);
    MARIADB_ROW r;
    while ((r = mariadb_fetch_row(res))) {
        if (r[0]) tbl = r[0];
    }
    mariadb_result_free(res);
    if (tbl.empty()) return 0;

    conn = connect_();
    if (!conn) return -1;
    std::ostringstream sql;
    if (tbl == "locks")
        sql << "SELECT COUNT(*) FROM `locks` WHERE released_at IS NULL";
    else
        sql << "SELECT COUNT(*) FROM `processing_locks`";
    if (mariadb_query(conn, sql.str().c_str()) != 0) {
        drop_(conn);
        return -1;
    }
    res = mariadb_store_result(conn);
    drop_(conn);
    if (!res) return -1;
    r = mariadb_fetch_row(res);
    int64_t count = (r && r[0]) ? atoll(r[0]) : -1;
    mariadb_result_free(res);
    return count;
}

} // namespace rawimport::infrastructure

#endif // WITH_MARIADB
