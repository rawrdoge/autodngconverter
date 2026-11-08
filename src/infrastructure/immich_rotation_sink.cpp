/*
 * rawimport-pipeline - Immich RotationSink adapter implementation
 */
#include "infrastructure/immich_rotation_sink.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iomanip>

#ifdef WITH_IMMICH

#include <curl/curl.h>

namespace rawimport::infrastructure {

namespace {

struct CurlResponse {
    std::string body;
    int status_code{0};
};

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* resp = static_cast<CurlResponse*>(userp);
    resp->body.append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

CurlResponse http_request(const std::string& method, const std::string& url,
                           const std::string& body, const std::string& api_key) {
    CurlResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) return resp;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!api_key.empty()) {
        std::string auth_header = "x-api-key: " + api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    if (!body.empty()) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

} // anon

ImmichRotationSink::ImmichRotationSink(std::string base_url, std::string api_key)
    : base_url_(std::move(base_url)), api_key_(std::move(api_key)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ImmichRotationSink::~ImmichRotationSink() {
    curl_global_cleanup();
}

std::string ImmichRotationSink::build_url(const std::string& path) const {
    return base_url_ + (base_url_.back() == '/' ? "" : "/") + path;
}

std::string ImmichRotationSink::http_put(const std::string& url, const std::string& body) const {
    return http_request("PUT", url, body, api_key_).body;
}

std::string ImmichRotationSink::http_get(const std::string& url) const {
    return http_request("GET", url, "", api_key_).body;
}

domain::HealthReport ImmichRotationSink::health_check() {
    try {
        auto resp = http_get(build_url("api/server/ping"));
        if (resp == "pong")
            return {domain::HealthStatus::Healthy, "Immich ping OK", domain::now()};
        return {domain::HealthStatus::Unhealthy, "Immich ping failed: " + resp, domain::now()};
    } catch (...) {
        return {domain::HealthStatus::Unhealthy, "Immich unreachable", domain::now()};
    }
}

bool ImmichRotationSink::push_orientation(int64_t asset_id, int orientation) {
    try {
        std::ostringstream body;
        body << "{\"orientation\":" << orientation << "}";
        auto resp = http_put(build_url("api/assets/" + std::to_string(asset_id)), body.str());
        return !resp.empty();
    } catch (...) {
        return false;
    }
}

} // namespace rawimport::infrastructure

#else // WITH_IMMICH not defined

namespace rawimport::infrastructure {

ImmichRotationSink::ImmichRotationSink(std::string, std::string) {}
ImmichRotationSink::~ImmichRotationSink() {}

domain::HealthReport ImmichRotationSink::health_check() {
    return {domain::HealthStatus::Unhealthy, "WITH_IMMICH not enabled", domain::now()};
}
bool ImmichRotationSink::push_orientation(int64_t, int) { return false; }

} // namespace rawimport::infrastructure

#endif // WITH_IMMICH
