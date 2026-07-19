// metrics.h — lightweight Prometheus-format metrics for the RawImport C++ rewrite.
// PRD §3.6 requires 5 metrics exposed in Prometheus text exposition format.
// Implemented without an external dependency to keep the build self-contained;
// the exposition format is compatible with prometheus-cpp scrapers.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

namespace rawimport {

class Metrics {
public:
    // Counters (monotonic).
    void inc_files_detected(int64_t n = 1);
    void inc_conversions_completed(const std::string& status, int64_t n = 1);
    void observe_conversion_duration(double seconds);
    void set_queue_depth(int64_t depth);
    void set_db_size_bytes(int64_t bytes);

    // Render in Prometheus text exposition format.
    std::string Render() const;

    static Metrics& instance();

private:
    mutable std::mutex mtx_;
    int64_t files_detected_total_ = 0;
    int64_t conversions_completed_total_completed_ = 0;
    int64_t conversions_completed_total_failed_ = 0;
    // histogram buckets for conversion duration (seconds)
    int64_t dur_bucket_[10] = {0,0,0,0,0,0,0,0,0,0}; // <=1,2,5,10,20,30,60,120,300,+Inf
    double dur_sum_ = 0;
    int64_t dur_count_ = 0;
    int64_t queue_depth_ = 0;
    int64_t db_size_bytes_ = 0;
};

} // namespace rawimport