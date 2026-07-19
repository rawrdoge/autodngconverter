// metrics.cpp — see metrics.h. Prometheus text exposition format.
#include "metrics.h"

#include <algorithm>
#include <sstream>

namespace rawimport {

Metrics& Metrics::instance() {
    static Metrics m;
    return m;
}

void Metrics::inc_files_detected(int64_t n) {
    std::lock_guard<std::mutex> lk(mtx_);
    files_detected_total_ += n;
}

void Metrics::inc_conversions_completed(const std::string& status, int64_t n) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (status == "failed") conversions_completed_total_failed_ += n;
    else conversions_completed_total_completed_ += n;
}

void Metrics::observe_conversion_duration(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    static const double edges[] = {1,2,5,10,20,30,60,120,300};
    int b = 0;
    while (b < 9 && seconds > edges[b]) ++b;
    dur_bucket_[b] += 1;
    dur_sum_ += seconds;
    dur_count_ += 1;
}

void Metrics::set_queue_depth(int64_t depth) {
    std::lock_guard<std::mutex> lk(mtx_);
    queue_depth_ = depth;
}

void Metrics::set_db_size_bytes(int64_t bytes) {
    std::lock_guard<std::mutex> lk(mtx_);
    db_size_bytes_ = bytes;
}

std::string Metrics::Render() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::ostringstream os;
    os << "# TYPE rawimport_files_detected_total counter\n";
    os << "rawimport_files_detected_total " << files_detected_total_ << "\n";
    os << "# TYPE rawimport_conversions_completed_total counter\n";
    os << "rawimport_conversions_completed_total{status=\"completed\"} "
       << conversions_completed_total_completed_ << "\n";
    os << "rawimport_conversions_completed_total{status=\"failed\"} "
       << conversions_completed_total_failed_ << "\n";
    os << "# TYPE rawimport_conversion_duration_seconds histogram\n";
    static const char* le[] = {"1","2","5","10","20","30","60","120","300","+Inf"};
    int64_t cum = 0;
    for (int i = 0; i < 10; ++i) {
        cum += dur_bucket_[i];
        os << "rawimport_conversion_duration_seconds_bucket{le=\"" << le[i] << "\"} " << cum << "\n";
    }
    os << "rawimport_conversion_duration_seconds_sum " << dur_sum_ << "\n";
    os << "rawimport_conversion_duration_seconds_count " << dur_count_ << "\n";
    os << "# TYPE rawimport_queue_depth gauge\n";
    os << "rawimport_queue_depth " << queue_depth_ << "\n";
    os << "# TYPE rawimport_db_size_bytes gauge\n";
    os << "rawimport_db_size_bytes " << db_size_bytes_ << "\n";
    return os.str();
}

} // namespace rawimport