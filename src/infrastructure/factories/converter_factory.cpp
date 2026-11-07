/*
 * rawimport-pipeline - Converter Factory implementation
 */
#ifdef WITH_DNGLAB
#include "infrastructure/dnglab_converter_engine.hpp"
#endif

#ifdef WITH_ADOBE_DNG
#include "infrastructure/adobe_dng_converter_engine.hpp"
#endif

#include "domain/ports/log_sink.hpp"
#include "domain/ports/config_provider.hpp"
#include "infrastructure/factories/converter_factory.hpp"
#include "domain/ports/converter_engine.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace rawimport::infrastructure {

namespace {

/// Normalize engine name: lowercase, trim.
std::string normalize_engine_name(std::string name) {
    // Trim whitespace
    auto start = name.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = name.find_last_not_of(" \t\r\n");
    name = name.substr(start, end - start + 1);
    // Lowercase
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

/// Check if the requested engine name is an Adobe DNG variant.
bool is_adobe_engine(const std::string& name) {
    return name == "adobedng" || name == "adobe" || name == "adobe_dng";
}

/// Check if the requested engine name is a dnglab variant.
bool is_dnglab_engine(const std::string& name) {
    return name == "dnglab" || name == "dng_lab";
}

/// Log helper using the provided LogSink (or no-op if null).
void log_info(const std::shared_ptr<domain::LogSink>& log, const std::string& msg) {
    if (log) log->info(msg);
}
void log_warn(const std::shared_ptr<domain::LogSink>& log, const std::string& msg) {
    if (log) log->warn(msg);
}
void log_error(const std::shared_ptr<domain::LogSink>& log, const std::string& msg) {
    if (log) log->error(msg);
}
void log_debug(const std::shared_ptr<domain::LogSink>& log, const std::string& msg) {
    if (log) log->debug(msg);
}

} // anonymous namespace

std::unique_ptr<domain::ConverterEngine> MakeConverterEngine(
    domain::ConfigProvider& config,
    const std::shared_ptr<domain::LogSink>& log
) {
    // Read converter engine from config (with prefix handling)
    auto engine_opt = config.get_string("CONVERTER_ENGINE");
    std::string engine_name = engine_opt ? normalize_engine_name(*engine_opt) : "dnglab";

    log_debug(log, "Converter factory: requested engine = '" + engine_name + "'");

    // ─── Explicit Adobe DNG request ──────────────────────────────────────────
    if (is_adobe_engine(engine_name)) {
#ifdef WITH_ADOBE_DNG
        log_info(log, "Creating Adobe DNG Converter engine (explicit request)");
        auto engine = std::make_unique<AdobeDngConverterEngine>();
        const auto health = engine->health_check();
        if (health.status == domain::HealthStatus::Unhealthy) {
            const std::string err = "Adobe DNG Converter explicitly requested but health check failed: " + health.message;
            log_error(log, err);
            throw std::runtime_error(err);
        }
        if (health.status == domain::HealthStatus::Degraded) {
            log_warn(log, "Adobe DNG Converter health degraded: " + health.message);
        }
        log_info(log, "Adobe DNG Converter engine ready");
        return engine;
#else
        const std::string err = "Adobe DNG Converter requested (RAWIMPORT_CONVERTER_ENGINE=" + engine_name +
                                ") but WITH_ADOBE_DNG not enabled at compile time";
        log_error(log, err);
        throw std::runtime_error(err);
#endif
    }

    // ─── Default / explicit dnglab request ───────────────────────────────────
    if (is_dnglab_engine(engine_name) || engine_name.empty()) {
#ifdef WITH_DNGLAB
        log_info(log, "Creating dnglab engine (default or explicit request)");
        auto engine = std::make_unique<DnglabConverterEngine>();
        const auto health = engine->health_check();

        if (health.status == domain::HealthStatus::Healthy) {
            log_info(log, "dnglab engine ready");
            return engine;
        }

        // Degraded or Unhealthy: warn and try fallback
        log_warn(log, "dnglab health check " +
                      (health.status == domain::HealthStatus::Degraded ? "degraded" : "failed") +
                      ": " + health.message);

        // Check if adobedng is available as fallback
#ifdef WITH_ADOBE_DNG
        log_info(log, "Attempting Adobe DNG Converter as fallback");
        auto fallback_engine = std::make_unique<AdobeDngConverterEngine>();
        const auto fallback_health = fallback_engine->health_check();
        if (fallback_health.status != domain::HealthStatus::Unhealthy) {
            if (fallback_health.status == domain::HealthStatus::Degraded) {
                log_warn(log, "Fallback Adobe DNG Converter health degraded: " + fallback_health.message);
            }
            log_info(log, "Fallback Adobe DNG Converter engine ready");
            return fallback_engine;
        }
        log_error(log, "Fallback Adobe DNG Converter also unhealthy: " + fallback_health.message);
#else
        log_error(log, "No fallback available (WITH_ADOBE_DNG not enabled)");
#endif

        // No healthy engine found
        const std::string err = "dnglab engine unavailable and no healthy fallback configured";
        log_error(log, err);
        throw std::runtime_error(err);
#else
        // WITH_DNGLAB not enabled, try Adobe DNG if available
#ifdef WITH_ADOBE_DNG
        log_warn(log, "WITH_DNGLAB not enabled, attempting Adobe DNG Converter as primary");
        auto engine = std::make_unique<AdobeDngConverterEngine>();
        const auto health = engine->health_check();
        if (health.status != domain::HealthStatus::Unhealthy) {
            if (health.status == domain::HealthStatus::Degraded) {
                log_warn(log, "Adobe DNG Converter health degraded: " + health.message);
            }
            log_info(log, "Adobe DNG Converter engine ready (WITH_DNGLAB disabled)");
            return engine;
        }
        const std::string err = "No converter engine available: WITH_DNGLAB disabled and Adobe DNG Converter unhealthy";
        log_error(log, err);
        throw std::runtime_error(err);
#else
        const std::string err = "No converter engine available: neither WITH_DNGLAB nor WITH_ADOBE_DNG enabled";
        log_error(log, err);
        throw std::runtime_error(err);
#endif
#endif
    }

    // ─── Unknown engine name ─────────────────────────────────────────────────
    const std::string err = "Unknown converter engine: '" + engine_name +
                            "' (valid: dnglab, adobedng, adobe)";
    log_error(log, err);
    throw std::invalid_argument(err);
}

} // namespace rawimport::infrastructure