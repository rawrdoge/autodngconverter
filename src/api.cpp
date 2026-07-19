#include "api.h"
#include "config.h"
#include "db.h"
#include "rotation.h"
#include "pipeline.h"
#include "metrics.h"

#include <atomic>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <spdlog/spdlog.h>

namespace rawimport {

struct ApiServer::Impl {
    int listen_fd = -1;
    std::thread accept_thread;
    std::atomic<bool> stop{false};
    const Config* cfg = nullptr;
    Store* store = nullptr;
    RotationManager* rotation_mgr = nullptr;
};

ApiServer::ApiServer(const Config& cfg, Store& store, RotationManager* rotation_mgr)
    : cfg_(cfg), store_(store), rotation_mgr_(rotation_mgr),
      p_(std::make_unique<Impl>()) {
    p_->cfg = &cfg;
    p_->store = &store;
    p_->rotation_mgr = rotation_mgr;
}

ApiServer::~ApiServer() { Stop(); }

namespace {

void send_response(int fd, int code, const std::string& body, const std::string& content_type = "application/json") {
    std::ostringstream os;
    os << "HTTP/1.1 " << code << " ";
    if (code == 200) os << "OK";
    else if (code == 404) os << "Not Found";
    else if (code == 401) os << "Unauthorized";
    else if (code == 503) os << "Service Unavailable";
    else os << "Error";
    os << "\r\nContent-Type: " << content_type << "\r\n";
    os << "Content-Length: " << body.size() << "\r\n";
    os << "Connection: close\r\n\r\n";
    os << body;
    std::string out = os.str();
#ifdef _WIN32
    send(fd, out.c_str(), static_cast<int>(out.size()), 0);
#else
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(out.size())) {
        ssize_t n = ::send(fd, out.c_str() + sent, out.size() - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
#endif
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

std::string record_to_json(const ImportRecord& r) {
    std::ostringstream os;
    os << "{";
    os << "\"sequence\":\"" << json_escape(r.sequence_name) << "\",";
    os << "\"source_path\":\"" << json_escape(r.source_path) << "\",";
    os << "\"source_hash\":\"" << json_escape(r.source_hash) << "\",";
    os << "\"output_path\":\"" << json_escape(r.output_path) << "\",";
    os << "\"output_hash\":\"" << json_escape(r.output_hash) << "\",";
    os << "\"camera_model\":\"" << json_escape(r.camera_model) << "\",";
    os << "\"capture_date\":\"" << json_escape(r.capture_date) << "\",";
    os << "\"capture_time\":\"" << json_escape(r.capture_time) << "\",";
    os << "\"folder_schema\":\"" << json_escape(r.folder_schema) << "\",";
    os << "\"status\":\"" << to_string(r.status) << "\",";
    os << "\"created_at\":\"" << json_escape(r.created_at) << "\",";
    os << "\"completed_at\":\"" << json_escape(r.completed_at) << "\",";
    os << "\"orientation\":" << r.orientation;
    os << "}";
    return os.str();
}

// minimal query-param extractor: ?key=val&...
std::unordered_map<std::string, std::string> parse_query(const std::string& q) {
    std::unordered_map<std::string, std::string> m;
    size_t i = 0;
    while (i < q.size()) {
        size_t eq = q.find('=', i);
        size_t amp = q.find('&', i);
        if (eq == std::string::npos) break;
        std::string k = q.substr(i, eq - i);
        std::string v = (amp == std::string::npos) ? q.substr(eq + 1)
                                                : q.substr(eq + 1, amp - eq - 1);
        m[k] = v;
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return m;
}

// very small JSON value extractor for flat {"key":"val"} or {"key":123}
std::string json_get(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\":";
    size_t pos = body.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    if (pos >= body.size()) return "";
    if (body[pos] == '"') {
        size_t end = body.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return body.substr(pos + 1, end - pos - 1);
    }
    size_t end = pos;
    while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != ' ') ++end;
    return body.substr(pos, end - pos);
}

void handle(int fd, ApiServer::Impl* p) {
    // read request (bounded). First read until we have complete headers, then
    // accumulate the declared body. A per-recv timeout prevents blocking.
    std::string req;
    char buf[4096];
    int total = 0;
    bool headers_done = false;
    long cl = 0;
    while (total < 65536) {
#ifdef _WIN32
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
#else
        struct timeval tv;
        tv.tv_sec = 5; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
#endif
        if (n <= 0) break;
        buf[n] = '\0';
        req += buf;
        total += static_cast<int>(n);
        if (!headers_done) {
            size_t hpos = req.find("\r\n\r\n");
            if (hpos == std::string::npos) continue;
            headers_done = true;
            // parse Content-Length (case-insensitive search)
            size_t clpos = req.find("Content-Length:");
            if (clpos == std::string::npos) clpos = req.find("content-length:");
            if (clpos != std::string::npos) {
                size_t cs = req.find("\r\n", clpos);
                std::string cstr = req.substr(clpos + 15,
                    (cs == std::string::npos ? req.size() : cs) - (clpos + 15));
                try { cl = std::stol(cstr); } catch (...) { cl = 0; }
            }
        }
        size_t header_len = req.find("\r\n\r\n") + 4;
        if (static_cast<long>(req.size()) >= static_cast<long>(header_len) + cl) break; // got full body
    }
    if (req.empty()) { send_response(fd, 400, "{\"error\":\"empty\"}"); return; }

    // parse method + path
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        send_response(fd, 400, "{\"error\":\"bad-request\"}"); return;
    }
    std::string method = req.substr(0, sp1);
    std::string full = req.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qpos = full.find('?');
    std::string path = (qpos == std::string::npos) ? full : full.substr(0, qpos);
    std::string query = (qpos == std::string::npos) ? "" : full.substr(qpos + 1);
    auto q = parse_query(query);

    // auth
    if (path != "/health" && !p->cfg->api_token.empty()) {
        size_t ah = req.find("Authorization:");
        bool ok = false;
        if (ah != std::string::npos) {
            size_t b = req.find("Bearer ", ah);
            if (b != std::string::npos) {
                std::string tok = req.substr(b + 7);
                size_t nl = tok.find("\r\n");
                if (nl != std::string::npos) tok = tok.substr(0, nl);
                if (tok == p->cfg->api_token) ok = true;
            }
        }
        if (!ok) { send_response(fd, 401, "{\"error\":\"unauthorized\"}"); return; }
    }

    Store& store = *p->store;

    if (method == "GET" && path == "/health") {
        send_response(fd, 200, "{\"status\":\"ok\"}");
        return;
    }
    if (method == "GET" && path == "/metrics") {
        send_response(fd, 200, Metrics::instance().Render(), "text/plain; version=0.0.4");
        return;
    }
    if (method == "GET" && path == "/api/v1/stats") {
        auto s = store.GetStats();
        std::ostringstream os;
        os << "{\"total\":" << s.total << ",\"completed\":" << s.completed
           << ",\"failed\":" << s.failed << ",\"queue_depth\":" << s.queue_depth << "}";
        send_response(fd, 200, os.str());
        return;
    }
    if (method == "GET" && path == "/api/v1/alerts") {
        send_response(fd, 200, "{\"alerts\":[]}");
        return;
    }
    if (method == "GET" && path == "/api/v1/imports") {
        int page = q.count("page") ? std::stoi(q["page"]) : 1;
        int limit = q.count("limit") ? std::stoi(q["limit"]) : 50;
        if (page < 1) page = 1; if (limit < 1) limit = 50;
        std::string status_f = q.count("status") ? q["status"] : "";
        std::string camera_f = q.count("camera") ? q["camera"] : "";
        auto rows = store.ListImports(page, limit, status_f, camera_f);
        auto s = store.GetStats();
        std::ostringstream os;
        os << "{\"total\":" << s.total << ",\"page\":" << page << ",\"limit\":" << limit << ",\"data\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) os << ",";
            os << record_to_json(rows[i]);
        }
        os << "]}";
        send_response(fd, 200, os.str());
        return;
    }
    if (method == "GET" && path.rfind("/api/v1/imports/hash/", 0) == 0) {
        std::string sha = path.substr(std::string("/api/v1/imports/hash/").size());
        auto r = store.GetImportByHash(sha);
        if (!r) { send_response(fd, 404, "{\"error\":\"not-found\"}"); return; }
        send_response(fd, 200, record_to_json(*r));
        return;
    }
    if (method == "GET" && path.rfind("/api/v1/imports/by-source", 0) == 0) {
        std::string sp = q.count("path") ? q["path"] : "";
        auto r = store.GetImportBySourcePath(sp);
        if (!r) { send_response(fd, 404, "{\"error\":\"not-found\"}"); return; }
        send_response(fd, 200, record_to_json(*r));
        return;
    }
    if (method == "GET" && path.rfind("/api/v1/imports/", 0) == 0) {
        std::string seq = path.substr(std::string("/api/v1/imports/").size());
        auto r = store.GetImportBySequence(seq);
        if (!r) { send_response(fd, 404, "{\"error\":\"not-found\"}"); return; }
        send_response(fd, 200, record_to_json(*r));
        return;
    }
    if (method == "POST" && path.rfind("/api/v1/imports/", 0) == 0 &&
        path.find("/reconvert") != std::string::npos) {
        std::string seq = path.substr(std::string("/api/v1/imports/").size());
        seq = seq.substr(0, seq.find("/reconvert"));
        auto r = store.GetImportBySequence(seq);
        if (!r) { send_response(fd, 404, "{\"error\":\"not-found\"}"); return; }
        // extract body
        size_t bpos = req.find("\r\n\r\n");
        std::string body = (bpos != std::string::npos) ? req.substr(bpos + 4) : "";
        ReconversionJob job;
        job.import_id = r->id;
        job.previous_output_hash = r->output_hash;
        job.settings.compression = json_get(body, "compression");
        if (job.settings.compression.empty()) job.settings.compression = "lossless";
        job.reason = json_get(body, "reason");
        int64_t rid = store.InsertReconversion(r->id, job);
        std::ostringstream os;
        os << "{\"reconversion_id\":" << rid << ",\"sequence\":\"" << json_escape(seq)
           << "\",\"status\":\"pending\",\"queued_at\":\"\"}";
        send_response(fd, 200, os.str());
        return;
    }
    if (method == "POST" && path == "/api/v1/imports/by-path/preview-updated") {
        size_t bpos = req.find("\r\n\r\n");
        std::string body = (bpos != std::string::npos) ? req.substr(bpos + 4) : "";
        std::string op = json_get(body, "output_path");
        if (op.empty()) { send_response(fd, 400, "{\"error\":\"missing-output_path\"}"); return; }
        auto r = store.GetImportByOutputPath(op);
        if (!r) { send_response(fd, 404, "{\"error\":\"not-found\"}"); return; }
        // Recompute the output hash from the (re-embedded) DNG on disk and sync.
        std::string new_hash = sha256_file(op);
        if (new_hash.empty()) { send_response(fd, 500, "{\"error\":\"hash-failed\"}"); return; }
        store.UpdateOutputHash(r->id, new_hash);
        store.RecordPreviewEdit(r->id, "api-preview-updated", r->output_hash, new_hash, 0, 0, 0);
        SPDLOG_INFO("[api] preview-updated for {} -> new output_hash synced", op);
        send_response(fd, 200, "{\"status\":\"synced\"}");
        return;
    }
    if (method == "POST" && path == "/api/v1/imports/by-source/rotation-updated") {
        if (!p->rotation_mgr) { send_response(fd, 503, "{\"error\":\"rotation-disabled\"}"); return; }
        size_t bpos = req.find("\r\n\r\n");
        std::string body = (bpos != std::string::npos) ? req.substr(bpos + 4) : "";
        std::string sp = json_get(body, "source_path");
        int orient = 1;
        std::string os = json_get(body, "orientation");
        if (!os.empty()) orient = std::stoi(os);
        std::string client = json_get(body, "client_id");
        auto r = store.GetImportBySourcePath(sp);
        if (!r) { send_response(fd, 404, "{\"error\":\"not-found\"}"); return; }
        p->rotation_mgr->Queue(r->id, r->output_path, orient, client);
        send_response(fd, 200, "{\"status\":\"queued\"}");
        return;
    }

    send_response(fd, 404, "{\"error\":\"no-route\"}");
}

} // namespace

void ApiServer::Run() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif
    int fd =
#ifdef _WIN32
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
        ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (fd < 0) { SPDLOG_ERROR("[api] socket failed"); return; }
    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.http_port));
#ifdef _WIN32
    if (bind(fd, (SOCKADDR*)&addr, sizeof(addr)) != 0) { closesocket(fd); return; }
    if (listen(fd, 16) != 0) { closesocket(fd); return; }
#else
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { ::close(fd); return; }
    if (::listen(fd, 16) != 0) { ::close(fd); return; }
#endif
    p_->listen_fd = fd;
    SPDLOG_INFO("[api] listening on :{}", p_->cfg->http_port);

    p_->accept_thread = std::thread([this, fd]() {
        while (!p_->stop.load()) {
#ifdef _WIN32
            int cfd = accept(fd, nullptr, nullptr);
            if (cfd == INVALID_SOCKET) { if (p_->stop.load()) break; continue; }
            handle(cfd, p_.get());
            closesocket(cfd);
#else
            int cfd = ::accept(fd, nullptr, nullptr);
            if (cfd < 0) { if (p_->stop.load()) break; continue; }
            handle(cfd, p_.get());
            ::close(cfd);
#endif
        }
    });
}

void ApiServer::Stop() {
    if (p_->stop.exchange(true)) return;
#ifdef _WIN32
    if (p_->listen_fd >= 0) { closesocket(p_->listen_fd); p_->listen_fd = -1; }
    if (p_->accept_thread.joinable()) p_->accept_thread.join();
    WSACleanup();
#else
    if (p_->listen_fd >= 0) { ::close(p_->listen_fd); p_->listen_fd = -1; }
    if (p_->accept_thread.joinable()) p_->accept_thread.join();
#endif
}

} // namespace rawimport