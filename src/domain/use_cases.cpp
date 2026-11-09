/*
 * rawimport-pipeline - domain use cases implementation
 */
#include "domain/use_cases.hpp"

#include <algorithm>
#include <cerrno>
#include <iostream>
#include <map>
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
        log_.LogFilesystemReadError("", source.native, ENOENT, "exists");
        log_.error(outcome.error);
        return outcome;
    }

    // 2. Compute SHA256 hash for duplicate detection.
    int hash_errno = 0;
    auto source_hash_opt = fs_.sha256_file(source, &hash_errno);
    if (!source_hash_opt) {
        outcome.error = "Failed to compute source file hash";
        log_.LogFilesystemReadError("", source.native, hash_errno ?: EIO, "sha256_file");
        log_.error(outcome.error);
        return outcome;
    }
    domain::SHA256Hash source_hash = *source_hash_opt;

    // 3. Check for existing import with same hash (duplicate detection).
    auto existing = store_.find_import_by_source_hash(source_hash);
    domain::ImportRecord rec;
    bool reuse_existing = false;
    if (existing) {
        // If completed, treat as duplicate and return existing record
        if (existing->status == domain::ImportStatus::Completed) {
            outcome.error = "Duplicate import detected: file already imported (import_id=" +
                            std::to_string(existing->sequence_id) + ")";
            log_.LogCollisionDuplicateFound(
                "",  // no sequence ID allocated for duplicate (not imported)
                std::to_string(existing->sequence_id),
                source_hash,
                source.native);
            log_.warn(outcome.error, {{"source", source.native}, {"existing_import_id", std::to_string(existing->sequence_id)}});
            outcome.record = *existing;
            return outcome;  // Not a failure, but duplicate detected
        }
        // If failed, allow retry by reusing the existing record
        if (existing->status == domain::ImportStatus::Failed) {
            log_.info("Retrying previously failed import", {{"source", source.native}, {"import_id", std::to_string(existing->sequence_id)}});
            // Update the existing record with new destination and reset status
            existing->output_path = dest;
            existing->status = domain::ImportStatus::Queued;
            existing->error_message.clear();
            existing->started_at = domain::Instant{};
            existing->completed_at = domain::Instant{};
            if (!store_.update_import(*existing)) {
                outcome.error = "Failed to update import record for retry";
                log_.error(outcome.error);
                return outcome;
            }
            rec = *existing;
            reuse_existing = true;
            // Continue to conversion (fall through)
        }
        // If queued or converting, skip this file (another process may be handling it)
        if (existing->status == domain::ImportStatus::Queued || existing->status == domain::ImportStatus::Converting) {
            outcome.error = "Import already in progress or queued (import_id=" +
                            std::to_string(existing->sequence_id) + ")";
            log_.warn(outcome.error, {{"source", source.native}, {"existing_import_id", std::to_string(existing->sequence_id)}, {"status", domain::import_status_str(existing->status)}});
            outcome.record = *existing;
            return outcome;
        }
    }

    // 4. Check lock.
    if (store_.is_locked(-1)) { // simplified: no import_id yet
        // For new imports we skip lock check; locks are for reconversion safety.
    }

    // 5. Create import record (only if not reusing existing failed record).
    if (!reuse_existing) {
        rec.source_path   = source;
        // If dest.native ends with '/' or is empty, treat as directory for auto-naming
        bool auto_name = dest.native.empty() || dest.native.back() == '/' || dest.native.back() == '\\';
        if (auto_name) {
            // Placeholder - will be updated after sequence ID is allocated
            rec.output_path = domain::FilePath{dest.native + "placeholder.dng"};
        } else {
            rec.output_path = dest;
        }
        rec.status        = domain::ImportStatus::Queued;
        rec.source_hash   = source_hash;
        rec.source_fingerprint = fs_.compute_fast_fingerprint(source);
        rec.created_at    = domain::now();

        auto id_opt = store_.create_import(rec);
        if (!id_opt) {
            outcome.error = "Failed to create import record";
            log_.LogDbQueryFailed("", "create_import returned null");
            log_.error(outcome.error);
            return outcome;
        }
        rec.sequence_id = *id_opt;

        // If auto-naming, update output path with sequence ID
        if (auto_name) {
            std::string dir = dest.native;
            if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
                dir.pop_back();
            }
            rec.output_path = domain::FilePath{dir + "/IMG_" + std::to_string(rec.sequence_id) + ".dng"};
            if (!store_.update_import(rec)) {
                outcome.error = "Failed to update import record with output path";
                log_.LogDbQueryFailed(std::to_string(rec.sequence_id), "update_import returned false");
                log_.error(outcome.error);
                return outcome;
            }
        }
    }
    outcome.record = rec;

    // 6. Update status → Converting.
    rec.status = domain::ImportStatus::Converting;
    rec.started_at = domain::now();
    store_.update_import(rec);

    // 7. Extract EXIF.
    auto exif_opt = extractor_.extract(source);
    if (!exif_opt) {
        if (extractor_.is_available()) {
            log_.LogExifExtractionFailed(std::to_string(rec.sequence_id), source.native, "extraction returned empty", false);
        } else {
            log_.debug("EXIF extraction skipped (extractor not available)", {{"source", source.native}});
        }
    } else {
        log_.info("EXIF extracted: " + exif_opt->make + " " + exif_opt->model,
                  {{"source", source.native}});
    }

    // 8. Convert.
    // Use the resolved output path (auto-generated IMG_{id}.dng if dest was a directory)
    const domain::FilePath& output_path = rec.output_path;
    auto result = engine_.convert(source, output_path, settings);
    if (!result) {
        rec.status = domain::ImportStatus::Failed;
        rec.error_message = "Conversion failed";
        store_.update_import(rec);
        outcome.error = "Conversion failed";
        metrics_.increment_counter("imports_failed", 1, {{"engine", engine_.name()}});
        log_.LogImportFailed(std::to_string(rec.sequence_id), outcome.error);
        log_.LogConversionFailedEngineError(std::to_string(rec.sequence_id), engine_.name(), -1, "engine returned null result");
        log_.error(outcome.error, {{"source", source.native}});
        return outcome;
    }

    // 9. Success.
    rec.status     = domain::ImportStatus::Completed;
    rec.output_path = *result;
    rec.completed_at = domain::now();
    store_.update_import(rec);

    outcome.record       = rec;
    outcome.success      = true;
    outcome.error.clear();

    metrics_.increment_counter("imports_completed", 1,
                                {{"engine", engine_.name()}});
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
        log_.LogReconversionLockFailed(std::to_string(import_id), "unknown", 0);
        log_.error(outcome.error);
        return outcome;
    }

    // Fetch original import to get settings.
    auto imp = store_.get_import(import_id);
    if (!imp) {
        outcome.error = "Import record not found: " + std::to_string(import_id);
        log_.LogDbQueryFailed(std::to_string(import_id), "get_import returned empty (record not found or DB error)");
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
        log_.LogDbQueryFailed(std::to_string(import_id), "create_reconversion returned null");
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
        log_.LogImportFailed(std::to_string(import_id), outcome.error);
        log_.LogConversionFailedEngineError(std::to_string(import_id), engine_.name(), -1, "engine returned null result");
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

    // Normalize supported extensions to lowercase for case-insensitive comparison.
    std::set<std::string> normalized_exts;
    for (const auto& ext : supported_extensions) {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        normalized_exts.insert(lower);
    }

    for (const auto& fp : files) {
        std::string ext = fp.extension();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (!normalized_exts.count(ext)) continue;

        int hash_errno = 0;
        auto hash = fs_.sha256_file(fp, &hash_errno);
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
        log_.LogRotationPushFailed(std::to_string(import_id), "immich", outcome.error);
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
    std::map<std::string, std::string> cfg;
    const std::vector<std::string> keys = {"OUTPUT_DIR", "INPUT_DIR", "CONVERTER_ENGINE",
                                           "DB_CONNECTION", "DATABASE_URL", "DB_DRIVER"};
    for (const auto& k : keys) {
        auto v = cfg_.get_string(k);
        if (v) cfg["RAWIMPORT_" + k] = *v;
    }
    return domain::validate_config(cfg);
}

} // namespace rawimport::use_cases
