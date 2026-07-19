#pragma once
// reconcile.h — nomenclature-aware library reconciliation for the C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §3.3.
#include <string>
#include "config.h"
#include "db.h"

namespace rawimport {

// ReconcileLibrary scans /output and /archive for existing IMG_{n}.dng files,
// registers 'legacy' placeholders in the DB (synthetic legacy:IMG_{n} source hash),
// and reserves the sequence counter past the max on-disk n so new imports never
// collide. Call once after Migrate(), before worker.Start().
bool ReconcileLibrary(const Config& cfg, Store& store);

} // namespace rawimport