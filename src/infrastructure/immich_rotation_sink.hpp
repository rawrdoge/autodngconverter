/*
 * rawimport-pipeline - Immich RotationSink adapter
 */
#pragma once

#include <string>
#include <optional>

#include "domain/ports/rotation_sink.hpp"

namespace rawimport::infrastructure {

class ImmichRotationSink final : public domain::RotationSink {
public:
    // Base URL ends WITHOUT trailing slash, e.g. "https://immich.example.com".
    explicit ImmichRotationSink(std::string base_url, std::string api_key);

    HealthReport  health_check() override;
    bool          push_orientation(int64_t asset_id, int orientation) override;

private:
    std::string base_url_;
    std::string api_key_;
    std::string build_url(const std::string& path) const;
    std::string http_put(const std::string& url, const std::string& body) const;
    std::string http_get(const std::string& url) const;
};

} // namespace rawimport::infrastructure
