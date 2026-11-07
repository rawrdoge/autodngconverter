/*
 * rawimport-pipeline - domain use cases
 */
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "ports/import_store.hpp"
#include "ports/converter_engine.hpp"
#include "ports/exif_extractor.hpp"
#include "ports/file_system.hpp"
#include "ports/hash_service.hpp"
#include "ports/config_provider.hpp"
#include "ports/metrics_sink.hpp"
#include "ports/log_sink.hpp"
#include "ports/rotation_sink.hpp"

#include "domain/types.hpp"

namespace rawimport::use_cases {

// ─── Import ───────────────────────────────────────────────────────────────────
struct ImportOutcome {
    domain::ImportRecord record;
    bool                  success = false;
    std::string           error;
};

class Import {
public:
    Import(domain::ImportStore& store,
           domain::ConverterEngine& engine,
           domain::ExifExtractor& extractor,
           domain::FileSystem& fs,
           domain::HashService& hash_svc,
           domain::LogSink& log,
           domain::MetricsSink& metrics);

    ImportOutcome execute(const domain::FilePath& source,
                          const domain::FilePath& dest,
                          const domain::ConversionSettings& settings);

private:
    domain::ImportStore&   store_;
    domain::ConverterEngine& engine_;
    domain::ExifExtractor&  extractor_;
    domain::FileSystem&     fs_;
    domain::HashService&    hash_svc_;
    domain::LogSink&        log_;
    domain::MetricsSink&    metrics_;
};

// ─── Reconversion ─────────────────────────────────────────────────────────────
struct ReconversionOutcome {
    domain::ReconversionRecord record;
    bool                       success = false;
    std::string                error;
};

class Reconversion {
public:
    Reconversion(domain::ImportStore& store,
                 domain::ConverterEngine& engine,
                 domain::FileSystem& fs,
                 domain::LogSink& log);

    ReconversionOutcome execute(int64_t import_id,
                                 const domain::FilePath& source_dng,
                                 const domain::FilePath& output_dng,
                                 const domain::ConversionSettings& settings);
private:
    domain::ImportStore&   store_;
    domain::ConverterEngine& engine_;
    domain::FileSystem&     fs_;
    domain::LogSink&        log_;
};

// ─── Scan ─────────────────────────────────────────────────────────────────────
class Scan {
public:
    Scan(domain::ImportStore& store,
         domain::FileSystem& fs,
         domain::HashService& hash_svc,
         domain::LogSink& log);

    domain::ScanResult execute(const std::string& input_dir,
                                const std::vector<std::string>& supported_extensions);
private:
    domain::ImportStore&  store_;
    domain::FileSystem&   fs_;
    domain::HashService&  hash_svc_;
    domain::LogSink&      log_;
};

// ─── Rotation ─────────────────────────────────────────────────────────────────
struct RotationOutcome {
    int64_t       import_id;
    int           applied_orientation;
    bool          success = false;
    std::string   error;
};

class Rotation {
public:
    Rotation(domain::ImportStore& store,
             domain::RotationSink& sink,
             domain::LogSink& log);

    RotationOutcome execute(int64_t import_id, int orientation);
private:
    domain::ImportStore&  store_;
    domain::RotationSink& sink_;
    domain::LogSink&      log_;
};

// ─── Stats ────────────────────────────────────────────────────────────────────
struct StatsSnapshot {
    int total_imports        = 0;
    int completed_imports    = 0;
    int failed_imports       = 0;
    int pending_reconversions = 0;
    int active_locks         = 0;
    int alerts_count         = 0;
};

class Stats {
public:
    Stats(domain::ImportStore& store);
    StatsSnapshot compute();
private:
    domain::ImportStore& store_;
};

// ─── Config validation ────────────────────────────────────────────────────────
class ConfigValidate {
public:
    ConfigValidate(domain::ConfigProvider& cfg);
    domain::ValidationReport execute();
private:
    domain::ConfigProvider& cfg_;
};

} // namespace rawimport::use_cases
