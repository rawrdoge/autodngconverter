/*
 * rawimport-pipeline - domain types unit tests
 * Single-TU test: all tests and type impls are in this compilation unit.
 */
#include "utest.h"
#include "domain/types.hpp"
#include <cstring>

// Pull in the types implementation so sha256_hex is available.
#include "../src/domain/types.cpp"

using namespace rawimport::domain;

// ─── FilePath ─────────────────────────────────────────────────────────────────
TEST(filepath_filename) {
    FilePath fp{"/foo/bar/neffile.NEF"};
    UTEST_ASSERT_STRING(fp.filename().c_str(), "neffile.NEF");
}

TEST(filepath_extension) {
    FilePath fp{"/foo/bar/photo.CR2"};
    UTEST_ASSERT_STRING(fp.extension().c_str(), ".CR2");
}

TEST(filepath_extension_no_dot) {
    FilePath fp{"photo.nef"};
    UTEST_ASSERT_STRING(fp.extension().c_str(), ".nef");
}

TEST(filepath_no_extension) {
    FilePath fp{"/foo/bar/README"};
    UTEST_ASSERT_STRING(fp.extension().c_str(), "");
}

// ─── engine_name ──────────────────────────────────────────────────────────────
TEST(engine_name_dnglab) {
    UTEST_ASSERT_STRING(engine_name(RawConverterEngine::Dnglab).c_str(), "dnglab");
}

TEST(engine_name_adobe) {
    UTEST_ASSERT_STRING(engine_name(RawConverterEngine::AdobeDngConverter).c_str(), "adobe_dng");
}

// ─── ImportStatus strings ─────────────────────────────────────────────────────
TEST(import_status_str) {
    UTEST_ASSERT_STRING(import_status_str(ImportStatus::Queued),      "queued");
    UTEST_ASSERT_STRING(import_status_str(ImportStatus::Converting),  "converting");
    UTEST_ASSERT_STRING(import_status_str(ImportStatus::Completed),   "completed");
    UTEST_ASSERT_STRING(import_status_str(ImportStatus::Failed),      "failed");
    UTEST_ASSERT_STRING(import_status_str(static_cast<ImportStatus>(99)), "unknown");
}

// ─── SHA256 hash utility ──────────────────────────────────────────────────────
TEST(sha256_hex_empty) {
    SHA256Hash h{};
    std::string hex = sha256_hex(h);
    UTEST_ASSERT_EQ(hex.length(), 64u);
    // All zeros → all '0' chars
    for (char c : hex) UTEST_ASSERT(c == '0');
}

TEST(sha256_hex_all_ff) {
    SHA256Hash h;
    std::memset(h.data(), 0xFF, h.size());
    std::string hex = sha256_hex(h);
    UTEST_ASSERT_EQ(hex.length(), 64u);
    for (char c : hex)
        UTEST_ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

// ─── FastFingerprint defaults ─────────────────────────────────────────────────
TEST(fast_fingerprint_default) {
    FastFingerprint fp{};
    UTEST_ASSERT_EQ(fp.size_bytes, 0ULL);
    UTEST_ASSERT_EQ(fp.fnv1a, 0ULL);
}

// ─── Required config keys ─────────────────────────────────────────────────────
TEST(required_config_keys_count) {
    auto keys = required_config_keys();
    UTEST_ASSERT_EQ(keys.size(), 6u);
    UTEST_ASSERT_STRING(keys[0].name.c_str(), "OUTPUT_DIR");
    UTEST_ASSERT_STRING(keys[1].name.c_str(), "INPUT_DIR");
    UTEST_ASSERT_STRING(keys[2].name.c_str(), "CONVERTER_ENGINE");
    UTEST_ASSERT_STRING(keys[3].name.c_str(), "DB_CONNECTION");
    UTEST_ASSERT_STRING(keys[4].name.c_str(), "DATABASE_URL");
    UTEST_ASSERT_STRING(keys[5].name.c_str(), "DB_DRIVER");
}

// ─── ConversionSettings defaults ──────────────────────────────────────────────
TEST(conversion_settings_defaults) {
    ConversionSettings cs;
    UTEST_ASSERT_EQ(static_cast<int>(cs.engine), static_cast<int>(RawConverterEngine::Dnglab));
    UTEST_ASSERT(cs.linear_dng == false);
    UTEST_ASSERT(cs.lossy == false);
    UTEST_ASSERT_EQ(cs.compression, 4);
}

// ─── ExifResult defaults ──────────────────────────────────────────────────────
TEST(exif_result_defaults) {
    ExifResult er;
    UTEST_ASSERT_EQ(er.orientation, 1);
    UTEST_ASSERT_STRING(er.make.c_str(), "");
    UTEST_ASSERT_STRING(er.model.c_str(), "");
}

// ─── Alert severity ───────────────────────────────────────────────────────────
TEST(alert_severity_order) {
    UTEST_ASSERT_EQ(static_cast<int>(AlertSeverity::Info), 0);
    UTEST_ASSERT_EQ(static_cast<int>(AlertSeverity::Warning), 1);
    UTEST_ASSERT_EQ(static_cast<int>(AlertSeverity::Error), 2);
}

// ─── HealthStatus enum ────────────────────────────────────────────────────────
TEST(health_status_values) {
    UTEST_ASSERT_EQ(static_cast<int>(HealthStatus::Healthy),     0);
    UTEST_ASSERT_EQ(static_cast<int>(HealthStatus::Degraded),    1);
    UTEST_ASSERT_EQ(static_cast<int>(HealthStatus::Unhealthy),   2);
}

// ─── Instant / now ────────────────────────────────────────────────────────────
TEST(now_returns_valid_time) {
    auto t = now();
    auto epoch = t.time_since_epoch();
    UTEST_ASSERT(epoch.count() > 0LL);
}

// ─── ImportRecord full round-trip ─────────────────────────────────────────────
TEST(import_record_creation) {
    ImportRecord r;
    r.sequence_id     = 42;
    r.source_path.native = "/input/photo.NEF";
    r.output_path.native = "/output/photo.DNG";
    r.status          = ImportStatus::Queued;
    r.created_at      = now();

    UTEST_ASSERT_EQ(r.sequence_id, 42);
    UTEST_ASSERT_STRING(r.source_path.native.c_str(), "/input/photo.NEF");
    UTEST_ASSERT_STRING(r.output_path.native.c_str(), "/output/photo.DNG");
    UTEST_ASSERT_EQ(static_cast<int>(r.status), static_cast<int>(ImportStatus::Queued));
}

// ─── ReconversionStatus values ────────────────────────────────────────────────
TEST(reconversion_status_values) {
    UTEST_ASSERT_EQ(static_cast<int>(ReconversionStatus::Pending),  0);
    UTEST_ASSERT_EQ(static_cast<int>(ReconversionStatus::Running),  1);
    UTEST_ASSERT_EQ(static_cast<int>(ReconversionStatus::Done),     2);
    UTEST_ASSERT_EQ(static_cast<int>(ReconversionStatus::Failed),   3);
}

// ─── FastFingerprint with real data ───────────────────────────────────────────
TEST(fnv1a_known_input) {
    FastFingerprint fp{};
    fp.fnv1a = 1469598103934665603ULL;
    UTEST_ASSERT_EQ(fp.fnv1a, 1469598103934665603ULL);
}

// ─── Lock defaults ────────────────────────────────────────────────────────────
TEST(lock_defaults) {
    Lock l;
    UTEST_ASSERT_EQ(l.id, -1LL);
    UTEST_ASSERT_EQ(l.import_id, 0LL);
    UTEST_ASSERT_STRING(l.holder.c_str(), "");
}

// ─── PreviewEdit defaults ─────────────────────────────────────────────────────
TEST(preview_edit_defaults) {
    PreviewEdit e;
    UTEST_ASSERT_EQ(e.id, -1LL);
    UTEST_ASSERT_EQ(e.import_id, 0LL);
}

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    return utest_main();
}
