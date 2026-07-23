# LEAD Escalations — PRD v2.0.0 vs C++ Code Contradictions

**Generated:** 2026-07-17
**Author:** C++ rewrite dev (grill session)
**Status:** RESOLVED (Phase A–D completed 2026-07-17) — all BLOCKS-RUNTIME items fixed + verified via live e2e (docker-compose.cpp-e2e.yml + MariaDB 10.11)
**Source of truth:** `PRD_RawImport_Pipeline_CppRewrite.md` (v2.0.0) vs current `src/*.cpp` + `migrations/`.

---

## SCHEMA-DRIFT (runtime SQL break if migration file is authoritative)

### L4 — `processing_locks` key mismatch — **RESOLVED**
- **Conflict:** `db.cpp::Migrate()` inline DDL created `processing_locks(import_id PK)` while `0001_init.sql` defined `processing_locks(source_hash PK)`. Code worked only by accident of execution order (inline DDL ran first).
- **Fix:** Removed inline DDL from `db.cpp::Migrate()`; aligned `0001_init.sql` `processing_locks` to `import_id`-keyed form (matches `AcquireLock`/`ReleaseLock`). Single source of truth = migration.
- **Verified:** `DESCRIBE processing_locks` shows `import_id` PK in live DB.
- **Tag:** `SCHEMA-DRIFT`, `BLOCKS-RUNTIME` → **FIXED**

### L5 — `preview_edits` column names — **RESOLVED**
- **Conflict:** `RecordPreviewEdit` inserted `prev_hash/new_hash/width/height/created_at` but migration had `previous_output_hash/new_output_hash/preview_width/preview_height/edited_at`.
- **Fix:** Aligned `db.cpp::RecordPreviewEdit` to migration column names.
- **Verified:** `DESCRIBE preview_edits` shows correct columns; code matches.
- **Tag:** `SCHEMA-DRIFT`, `BLOCKS-RUNTIME` → **FIXED**

---

## LOGIC / FEATURE DEFECTS (compile fine, behavior wrong)

### L1 — Re-conversion uses row id as sequence — **RESOLVED**
- **Conflict:** `worker.cpp` called `GetImportBySequence("IMG_" + id)` where `id` is `imports.id`, not sequence number → lookup never matched → reconvert silently skipped.
- **Fix:** Added `Store::GetImportById(int64_t)` (db.h + db.cpp); `worker.cpp` now calls `store_.GetImportById(job.import_id)`.
- **Verified:** `POST /api/v1/imports/IMG_9001/reconvert` returns `reconversion_id:1` and writes a `reconversions` row (e2e test).
- **Tag:** `LOGIC-DEFECT` → **FIXED**

### L11 — Reconcile legacy hash parity drift — **RESOLVED**
- **Conflict:** C++ used `"legacy:" + sha256_file(abs)`; Go `reconcile.go` uses synthetic `"legacy:IMG_{n}"`.
- **Fix:** `reconcile.cpp` now uses `"legacy:IMG_" + std::to_string(n)` to match Go reference.
- **Tag:** `PARITY-DRIFT` → **FIXED**

### L12 — Thumbnail extract not implemented — **DEFERRED (FEATURE-GAP, non-runtime)**
- No code change. Out of scope for this pass (user directive: feature gaps deferred).
- **Tag:** `FEATURE-GAP` → **DEFERRED**

---

## NEW BUGS DISCOVERED DURING TRIAGE (not in original L-list)

### L14 — Ambiguous `id` column in `GetImportBySequence` JOIN — **RESOLVED**
- **Symptom:** `GET /api/v1/imports/IMG_9001` returned `not-found` despite row existing. `ListImports` worked.
- **Root cause:** `SELECT id, ... FROM imports i JOIN sequences s` → MySQL Error 1052 "Column 'id' in SELECT is ambiguous" (both tables have `id`). `mysql_query` failed → nullopt → 404.
- **Fix:** Qualified all columns as `i.id`, `i.source_path`, etc. in `GetImportBySequence`.
- **Verified:** `GET /api/v1/imports/IMG_9001` now returns the record.
- **Tag:** `BLOCKS-RUNTIME` → **FIXED**

### L15 — `reconversions` missing `reason` column — **RESOLVED**
- **Symptom:** `InsertReconversion` returned 0 (failure) → reconvert API returned `reconversion_id:0`.
- **Root cause:** Code INSERTs `reason` column but `0001_init.sql` `reconversions` table had no `reason` column → MySQL Error 1054 "Unknown column 'reason'".
- **Fix:** Added `reason TEXT` column to `reconversions` in `0001_init.sql`.
- **Verified:** reconvert writes row with `reason` column present.
- **Tag:** `BLOCKS-RUNTIME` → **FIXED**

### L16 — `InsertReconversion` inserted bare string into JSON column — **RESOLVED**
- **Symptom:** Even after L15, would fail: `conversion_settings` is `JSON NOT NULL` but code inserted `job.settings.compression` (bare string `"-c"`) → JSON validation error.
- **Fix:** `InsertReconversion` now serializes `ConversionSettings` to a valid JSON object string.
- **Verified:** `reconversions.conversion_settings` = `{"compression":"lossless",...}` (valid JSON) in live DB.
- **Tag:** `BLOCKS-RUNTIME` → **FIXED**

---

## DOC / LOW-SEVERITY

### L6 — `alerts` schema drift — **OK (doc only)**
- Code matches migration; PRD field `component` doesn't exist. Doc mismatch only.

### L3 — Rotation endpoint resolution — **OK (verified consistent)**

### L7 — Prometheus unimplemented — **DEFERRED (KNOWN-GAP)**

### L8 — Web UI deferred — **OK (consistent)**

### L9 — `build_folder_schema` — **OK (compiles, used in worker.cpp)**

### L10 — `reconcile.cpp` hash — **RESOLVED (see L11)**

### L13 — `adobedng` deferred — **OK (consistent with user directive)**

---

## MECHANICAL COMPILE ERRORS (fixed in-place, NOT escalated)
- `db.cpp`: add `#include <thread>` (uses `std::this_thread::sleep_for`).
- `worker.cpp`: add `#include <cctype>` (uses `tolower`).
- `CMakeLists.txt`: link target `mariadb` → `libmariadb::libmariadb` (C connector export name).

## SUMMARY
- **BLOCKS-RUNTIME fixed:** L1, L4, L5, L11, L14, L15, L16 (7 items)
- **DEFERRED (feature gaps, non-runtime):** L12, L7
- **OK / doc-only:** L3, L6, L8, L9, L10, L13
- **Verification:** docker build + MariaDB 10.11 e2e + reconvert round-trip (reconversion_id:1, valid JSON settings, reason column populated)
