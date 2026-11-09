/*
 * rawimport-pipeline - Environment ConfigProvider adapter implementation
 */
#include "infrastructure/env_config_provider.hpp"

#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace rawimport::infrastructure {

// ─── EnvConfigProvider ────────────────────────────────────────────────────────
// Resolve a logical key to either prefixed (RAWIMPORT_*) or unprefixed (legacy).
// Supports aliases: INPUT_DIR <-> WATCH_DIR.
static std::string resolve_config_key(const std::string& prefix, const std::string& key) {
    // First check for prefixed key (new format)
    std::string prefixed = prefix + key;
    if (std::getenv(prefixed.c_str())) return prefixed;
    // Fall back to unprefixed key (legacy format, e.g., OUTPUT_DIR, WATCH_DIR)
    if (std::getenv(key.c_str())) return key;
    // Alias: INPUT_DIR -> WATCH_DIR (legacy docker-compose uses WATCH_DIR)
    if (key == "INPUT_DIR") {
        if (std::getenv("WATCH_DIR")) return "WATCH_DIR";
    }
    return prefixed; // return prefixed for consistent error messages
}

EnvConfigProvider::EnvConfigProvider(std::string prefix) : prefix_(std::move(prefix)) {}

std::string EnvConfigProvider::resolve_key(const std::string& key) const {
    return resolve_config_key(prefix_, key);
}

std::optional<std::string> EnvConfigProvider::get_string(const std::string& key) {
    const char* v = std::getenv(resolve_key(key).c_str());
    if (!v) return std::nullopt;
    return std::string(v);
}

std::optional<int> EnvConfigProvider::get_int(const std::string& key) {
    auto s = get_string(key);
    if (!s) return std::nullopt;
    try { return std::stoi(*s); } catch (...) { return std::nullopt; }
}

std::optional<bool> EnvConfigProvider::get_bool(const std::string& key) {
    auto s = get_string(key);
    if (!s) return std::nullopt;
    std::string lower;
    std::transform(s->begin(), s->end(), std::back_inserter(lower),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") return false;
    return std::nullopt;
}

std::optional<double> EnvConfigProvider::get_double(const std::string& key) {
    auto s = get_string(key);
    if (!s) return std::nullopt;
    try { return std::stod(*s); } catch (...) { return std::nullopt; }
}

std::map<std::string, std::string> EnvConfigProvider::to_map() {
    // Scan common keys and return their values.
    std::map<std::string, std::string> out;
    // Return everything starting with prefix_ for introspection.
    // getenv doesn't enumerate, so we expose known keys.
    const std::vector<std::string> known = {
        "OUTPUT_DIR", "INPUT_DIR", "CONVERTER_ENGINE",
        "DB_CONNECTION", "DATABASE_URL", "DB_DRIVER",
        "LOG_LEVEL", "MAX_WORKERS",
        "IMMICH_BASE_URL", "IMMICH_API_KEY",
        "EXIFTOOL_PATH", "DNGLAB_PATH", "ADOBE_DNG_PATH",
    };
    for (const auto& k : known) {
        auto v = get_string(k);
        if (v) out[k] = *v;
    }
    return out;
}

domain::ValidationReport EnvConfigProvider::validate(const std::vector<domain::RequiredConfigKey>& required) {
    domain::ValidationReport report;
    for (const auto& rk : required) {
        auto v = get_string(rk.name);
        if (!v || v->empty()) {
            report.missing_keys.push_back(rk.name);
        }
    }
    report.valid = report.missing_keys.empty();
    return report;
}

} // namespace rawimport::infrastructure
