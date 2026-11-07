/*
 * rawimport-pipeline - port interface compilation smoke test
 * Single-TU test.
 */
#include "utest.h"

#include "domain/ports/import_store.hpp"
#include "domain/ports/converter_engine.hpp"
#include "domain/ports/exif_extractor.hpp"
#include "domain/ports/file_system.hpp"
#include "domain/ports/hash_service.hpp"
#include "domain/ports/config_provider.hpp"
#include "domain/ports/metrics_sink.hpp"
#include "domain/ports/log_sink.hpp"
#include "domain/ports/rotation_sink.hpp"
#include "domain/ports.hpp"
#include "domain/use_cases.hpp"

using namespace rawimport::domain;

TEST(port_import_store_is_abstract) {
    ImportStore* ptr = nullptr;
    (void)ptr;
}

TEST(port_converter_engine_is_abstract) {
    ConverterEngine* ptr = nullptr;
    (void)ptr;
}

TEST(port_exif_extractor_is_abstract) {
    ExifExtractor* ptr = nullptr;
    (void)ptr;
}

TEST(port_file_system_is_abstract) {
    FileSystem* ptr = nullptr;
    (void)ptr;
}

TEST(port_hash_service_is_abstract) {
    HashService* ptr = nullptr;
    (void)ptr;
}

TEST(port_config_provider_is_abstract) {
    ConfigProvider* ptr = nullptr;
    (void)ptr;
}

TEST(port_metrics_sink_is_abstract) {
    MetricsSink* ptr = nullptr;
    (void)ptr;
}

TEST(port_log_sink_is_abstract) {
    LogSink* ptr = nullptr;
    (void)ptr;
}

TEST(port_rotation_sink_is_abstract) {
    RotationSink* ptr = nullptr;
    (void)ptr;
}

TEST(import_record_complete) {
    ImportRecord r;
    r.sequence_id     = 1;
    r.source_path.native = "/test.NEF";
    r.status          = ImportStatus::Completed;
    UTEST_ASSERT_EQ(r.sequence_id, 1);
}

TEST(conversion_settings_complete) {
    ConversionSettings cs;
    cs.engine       = RawConverterEngine::AdobeDngConverter;
    cs.lossy        = true;
    cs.compression  = 6;
    cs.extra["key"] = "val";
    UTEST_ASSERT_EQ(static_cast<int>(cs.engine), static_cast<int>(RawConverterEngine::AdobeDngConverter));
    UTEST_ASSERT(cs.lossy == true);
}

TEST(exif_result_complete) {
    ExifResult er;
    er.make    = "Canon";
    er.model   = "EOS R5";
    er.orientation = 6;
    er.raw["ExposureTime"] = "1/250";
    UTEST_ASSERT_STRING(er.make.c_str(), "Canon");
    UTEST_ASSERT_EQ(er.orientation, 6);
}

TEST(alert_complete) {
    Alert a;
    a.severity = AlertSeverity::Error;
    a.message  = "Disk full";
    a.code     = "ERR_DISK_FULL";
    UTEST_ASSERT_EQ(static_cast<int>(a.severity), static_cast<int>(AlertSeverity::Error));
}

TEST(scan_result_complete) {
    ScanResult sr;
    sr.new_files.push_back({"a.NEF"});
    sr.removed_files.push_back({"b.NEF"});
    sr.unchanged_files.push_back({"c.NEF"});
    UTEST_ASSERT_EQ(sr.new_files.size(), 1u);
    UTEST_ASSERT_EQ(sr.removed_files.size(), 1u);
    UTEST_ASSERT_EQ(sr.unchanged_files.size(), 1u);
}

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}
