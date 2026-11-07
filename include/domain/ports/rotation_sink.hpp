/*
 * rawimport-pipeline - RotationSink port interface
 */
#pragma once

#include <string>
#include <optional>

namespace rawimport::domain {

class RotationSink {
public:
    virtual ~RotationSink() = default;

    virtual HealthReport health_check() = 0;

    // Push orientation metadata back to the source system.
    // Returns true on success.
    virtual bool push_orientation(int64_t asset_id, int orientation) = 0;
};

} // namespace rawimport::domain
