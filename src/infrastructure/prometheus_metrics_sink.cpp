/*
 * rawimport-pipeline - In-memory Prometheus MetricsSink adapter implementation
 */
#include "infrastructure/prometheus_metrics_sink.hpp"

#include <sstream>
#include <algorithm>
#include <iomanip>

namespace rawimport::infrastructure {

PrometheusMetricsSink::PrometheusMetricsSink() = default;

CounterEntry& PrometheusMetricsSink::get_counter(
    const std::string& name, const std::map<std::string, std::string>& labels) {
    const std::string key = name + make_label_str(labels);
    auto it = counters_.find(key);
    if (it != counters_.end()) return *it->second;
    auto e = std::make_unique<CounterEntry>();
    e->labels = labels;
    auto ptr = e.get();
    counters_[key] = std::move(e);
    return *ptr;
}

HistogramEntry& PrometheusMetricsSink::get_histogram(
    const std::string& name, const std::map<std::string, std::string>& labels) {
    const std::string key = name + make_label_str(labels);
    auto it = histograms_.find(key);
    if (it != histograms_.end()) return *it->second;
    auto e = std::make_unique<HistogramEntry>();
    e->labels = labels;
    auto ptr = e.get();
    histograms_[key] = std::move(e);
    return *ptr;
}

std::string PrometheusMetricsSink::make_label_str(const std::map<std::string, std::string>& labels) const {
    std::string s;
    for (const auto& [k, v] : labels) {
        s += k + "=" + v + ",";
    }
    return s;
}

void PrometheusMetricsSink::increment_counter(const std::string& name, int delta,
                                               const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mtx_);
    get_counter(name, labels).value.fetch_add(delta, std::memory_order_relaxed);
}

void PrometheusMetricsSink::observe_histogram(const std::string& name, double value,
                                               const std::map<std::string, std::string>& labels) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& h = get_histogram(name, labels);
    h.sum += value;
    h.count++;
}

std::string PrometheusMetricsSink::render_prometheus() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ostringstream oss;

    for (const auto& [key, counter] : counters_) {
        const std::string label_str = make_label_str(counter->labels);
        oss << "# HELP " << key << " Counter\n";
        oss << "# TYPE " << key << " counter\n";
        if (!label_str.empty())
            oss << key << "{" << label_str.substr(0, label_str.size()-1) << "} "
                << counter->value.load(std::memory_order_relaxed) << "\n";
        else
            oss << key << " " << counter->value.load(std::memory_order_relaxed) << "\n";
    }

    for (const auto& [key, hist_ptr] : histograms_) {
        const std::string label_str = make_label_str(hist_ptr->labels);
        oss << "# HELP " << key << "_sum Histogram sum\n";
        oss << "# TYPE " << key << "_sum summary\n";
        oss << key << "_sum" << (!label_str.empty() ? "{" + label_str.substr(0, label_str.size()-1) + "}" : "")
            << " " << std::fixed << std::setprecision(6) << hist_ptr->sum << "\n";
        oss << key << "_count" << (!label_str.empty() ? "{" + label_str.substr(0, label_str.size()-1) + "}" : "")
            << " " << hist_ptr->count << "\n";
    }

    return oss.str();
}

} // namespace rawimport::infrastructure
