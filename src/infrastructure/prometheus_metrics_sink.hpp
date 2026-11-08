/*
 * rawimport-pipeline - In-memory Prometheus MetricsSink adapter
 */
#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

#include "domain/ports/metrics_sink.hpp"

namespace rawimport::infrastructure {

// Internal counter entry (thread-safe).
struct CounterEntry {
    std::atomic<int64_t> value{0};
    std::map<std::string, std::string> labels;
    std::string name;
};

// Internal histogram bucket entry.
struct HistogramEntry {
    double             sum{0.0};
    int64_t            count{0};
    std::map<std::string, std::string> labels;
    std::string name;
};

class PrometheusMetricsSink final : public domain::MetricsSink {
public:
    PrometheusMetricsSink();

    void increment_counter(const std::string& name,
                             int delta,
                             const std::map<std::string, std::string>& labels) override;
    void observe_histogram(const std::string& name,
                             double value,
                             const std::map<std::string, std::string>& labels) override;
    std::string render_prometheus() override;

private:
    mutable std::mutex          mtx_;
    std::map<std::string, std::unique_ptr<CounterEntry>> counters_;
    std::map<std::string, std::unique_ptr<HistogramEntry>> histograms_;

    CounterEntry& get_counter(const std::string& name,
                               const std::map<std::string, std::string>& labels);
    HistogramEntry& get_histogram(const std::string& name,
                                   const std::map<std::string, std::string>& labels);
    std::string make_label_str(const std::map<std::string, std::string>& labels) const;
};

} // namespace rawimport::infrastructure
