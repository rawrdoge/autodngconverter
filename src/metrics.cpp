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

void Metrics::observe_fast_fingerprint_duration(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    static const double edges[] = {0.001,0.002,0.005,0.01,0.02,0.05,0.1,0.2,0.5};
    int b = 0;
    while (b < 9 && seconds > edges[b]) ++b;
    fp_bucket_[b] += 1;
    fp_sum_ += seconds;
    fp_count_ += 1;
}

void Metrics::observe_hash_duration(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    static const double edges[] = {0.01,0.02,0.05,0.1,0.2,0.5,1,2,5};
    int b = 0;
    while (b < 9 && seconds > edges[b]) ++b;
    hash_bucket_[b] += 1;
    hash_sum_ += seconds;
    hash_count_ += 1;
}

void Metrics::observe_exif_duration(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    static const double edges[] = {0.01,0.02,0.05,0.1,0.2,0.5,1,2,5};
    int b = 0;
    while (b < 9 && seconds > edges[b]) ++b;
    exif_bucket_[b] += 1;
    exif_sum_ += seconds;
    exif_count_ += 1;
}

void Metrics::observe_db_duration(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    static const double edges[] = {0.001,0.002,0.005,0.01,0.02,0.05,0.1,0.2,0.5};
    int b = 0;
    while (b < 9 && seconds > edges[b]) ++b;
    db_bucket_[b] += 1;
    db_sum_ += seconds;
    db_count_ += 1;
}

void Metrics::observe_archive_duration(double seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    static const double edges[] = {0.01,0.02,0.05,0.1,0.2,0.5,1,2,5};
    int b = 0;
    while (b < 9 && seconds > edges[b]) ++b;
    arch_bucket_[b] += 1;
    arch_sum_ += seconds;
    arch_count_ += 1;
}

static void render_histogram(std::ostringstream& os, const char* name,
                             const int64_t* buckets, double sum, int64_t count) {
    os << "# TYPE " << name << " histogram\n";
    static const char* le[] = {"1","2","5","10","20","30","60","120","300","+Inf"};
    int64_t cum = 0;
    for (int i = 0; i < 10; ++i) {
        cum += buckets[i];
        os << name << "_bucket{le=\"" << le[i] << "\"} " << cum << "\n";
    }
    os << name << "_sum " << sum << "\n";
    os << name << "_count " << count << "\n";
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
    
    // Per-stage histograms (bucket edges are baked into render_histogram)
    render_histogram(os, "rawimport_fast_fingerprint_duration_seconds", fp_bucket_, fp_sum_, fp_count_);
    render_histogram(os, "rawimport_hash_duration_seconds", hash_bucket_, hash_sum_, hash_count_);
    render_histogram(os, "rawimport_exif_duration_seconds", exif_bucket_, exif_sum_, exif_count_);
    render_histogram(os, "rawimport_db_duration_seconds", db_bucket_, db_sum_, db_count_);
    render_histogram(os, "rawimport_archive_duration_seconds", arch_bucket_, arch_sum_, arch_count_);
    
    return os.str();
}

} // namespace rawimport
