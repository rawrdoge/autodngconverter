/*
 * rawimport-pipeline - MetricsSink port interface
 */
#pragma once

#include <string>

namespace rawimport::domain {

class MetricsSink {
public:
    virtual ~MetricsSink() = default;

    // Record a counter increment with optional labels.
    virtual void increment_counter(const std::string& name,
                                    int delta = 1,
                                    const std::map<std::string, std::string>& labels = {}) = 0;

    // Record an observed value in a histogram / timer.
    virtual void observe_histogram(const std::string& name,
                                    double value,
                                    const std::map<std::string, std::string>& labels = {}) = 0;

    // Render current metrics in Prometheus exposition format.
    virtual std::string render_prometheus() = 0;
};

} // namespace rawimport::domain
