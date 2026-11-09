/*
 * rawimport-pipeline - In-memory Prometheus MetricsSink adapter implementation
 */
#include "infrastructure/prometheus_metrics_sink.hpp"

#include <sstream>
#include <algorithm>
#include <iomanip>

namespace rawimport::infrastructure {

PrometheusMetricsSink::PrometheusMetricsSink() = default;

static std::string prometheus_escape(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 4);
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

static std::string key_escape(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 4);
    for (char c : v) {
        if (c == '\\' || c == ',' || c == '=') out += '\\';
        out += c;
    }
    return out;
}

static std::string make_key_str(const std::map<std::string, std::string>& labels) {
    std::string s;
    for (const auto& [k, v] : labels) {
        if (!s.empty()) s += ",";
        s += k + "=" + key_escape(v);
    }
    return s;
}

CounterEntry& PrometheusMetricsSink::get_counter(
    const std::string& name, const std::map<std::string, std::string>& labels) {
    const std::string key = name + "{" + make_key_str(labels) + "}";
    auto it = counters_.find(key);
    if (it != counters_.end()) return *it->second;
    auto e = std::make_unique<CounterEntry>();
    e->labels = labels;
    e->name = name;
    auto ptr = e.get();
    counters_[key] = std::move(e);
    return *ptr;
}

HistogramEntry& PrometheusMetricsSink::get_histogram(
    const std::string& name, const std::map<std::string, std::string>& labels) {
    const std::string key = name + "{" + make_key_str(labels) + "}";
    auto it = histograms_.find(key);
    if (it != histograms_.end()) return *it->second;
    auto e = std::make_unique<HistogramEntry>();
    e->labels = labels;
    e->name = name;
    auto ptr = e.get();
    histograms_[key] = std::move(e);
    return *ptr;
}

std::string PrometheusMetricsSink::make_label_str(const std::map<std::string, std::string>& labels) const {
    std::string s;
    for (const auto& [k, v] : labels) {
        if (!s.empty()) s += ",";
        s += k + "=\"" + prometheus_escape(v) + "\"";
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
        oss << "# HELP " << counter->name << " Counter\n";
        oss << "# TYPE " << counter->name << " counter\n";
        if (!label_str.empty())
            oss << counter->name << "{" << label_str << "} "
                << counter->value.load(std::memory_order_relaxed) << "\n";
        else
            oss << counter->name << " " << counter->value.load(std::memory_order_relaxed) << "\n";
    }

    for (const auto& [key, hist_ptr] : histograms_) {
        const std::string label_str = make_label_str(hist_ptr->labels);
        const std::string base_name = hist_ptr->name;
        oss << "# HELP " << base_name << "_sum Histogram sum\n";
        oss << "# TYPE " << base_name << "_sum summary\n";
        if (!label_str.empty())
            oss << base_name << "_sum{" << label_str << "} "
                << std::fixed << std::setprecision(6) << hist_ptr->sum << "\n";
        else
            oss << base_name << "_sum " << std::fixed << std::setprecision(6) << hist_ptr->sum << "\n";
        if (!label_str.empty())
            oss << base_name << "_count{" << label_str << "} " << hist_ptr->count << "\n";
        else
            oss << base_name << "_count " << hist_ptr->count << "\n";
    }

    return oss.str();
}

} // namespace rawimport::infrastructure