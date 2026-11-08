/*
 * rawimport-pipeline - domain use cases implementation
 */
#include "domain/use_cases.hpp"

#include <algorithm>
#include <iostream>
#include <set>

namespace rawimport::use_cases {

// ─── Import ───────────────────────────────────────────────────────────────────
Import::Import(domain::ImportStore& store,
               domain::ConverterEngine& engine,
               domain::ExifExtractor& extractor,
               domain::FileSystem& fs,
               domain::HashService& hash_svc,
               domain::LogSink& log,
               domain::MetricsSink& metrics)
    : store_(store), engine_(engine), extractor_(extractor),
      fs_(fs), hash_svc_(hash_svc), log_(log), metrics_(metrics) {}

ImportOutcome Import::execute(const domain::FilePath& source,
                               const domain::FilePath& dest,
                               const domain::ConversionSettings& settings) {
    ImportOutcome outcome;

    // 1. Verify source exists.
    if (!fs_.exists(source)) {
        outcome.error = "Source file not found: " + source.native;
        log_.error(outcome.error);
        return outcome;
    }

    // 2. Check lock.
    if (store_.is_locked(-1)) { // simplified: no import_id yet
        // For new imports we skip lock check; locks are for reconversion safety.
    }

    // 3. Create import record.
    domain::ImportRecord rec;
    rec.source_path   = source;
    rec.output_path   = dest;
    rec.status        = domain::ImportStatus::Queued;
    rec.source_fingerprint = fs_.compute_fast_fingerprint(source);
    rec.created_at    = domain::now();

    auto id_opt = store_.create_import(rec);
    if (!id_opt) {
        outcome.error = "Failed to create import record";
        log_.error(outcome.error);
        return outcome;
    }
    rec.sequence_id = *id_opt;
    outcome.record = rec;

    // 4. Update status → Converting.
    rec.status = domain::ImportStatus::Converting;
    rec.started_at = domain::now();
    store_.update_import(rec);

    // 5. Extract EXIF.
    auto exif_opt = extractor_.extract(source);
    if (exif_opt) {
        log_.info("EXIF extracted: " + exif_opt->make + " " + exif_opt->model,
                  {{"source", source.native}});
    }

    // 6. Convert.
    auto result = engine_.convert(source, dest, settings);
    if (!result) {
        rec.status = domain::ImportStatus::Failed;
        rec.error_message = "Conversion failed";
        store_.update_import(rec);
        outcome.error = "Conversion failed";
        metrics_.increment_counter("imports_failed", 1, {{"engine", engine_name(settings.engine)}});
        log_.error(outcome.error, {{"source", source.native}});
        return outcome;
    }

    // 7. Success.
    rec.status     = domain::ImportStatus::Completed;
    rec.output_path = *result;
    rec.completed_at = domain::now();
    store_.update_import(rec);

    outcome.record       = rec;
    outcome.success      = true;
    outcome.error.clear();

    metrics_.increment_counter("imports_completed", 1,
                                {{"engine", engine_name(settings.engine)}});
    log_.info("Import completed: " + source.native + " -> " + result->native);
    return outcome;
}

// ─── Reconversion ─────────────────────────────────────────────────────────────
Reconversion::Reconversion(domain::ImportStore& store,
                            domain::ConverterEngine& engine,
                            domain::FileSystem& fs,
                            domain::LogSink& log)
    : store_(store), engine_(engine), fs_(fs), log_(log) {}

ReconversionOutcome Reconversion::execute(int64_t import_id,
                                           const domain::FilePath& source_dng,
                                           const domain::FilePath& output_dng,
                                           const domain::ConversionSettings& settings) {
    ReconversionOutcome outcome;

    // Acquire lock.
    auto lock_opt = store_.acquire_lock(import_id, "reconversion");
    if (!lock_opt) {
        outcome.error = "Could not acquire lock for import " + std::to_string(import_id);
        log_.error(outcome.error);
        return outcome;
    }

    // Fetch original import to get settings.
    auto imp = store_.get_import(import_id);
    if (!imp) {
        outcome.error = "Import record not found: " + std::to_string(import_id);
        store_.release_lock(*lock_opt);
        return outcome;
    }

    // Create reconversion record.
    domain::ReconversionRecord rec;
    rec.import_id          = import_id;
    rec.source_dng_path    = source_dng;
    rec.output_dng_path    = output_dng;
    rec.status             = domain::ReconversionStatus::Running;
    rec.scheduled_at       = domain::now();
    rec.started_at         = domain::now();

    auto rec_id = store_.create_reconversion(rec);
    if (!rec_id) {
        outcome.error = "Failed to create reconversion record";
        store_.release_lock(*lock_opt);
        return outcome;
    }
    rec.id = *rec_id;

    // Convert.
    auto result = engine_.convert(source_dng, output_dng, settings);
    if (!result) {
        rec.status = domain::ReconversionStatus::Failed;
        rec.error_message = "Conversion failed";
        store_.update_reconversion(rec);
        outcome.error = "Reconversion failed";
        store_.release_lock(*lock_opt);
        return outcome;
    }

    rec.status       = domain::ReconversionStatus::Done;
    rec.completed_at = domain::now();
    store_.update_reconversion(rec);
    store_.release_lock(*lock_opt);

    outcome.record = rec;
    outcome.success = true;
    return outcome;
}

// ─── Scan ─────────────────────────────────────────────────────────────────────
Scan::Scan(domain::ImportStore& store,
           domain::FileSystem& fs,
           domain::HashService& hash_svc,
           domain::LogSink& log)
    : store_(store), fs_(fs), hash_svc_(hash_svc), log_(log) {}

domain::ScanResult Scan::execute(const std::string& input_dir,
                                   const std::vector<std::string>& supported_extensions) {
    domain::ScanResult result;

    // List directory.
    auto files = fs_.list_directory(input_dir, "");
    std::set<std::string> seen_hashes;

    for (const auto& fp : files) {
        bool is_raw = false;
        for (const auto& ext : supported_extensions) {
            if (fp.extension() == ext) { is_raw = true; break; }
        }
        if (!is_raw) continue;

        auto hash = fs_.sha256_file(fp);
        if (!hash) continue;

        auto hhex = domain::sha256_hex(*hash);
        if (seen_hashes.count(hhex)) {
            result.unchanged_files.push_back(fp);
        } else {
            seen_hashes.insert(hhex);
            result.new_files.push_back(fp);
        }
    }

    log_.info("Scan complete: " + std::to_string(result.new_files.size()) +
              " new, " + std::to_string(result.unchanged_files.size()) + " unchanged");
    return result;
}

// ─── Rotation ─────────────────────────────────────────────────────────────────
Rotation::Rotation(domain::ImportStore& store,
                    domain::RotationSink& sink,
                    domain::LogSink& log)
    : store_(store), sink_(sink), log_(log) {}

RotationOutcome Rotation::execute(int64_t import_id, int orientation) {
    RotationOutcome outcome;
    outcome.import_id = import_id;

    auto imp = store_.get_import(import_id);
    if (!imp) {
        outcome.error = "Import not found: " + std::to_string(import_id);
        log_.error(outcome.error);
        return outcome;
    }

    bool pushed = sink_.push_orientation(import_id, orientation);
    if (!pushed) {
        outcome.error = "Failed to push orientation to rotation sink";
        log_.error(outcome.error);
        return outcome;
    }

    outcome.applied_orientation = orientation;
    outcome.success = true;
    log_.info("Rotation applied: import=" + std::to_string(import_id) +
              " orientation=" + std::to_string(orientation));
    return outcome;
}

// ─── Stats ────────────────────────────────────────────────────────────────────
Stats::Stats(domain::ImportStore& store) : store_(store) {}

StatsSnapshot Stats::compute() {
    StatsSnapshot snap;
    auto all = store_.list_imports();
    snap.total_imports = static_cast<int>(all.size());
    for (const auto& r : all) {
        switch (r.status) {
            case domain::ImportStatus::Completed: snap.completed_imports++; break;
            case domain::ImportStatus::Failed:    snap.failed_imports++;    break;
            default: break;
        }
    }
    snap.active_locks = static_cast<int>(store_.list_active_locks().size());
    snap.alerts_count = static_cast<int>(store_.list_alerts().size());
    return snap;
}

// ─── Config validation ────────────────────────────────────────────────────────
ConfigValidate::ConfigValidate(domain::ConfigProvider& cfg) : cfg_(cfg) {}

domain::ValidationReport ConfigValidate::execute() {
    auto report = cfg_.validate(domain::required_config_keys());
    return report;
}

} // namespace rawimport::use_cases
