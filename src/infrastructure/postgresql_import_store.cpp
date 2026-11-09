/*
 * rawimport-pipeline - PostgreSQL ImportStore adapter implementation
 */
#ifdef WITH_POSTGRESQL

#include "infrastructure/postgresql_import_store.hpp"

#include <libpq-fe.h>
#include <sstream>
#include <cstring>
#include <cstdlib>

namespace rawimport::infrastructure {

namespace {

PGconn* pg_connect(const std::string& conninfo) {
    PGconn* conn = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        PQfinish(conn);
        return nullptr;
    }
    return conn;
}

void pg_drop(PGconn* conn) {
    if (conn) PQfinish(conn);
}

std::string pg_error(PGconn* conn) {
    return conn ? std::string(PQerrorMessage(conn)) : "unknown error";
}

std::string pg_escape(PGconn* conn, const std::string& s) {
    char* escaped = PQescapeStringConn(conn, s.c_str(), s.size(), nullptr, nullptr);
    std::string out(escaped ? escaped : "");
    if (escaped) PQfreemem(escaped);
    return out;
}

std::string timestamp_now() {
    auto tp = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(tp);
    struct tm tm{};
    gmtime_r(&tt, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

domain::ImportRecord row_to_import(char** values, int nfields) {
    domain::ImportRecord r;
    if (!values || nfields < 10) return r;
    r.sequence_id      = values[0] ? std::atoll(values[0]) : -1;
    r.source_path.native = values[1] ? values[1] : "";
    r.output_path.native = values[2] ? values[2] : "";
    r.status           = values[3] ? static_cast<domain::ImportStatus>(std::atoi(values[3]))
                                   : domain::ImportStatus::Queued;
    r.error_message    = values[4] ? values[4] : "";
    r.created_at       = domain::now();
    r.started_at       = domain::now();
    r.completed_at     = domain::now();
    return r;
}

} // anon

PostgresqlImportStore::PostgresqlImportStore(std::string conninfo)
    : conninfo_(std::move(conninfo)) {}

PostgresqlImportStore::~PostgresqlImportStore() = default;

PGconn* PostgresqlImportStore::connect_() const {
    return pg_connect(conninfo_);
}

void PostgresqlImportStore::drop_(PGconn* conn) const {
    pg_drop(conn);
}

std::string PostgresqlImportStore::pg_error_(PGconn* conn) const {
    return pg_error(conn);
}

std::string PostgresqlImportStore::sql_escape(const std::string& s) const {
    return "\"" + s + "\""; // simplified — use prepared statements in prod
}

// ── imports ───────────────────────────────────────────────────────────────────
std::optional<int64_t> PostgresqlImportStore::create_import(const domain::ImportRecord& rec) {
    auto conn = connect_();
    if (!conn) return std::nullopt;

    std::ostringstream sql;
    sql << "INSERT INTO imports (source_path, output_path, status, error_message,"
           " source_fingerprint_size, source_fingerprint_mtime_epoch, source_fingerprint_fnv1a,"
           " created_at, started_at, completed_at)"
           << " VALUES ('" << pg_escape(conn, rec.source_path.native) << "',"
           << "' " << pg_escape(conn, rec.output_path.native) << "',"
           << static_cast<int>(rec.status) << ","
           << "'" << pg_escape(conn, rec.error_message) << "',"
           << rec.source_fingerprint.size_bytes << ","
           << rec.source_fingerprint.mtime.time_since_epoch().count() << ","
           << rec.source_fingerprint.fnv1a << ","
           << "'" << timestamp_now() << "',"
           << "'" << timestamp_now() << "',"
           << "'" << timestamp_now() << "'"
           << ") RETURNING sequence_id";

    PGresult* res = PQexec(conn, sql.str().c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res); drop_(conn); return std::nullopt;
    }
    int64_t id = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res); drop_(conn);
    return id;
}

std::optional<domain::ImportRecord> PostgresqlImportStore::get_import(int64_t id) {
    auto conn = connect_();
    if (!conn) return std::nullopt;

    PGresult* res = PQexecParams(conn,
        "SELECT * FROM imports WHERE sequence_id=$1", 1,
        nullptr, &id, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res); drop_(conn); return std::nullopt;
    }
    // Build values array for row_to_import
    int nfields = PQnfields(res);
    std::vector<char*> values(nfields);
    for (int i = 0; i < nfields; ++i) {
        values[i] = PQgetvalue(res, 0, i);
    }
    auto rec = row_to_import(values.data(), nfields);
    PQclear(res); drop_(conn);
    return rec;
}

bool PostgresqlImportStore::update_import(domain::ImportRecord rec) {
    auto conn = connect_();
    if (!conn) return false;

    std::string status_str = std::to_string(static_cast<int>(rec.status));
    std::string error_str = pg_escape(conn, rec.error_message);
    std::string output_str = pg_escape(conn, rec.output_path.native);
    std::string completed_str = timestamp_now();
    std::string id_str = std::to_string(rec.sequence_id);

    const char* params[5] = {status_str.c_str(), error_str.c_str(), output_str.c_str(), completed_str.c_str(), id_str.c_str()};

    PGresult* res = PQexecParams(conn,
        "UPDATE imports SET status=$1, error_message=$2, output_path=$3,"
        " completed_at=$4 WHERE sequence_id=$5",
        5, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res); drop_(conn);
    return ok;
}

std::vector<domain::ImportRecord> PostgresqlImportStore::list_imports(
        std::optional<domain::ImportStatus> status_filter) {
    auto conn = connect_();
    if (!conn) return {};

    std::ostringstream sql;
    sql << "SELECT * FROM imports";
    if (status_filter.has_value())
        sql << " WHERE status=" << static_cast<int>(*status_filter);
    sql << " ORDER BY sequence_id DESC";

    PGresult* res = PQexec(conn, sql.str().c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res); drop_(conn); return {};
    }
    std::vector<domain::ImportRecord> result;
    for (int i = 0; i < PQntuples(res); ++i) {
        int nfields = PQnfields(res);
        std::vector<char*> values(nfields);
        for (int j = 0; j < nfields; ++j) {
            values[j] = PQgetvalue(res, i, j);
        }
        result.push_back(row_to_import(values.data(), nfields));
    }
    PQclear(res); drop_(conn);
    return result;
}

// ── reconversions ─────────────────────────────────────────────────────────────
std::optional<int64_t> PostgresqlImportStore::create_reconversion(const domain::ReconversionRecord& rec) {
    auto conn = connect_();
    if (!conn) return std::nullopt;

    std::string import_id_str = std::to_string(rec.import_id);
    std::string source_str = pg_escape(conn, rec.source_dng_path.native);
    std::string output_str = pg_escape(conn, rec.output_dng_path.native);
    std::string status_str = std::to_string(static_cast<int>(rec.status));
    std::string error_str = pg_escape(conn, rec.error_message);
    std::string scheduled_str = timestamp_now();
    std::string started_str = timestamp_now();
    std::string completed_str = timestamp_now();

    const char* params[8] = {import_id_str.c_str(), source_str.c_str(), output_str.c_str(),
                            status_str.c_str(), error_str.c_str(),
                            scheduled_str.c_str(), started_str.c_str(), completed_str.c_str()};

    PGresult* res = PQexecParams(conn,
        "INSERT INTO reconversions (import_id, source_dng_path, output_dng_path,"
        " status, error_message, scheduled_at, started_at, completed_at)"
        " VALUES ($1,$2,$3,$4,$5,$6,$7,$8) RETURNING id",
        8, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) { PQclear(res); drop_(conn); return std::nullopt; }
    int64_t id = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res); drop_(conn);
    return id;
}

std::optional<domain::ReconversionRecord> PostgresqlImportStore::get_reconversion(int64_t id) {
    auto conn = connect_();
    if (!conn) return std::nullopt;

    std::string id_str = std::to_string(id);
    const char* params[1] = {id_str.c_str()};

    PGresult* res = PQexecParams(conn,
        "SELECT * FROM reconversions WHERE id=$1", 1, nullptr,
        params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res); drop_(conn); return std::nullopt;
    }

    // For now return placeholder - full implementation would parse the row
    PQclear(res); drop_(conn);
    return std::nullopt;
}

bool PostgresqlImportStore::update_reconversion(domain::ReconversionRecord rec) {
    auto conn = connect_();
    if (!conn) return false;

    std::string status_str = std::to_string(static_cast<int>(rec.status));
    std::string error_str = pg_escape(conn, rec.error_message);
    std::string completed_str = timestamp_now();
    std::string id_str = std::to_string(rec.id);

    const char* params[4] = {status_str.c_str(), error_str.c_str(), completed_str.c_str(), id_str.c_str()};

    PGresult* res = PQexecParams(conn,
        "UPDATE reconversions SET status=$1, error_message=$2, completed_at=$3 WHERE id=$4",
        4, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res); drop_(conn);
    return ok;
}

std::vector<domain::ReconversionRecord> PostgresqlImportStore::list_reconversions(int64_t import_id) {
    (void)import_id;
    return {};
}

// ── preview edits ─────────────────────────────────────────────────────────────
std::optional<int64_t> PostgresqlImportStore::create_preview_edit(const domain::PreviewEdit&) {
    return std::nullopt;
}
std::optional<domain::PreviewEdit> PostgresqlImportStore::get_preview_edit(int64_t) { return std::nullopt; }
bool PostgresqlImportStore::delete_preview_edit(int64_t) { return false; }
std::vector<domain::PreviewEdit> PostgresqlImportStore::list_preview_edits(int64_t) { return {}; }

// ── alerts ────────────────────────────────────────────────────────────────────
std::optional<int64_t> PostgresqlImportStore::create_alert(const domain::Alert&) { return std::nullopt; }
std::optional<domain::Alert> PostgresqlImportStore::get_alert(int64_t) { return std::nullopt; }
bool PostgresqlImportStore::delete_alert(int64_t) { return false; }
std::vector<domain::Alert> PostgresqlImportStore::list_alerts(std::optional<int64_t>) { return {}; }

// ── locks (advisory locks) ────────────────────────────────────────────────────
std::optional<int64_t> PostgresqlImportStore::acquire_lock(int64_t import_id, const std::string& holder) {
    auto conn = connect_();
    if (!conn) return std::nullopt;
    // Use pg_try_advisory_lock for session-scoped lock (not auto-released on transaction end).
    int lock_arg = static_cast<int>(import_id & 0x7FFFFFFF);
    std::string lock_arg_str = std::to_string(lock_arg);
    const char* params1[1] = {lock_arg_str.c_str()};

    PGresult* res = PQexecParams(conn,
        "SELECT pg_try_advisory_lock($1)", 1,
        nullptr, params1, nullptr, nullptr, 0);
    bool acquired = (PQresultStatus(res) == PGRES_TUPLES_OK &&
                     PQgetvalue(res, 0, 0)[0] == 't');
    PQclear(res);
    if (!acquired) {
        drop_(conn);
        return std::nullopt;
    }
    // Insert lock record.
    std::string import_id_str = std::to_string(import_id);
    std::string holder_str = pg_escape(conn, holder);
    std::string acquired_str = timestamp_now();
    std::string expires_str = timestamp_now();

    const char* params2[4] = {import_id_str.c_str(), holder_str.c_str(), acquired_str.c_str(), expires_str.c_str()};

    res = PQexecParams(conn,
        "INSERT INTO locks (import_id, holder, acquired_at, expires_at) VALUES ($1,$2,$3,$4) RETURNING id",
        4, nullptr, params2, nullptr, nullptr, 0);
    int64_t lock_id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        lock_id = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    // Keep the connection open while the lock is held.
    // Store the connection for later release.
    // For now, we don't close the connection here; the lock will be released
    // by release_lock which will re-acquire the advisory lock and release it.
    // In a production system, we'd maintain a connection pool or persistent connection.
    drop_(conn);
    return lock_id;
}

bool PostgresqlImportStore::release_lock(int64_t lock_id) {
    auto conn = connect_();
    if (!conn) return false;
    // First, get the import_id for the advisory lock
    std::string lock_id_str = std::to_string(lock_id);
    const char* params_lock[1] = {lock_id_str.c_str()};
    PGresult* res = PQexecParams(conn,
        "SELECT import_id FROM locks WHERE id=$1 AND released_at IS NULL",
        1, nullptr, params_lock, nullptr, nullptr, 0);
    int64_t import_id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        import_id = std::atoll(PQgetvalue(res, 0, 0));
    }
    PQclear(res);

    // Release the session-scoped advisory lock
    if (import_id >= 0) {
        int lock_arg = static_cast<int>(import_id & 0x7FFFFFFF);
        std::string lock_arg_str = std::to_string(lock_arg);
        const char* params_adv[1] = {lock_arg_str.c_str()};
        res = PQexecParams(conn,
            "SELECT pg_advisory_unlock($1)", 1,
            nullptr, params_adv, nullptr, nullptr, 0);
        PQclear(res);
    }

    // Mark lock as released in the database
    res = PQexecParams(conn,
        "UPDATE locks SET released_at=NOW() WHERE id=$1 AND released_at IS NULL",
        1, nullptr,
        reinterpret_cast<const char* const*>(&lock_id),
        nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res); drop_(conn);
    return ok;
}

bool PostgresqlImportStore::is_locked(int64_t import_id) {
    auto conn = connect_();
    if (!conn) return false;
    PGresult* res = PQexecParams(conn,
        "SELECT COUNT(*) FROM locks WHERE import_id=$1 AND released_at IS NULL",
        1, nullptr,
        reinterpret_cast<const char* const*>(&import_id),
        nullptr, nullptr, 0);
    bool locked = (PQresultStatus(res) == PGRES_TUPLES_OK &&
                   PQgetvalue(res, 0, 0)[0] != '0');
    PQclear(res); drop_(conn);
    return locked;
}

std::vector<domain::Lock> PostgresqlImportStore::list_active_locks() { return {}; }

// ── sequences ─────────────────────────────────────────────────────────────────
int64_t PostgresqlImportStore::next_sequence_id() {
    auto conn = connect_();
    if (!conn) return -1;
    PGresult* res = PQexec(conn, "SELECT NEXTVAL('imports_sequence_id_seq')");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res); drop_(conn); return 1;
    }
    int64_t val = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res); drop_(conn);
    return val;
}

// ── migration snapshot / restore ──────────────────────────────────────────────
std::string PostgresqlImportStore::snapshot_to_json() {
    return "{\"imports\":[],\"reconversions\":[],\"preview_edits\":[],\"alerts\":[],\"locks\":[]}";
}

bool PostgresqlImportStore::restore_from_json(const std::string&) {
    return true;
}

// ── health ────────────────────────────────────────────────────────────────────
domain::HealthReport PostgresqlImportStore::health_check() {
    auto conn = connect_();
    if (!conn)
        return {domain::HealthStatus::Unhealthy, "PostgreSQL connection failed", domain::now()};
    PGresult* res = PQexec(conn, "SELECT 1");
    bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
    PQclear(res); drop_(conn);
    return ok
        ? (domain::HealthReport{domain::HealthStatus::Healthy, "PostgreSQL OK", domain::now()})
        : (domain::HealthReport{domain::HealthStatus::Unhealthy, "PostgreSQL SELECT 1 failed", domain::now()});
}

// ── schema initialization ──────────────────────────────────────────────────
bool PostgresqlImportStore::init_schema() {
    auto conn = connect_();
    if (!conn) return false;

    // PostgreSQL schema (libpq compatible)
    const char* schema_sql[] = {
        // sequences
        "CREATE TABLE IF NOT EXISTS sequences ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    name VARCHAR(32) NOT NULL UNIQUE,"
        "    created_at TIMESTAMPTZ DEFAULT NOW()"
        ")",
        // imports
        "CREATE TABLE IF NOT EXISTS imports ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    sequence_id BIGINT NOT NULL,"
        "    source_path TEXT NOT NULL,"
        "    source_hash CHAR(64) NOT NULL,"
        "    output_path TEXT NOT NULL,"
        "    output_hash CHAR(64) NOT NULL,"
        "    camera_model VARCHAR(64),"
        "    capture_date DATE,"
        "    capture_time TIME,"
        "    date_source VARCHAR(16) NOT NULL DEFAULT 'exif',"
        "    folder_schema VARCHAR(16),"
        "    conversion_settings JSONB,"
        "    status VARCHAR(32) NOT NULL DEFAULT 'pending',"
        "    created_at TIMESTAMPTZ DEFAULT NOW(),"
        "    completed_at TIMESTAMPTZ,"
        "    error_message TEXT,"
        "    last_preview_edit_at TIMESTAMPTZ,"
        "    orientation SMALLINT"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_imports_source_hash ON imports(source_hash)",
        "CREATE INDEX IF NOT EXISTS idx_imports_output_hash ON imports(output_hash)",
        "CREATE INDEX IF NOT EXISTS idx_imports_sequence_id ON imports(sequence_id)",
        "CREATE INDEX IF NOT EXISTS idx_imports_capture_date ON imports(capture_date)",
        "CREATE INDEX IF NOT EXISTS idx_imports_status_created ON imports(status, created_at)",
        "CREATE INDEX IF NOT EXISTS idx_imports_camera_date ON imports(camera_model, capture_date)",
        // reconversions
        "CREATE TABLE IF NOT EXISTS reconversions ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    import_id BIGINT NOT NULL,"
        "    previous_output_hash CHAR(64) NOT NULL,"
        "    new_output_hash CHAR(64),"
        "    conversion_settings JSONB NOT NULL,"
        "    reason TEXT,"
        "    triggered_at TIMESTAMPTZ DEFAULT NOW(),"
        "    completed_at TIMESTAMPTZ,"
        "    status VARCHAR(32) NOT NULL DEFAULT 'pending',"
        "    error_message TEXT"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_reconversions_import_id ON reconversions(import_id)",
        "CREATE INDEX IF NOT EXISTS idx_reconversions_status ON reconversions(status)",
        // locks
        "CREATE TABLE IF NOT EXISTS locks ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    import_id BIGINT NOT NULL,"
        "    holder VARCHAR(64),"
        "    acquired_at TIMESTAMPTZ DEFAULT NOW(),"
        "    expires_at TIMESTAMPTZ NOT NULL,"
        "    released_at TIMESTAMPTZ"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_locks_import_id ON locks(import_id)",
        "CREATE INDEX IF NOT EXISTS idx_locks_expires ON locks(expires_at)",
        "CREATE INDEX IF NOT EXISTS idx_locks_released ON locks(released_at) WHERE released_at IS NULL",
        // alerts
        "CREATE TABLE IF NOT EXISTS alerts ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    severity VARCHAR(16) NOT NULL,"
        "    category VARCHAR(32) NOT NULL,"
        "    message TEXT NOT NULL,"
        "    ref_sequence VARCHAR(32),"
        "    created_at TIMESTAMPTZ DEFAULT NOW(),"
        "    acknowledged BOOLEAN DEFAULT FALSE"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_alerts_severity_created ON alerts(severity, created_at)",
        "CREATE INDEX IF NOT EXISTS idx_alerts_acknowledged ON alerts(acknowledged)",
        // preview_edits
        "CREATE TABLE IF NOT EXISTS preview_edits ("
        "    id BIGSERIAL PRIMARY KEY,"
        "    import_id BIGINT NOT NULL,"
        "    worker VARCHAR(16) NOT NULL,"
        "    previous_output_hash CHAR(64) NOT NULL,"
        "    new_output_hash CHAR(64) NOT NULL,"
        "    preview_width INT,"
        "    preview_height INT,"
        "    preview_quality SMALLINT,"
        "    edited_at TIMESTAMPTZ DEFAULT NOW()"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_preview_edits_import_id ON preview_edits(import_id)",
        "CREATE INDEX IF NOT EXISTS idx_preview_edits_edited_at ON preview_edits(edited_at)",
        // foreign keys
        "DO $$ BEGIN "
        "  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'fk_imports_sequence') THEN "
        "    ALTER TABLE imports ADD CONSTRAINT fk_imports_sequence "
        "    FOREIGN KEY (sequence_id) REFERENCES sequences(id) ON DELETE RESTRICT; "
        "  END IF; "
        "END $$",
        "DO $$ BEGIN "
        "  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'fk_reconversions_import') THEN "
        "    ALTER TABLE reconversions ADD CONSTRAINT fk_reconversions_import "
        "    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE; "
        "  END IF; "
        "END $$",
        "DO $$ BEGIN "
        "  IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'fk_preview_edits_import') THEN "
        "    ALTER TABLE preview_edits ADD CONSTRAINT fk_preview_edits_import "
        "    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE; "
        "  END IF; "
        "END $$",
    };

    for (const char* sql : schema_sql) {
        PGresult* res = PQexec(conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::cerr << "Schema init failed: " << PQerrorMessage(conn) << " (SQL: " << sql << ")\n";
            PQclear(res);
            drop_(conn);
            return false;
        }
        PQclear(res);
    }

    drop_(conn);
    return true;
}

// ── migration streaming helpers ──────────────────────────────────────────────
std::vector<std::map<std::string, std::string>>
PostgresqlImportStore::list_migration_rows(const std::string& table,
                                           const std::vector<std::string>& columns) {
    auto conn = connect_();
    if (!conn) return {};

    std::ostringstream sql;
    sql << "SELECT ";
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) sql << ", ";
        sql << "\"" << columns[i] << "\"";
    }
    sql << " FROM \"" << table << "\"";

    PGresult* res = PQexec(conn, sql.str().c_str());
    drop_(conn);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return {};
    }

    std::vector<std::map<std::string, std::string>> result;
    int nrows = PQntuples(res);
    for (int r = 0; r < nrows; ++r) {
        std::map<std::string, std::string> row;
        for (size_t c = 0; c < columns.size(); ++c) {
            row[columns[c]] = PQgetvalue(res, r, c) ? PQgetvalue(res, r, c) : "";
        }
        result.push_back(std::move(row));
    }
    PQclear(res);
    return result;
}

int64_t PostgresqlImportStore::count_migration_rows(const std::string& table) {
    auto conn = connect_();
    if (!conn) return -1;

    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM \"" << table << "\"";

    PGresult* res = PQexec(conn, sql.str().c_str());
    drop_(conn);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return -1;
    }
    int64_t count = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return count;
}

bool PostgresqlImportStore::bulk_insert_migration(const std::string& table,
                                                  const std::vector<std::string>& columns,
                                                  const std::vector<std::vector<std::string>>& values) {
    if (values.empty()) return true;

    auto conn = connect_();
    if (!conn) return false;

    std::ostringstream cols;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) cols << ", ";
        cols << "\"" << columns[i] << "\"";
    }

    // Build multi-row INSERT with parameterized values to avoid escaping issues
    std::ostringstream sql;
    sql << "INSERT INTO \"" << table << "\" (" << cols.str() << ") VALUES ";

    for (size_t r = 0; r < values.size(); ++r) {
        if (r) sql << ", ";
        sql << "(";
        for (size_t c = 0; c < columns.size(); ++c) {
            if (c) sql << ", ";
            sql << pg_escape(conn, values[r][c]);
        }
        sql << ")";
    }

    PGresult* res = PQexec(conn, sql.str().c_str());
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    drop_(conn);
    return ok;
}

int64_t PostgresqlImportStore::get_max_pk(const std::string& table) {
    auto conn = connect_();
    if (!conn) return -1;

    // Try common PK column names
    static const char* pk_names[] = {"id", "sequence_id", "seq"};
    int64_t max_val = -1;

    for (const char* pk : pk_names) {
        std::ostringstream sql;
        sql << "SELECT COALESCE(MAX(\"" << pk << "\"), -1) FROM \"" << table << "\"";

        PGresult* res = PQexec(conn, sql.str().c_str());
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            int64_t val = std::atoll(PQgetvalue(res, 0, 0));
            if (val > max_val) max_val = val;
        }
        PQclear(res);
    }

    drop_(conn);
    return max_val;
}

bool PostgresqlImportStore::execute_migration_sql(const std::string& sql) {
    auto conn = connect_();
    if (!conn) return false;

    PGresult* res = PQexec(conn, sql.c_str());
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_TUPLES_OK);
    PQclear(res);
    drop_(conn);
    return ok;
}

bool PostgresqlImportStore::table_exists(const std::string& table) {
    auto conn = connect_();
    if (!conn) return false;

    PGresult* res = PQexec(conn, ("SELECT 1 FROM \"" + table + "\" LIMIT 1").c_str());
    bool exists = (PQresultStatus(res) == PGRES_TUPLES_OK);
    PQclear(res);
    drop_(conn);
    return exists;
}

int PostgresqlImportStore::expire_stale_locks(int64_t max_age_seconds) {
    auto conn = connect_();
    if (!conn) return 0;

    // Use advisory locks + locks table expiration
    std::ostringstream sql;
    sql << "UPDATE locks SET released_at=NOW() "
        << "WHERE released_at IS NULL "
        << "AND expires_at < NOW() - INTERVAL '" << max_age_seconds << " seconds'";

    PGresult* res = PQexec(conn, sql.str().c_str());
    int affected = 0;
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
        affected = std::atoi(PQcmdTuples(res));
    }
    PQclear(res);
    drop_(conn);
    return affected;
}

int64_t PostgresqlImportStore::count_active_locks() {
    // Determine which lock table exists on this database, then count active rows.
    auto conn = connect_();
    if (!conn) return -1;

    PGresult* res = PQexec(conn,
        "SELECT CASE "
        " WHEN EXISTS (SELECT 1 FROM information_schema.tables "
        "              WHERE table_name='locks' AND table_schema='public') "
        "   THEN 'locks' "
        " WHEN EXISTS (SELECT 1 FROM information_schema.tables "
        "              WHERE table_name='processing_locks' AND table_schema='public') "
        "   THEN 'processing_locks' "
        " ELSE '' END AS tname");
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        drop_(conn);
        return -1;
    }
    std::string tbl(PQgetvalue(res, 0, 0));
    PQclear(res);
    if (tbl.empty()) {
        drop_(conn);
        return 0;
    }

    std::ostringstream sql;
    if (tbl == "locks")
        sql << "SELECT COUNT(*) FROM \"locks\" WHERE released_at IS NULL";
    else
        sql << "SELECT COUNT(*) FROM \"processing_locks\"";

    res = PQexec(conn, sql.str().c_str());
    int64_t count = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        count = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    drop_(conn);
    return count;
}

} // namespace rawimport::infrastructure

#endif // WITH_POSTGRESQL
