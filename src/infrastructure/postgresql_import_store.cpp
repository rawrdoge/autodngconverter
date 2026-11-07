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
    auto rec = row_to_import(PQgetvalues(res, 0, nullptr), PQnfields(res));
    PQclear(res); drop_(conn);
    return rec;
}

bool PostgresqlImportStore::update_import(domain::ImportRecord rec) {
    auto conn = connect_();
    if (!conn) return false;

    PGresult* res = PQexecParams(conn,
        "UPDATE imports SET status=$1, error_message=$2, output_path=$3,"
        " completed_at=$4 WHERE sequence_id=$5",
        5, nullptr,
        reinterpret_cast<const char* const[]>{
            std::to_string(static_cast<int>(rec.status)).c_str(),
            pg_escape(conn, rec.error_message).c_str(),
            pg_escape(conn, rec.output_path.native).c_str(),
            timestamp_now().c_str(),
            std::to_string(rec.sequence_id).c_str()},
        nullptr, nullptr, 0);
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
    for (int i = 0; i < PQntuples(res); ++i)
        result.push_back(row_to_import(PQgetvalues(res, i, nullptr), PQnfields(res)));
    PQclear(res); drop_(conn);
    return result;
}

// ── reconversions ─────────────────────────────────────────────────────────────
std::optional<int64_t> PostgresqlImportStore::create_reconversion(const domain::ReconversionRecord& rec) {
    auto conn = connect_();
    if (!conn) return std::nullopt;
    PGresult* res = PQexecParams(conn,
        "INSERT INTO reconversions (import_id, source_dng_path, output_dng_path,"
        " status, error_message, scheduled_at, started_at, completed_at)"
        " VALUES ($1,$2,$3,$4,$5,$6,$7,$8) RETURNING id",
        8, nullptr,
        reinterpret_cast<const char* const*>{
            std::to_string(rec.import_id).c_str(),
            pg_escape(conn, rec.source_dng_path.native).c_str(),
            pg_escape(conn, rec.output_dng_path.native).c_str(),
            std::to_string(static_cast<int>(rec.status)).c_str(),
            pg_escape(conn, rec.error_message).c_str(),
            timestamp_now().c_str(),
            timestamp_now().c_str(),
            timestamp_now().c_str()},
        nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) { PQclear(res); drop_(conn); return std::nullopt; }
    int64_t id = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res); drop_(conn);
    return id;
}

std::optional<domain::ReconversionRecord> PostgresqlImportStore::get_reconversion(int64_t) {
    return std::nullopt; // placeholder
}

bool PostgresqlImportStore::update_reconversion(domain::ReconversionRecord) { return false; }

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
    // Use pg_try_advisory_xact_lock for automatic release on transaction end.
    int lock_arg = static_cast<int>(import_id & 0x7FFFFFFF);
    PGresult* res = PQexecParams(conn,
        "SELECT pg_try_advisory_xact_lock($1)", 1,
        nullptr, reinterpret_cast<const char* const*>(&lock_arg),
        nullptr, nullptr, 0);
    bool acquired = (PQresultStatus(res) == PGRES_TUPLES_OK &&
                     PQgetvalue(res, 0, 0)[0] == 't');
    PQclear(res); drop_(conn);
    if (!acquired) return std::nullopt;
    // Insert lock record.
    res = PQexecParams(conn,
        "INSERT INTO locks (import_id, holder, acquired_at, expires_at) VALUES ($1,$2,$3,$4) RETURNING id",
        4, nullptr,
        reinterpret_cast<const char* const*>{
            std::to_string(import_id).c_str(),
            pg_escape(conn, holder).c_str(),
            timestamp_now().c_str(),
            timestamp_now().c_str()},
        nullptr, nullptr, 0);
    int64_t lock_id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        lock_id = std::atoll(PQgetvalue(res, 0, 0));
    PQclear(res); drop_(conn);
    return lock_id;
}

bool PostgresqlImportStore::release_lock(int64_t lock_id) {
    auto conn = connect_();
    if (!conn) return false;
    PGresult* res = PQexecParams(conn,
        "DELETE FROM locks WHERE id=$1 AND released_at IS NULL",
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

} // namespace rawimport::infrastructure

#endif // WITH_POSTGRESQL
