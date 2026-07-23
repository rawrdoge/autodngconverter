# Product Requirements Document (PRD)
# RawImport Pipeline — C++20 Rewrite Specification
# Version: 2.0.0 (C++ Target)
# Date: 2026-07-16
# Status: Draft / Implementation Spec
# Based on: PRD_RawImport_Pipeline_Unified.md (v1.0.0) + Go repo @ 6f9c705 + memory-bank/*

---

## 1. Executive Summary

RawImport Pipeline is a Dockerized microservice that watches a designated import directory for new camera RAW files (NRW, NEF, CR2, ARW), converts them to DNG, assigns a **global monotonic sequence identifier** (`IMG_{n}`, no leading zeros), computes SHA-256 hashes for both source and output, and persists atomic records to MariaDB. Designed for NAS environments where multiple clients (macOS, Windows, Linux) drop files into a watched folder and consume the resulting DNGs in Immich and digiKam.

This document is the **authoritative specification for a from-scratch C++20 rewrite** that replaces the current Go single-binary (`rawimport-pipeline`) + Rust submodule (`vibelabdng`/`dnglab`) architecture. It consolidates:
- The original `PRD_RawImport_Pipeline_Unified.md` (v1.0.0, draft).
- The verified, CI-green behavior of the Go implementation (conversion pipeline, preview re-embed, reconciliation, REST API).
- All deferred/stubbed features and the formerly-deferred **Cross-App Rotation Sync** feature, which is **promoted to in-scope** for the C++ rewrite.

The C++ rewrite's goal is behavior parity with the Go service plus the rotation-sync feature, while eliminating the Go/Rust split in favor of a single language where practical (keeping `dnglab` as an external subprocess or promoting `dng_previewembed.cpp` to a first-class component).

---

## 2. Source of Truth & Lineage

| Source | Role |
|--------|------|
| `PRD_RawImport_Pipeline_Unified.md` (v1.0.0) | Original theory-craft PRD (draft) |
| Go repo `https://github.com/rawrdoge/autodngconverter.git` @ `6f9c705` | Current working implementation (single Echo binary + `vibelabdng` submodule) |
| `memory-bank/projectbrief.md` | Purpose, components, core requirements, key decisions |
| `memory-bank/productContext.md` | Why-it-exists, problems solved, UX goals, deferred rotation goal |
| `memory-bank/techContext.md` | Tech stack, constraints, dependencies, CI patterns |
| `memory-bank/systemPatterns.md` | Architecture, converter dispatcher, re-embed flow, API surface |
| `memory-bank/activeContext.md` | Recent changes, rotation-sync design sketch, deferred features |
| `memory-bank/progress.md` | What works, what's left, known issues, evolution of decisions |

**Branch discipline (carried forward):** the remote default branch is `main`. All commits push directly to `main`.

---

## 3. Working Features to Port (verified, CI-green in Go impl)

These are proven-correct and MUST be reproduced in C++ with behavior parity.

### 3.1 Conversion Pipeline (PRD §5.1 happy path)
Sequence (single serial worker, bounded in-memory queue default 100):
1. Poller walks `/watch` every ~10s (range 5–15s), debounces 2s after last write, skips `.part`/`.tmp`/`.download`.
2. Compute SHA-256(source) before conversion.
3. Duplicate check: if `source_hash` exists in DB → skip (log duplicate).
4. `BEGIN` transaction → `AllocateSequence()` → `IMG_{n}` (BIGINT, gaps acceptable, never reused).
5. EXIF `DateTimeOriginal` → folder `YYYY/MM` (`date_source` = exif|mtime fallback).
6. Convert via default engine (`dnglab convert`):
   - **Authoritative CLI (do NOT add flags that abort):** `dnglab convert --input <SRC> --output <DST> -c <lossless|uncompressed> --keep-mtime <true|false> -f`
   - Preview size (1024×1024 medium / 4000×3000 full), DNG 1.4, JPEG q92 are dnglab built-in defaults. Passing `--preview-medium/--preview-full/--dng-version/--jpeg-quality/--compress/--linear/--seed` on `convert` ABORTS the conversion (clap panic).
7. Extract embedded JPEG thumbnail (SubIFD1 medium primary; SubIFD2 full fallback) → sidecar `IMG_{n}.thumb.jpg` (gated by `GEN_THUMB_JPEG`, default false).
8. Compute SHA-256(output).
9. `INSERT` into `imports` (all fields) → `COMMIT`.
10. Move source → `/archive/YYYY/MM/`, DNG → `/output/YYYY/MM/`.

### 3.2 Preview Re-embed (the major win — `betterembeds.lua` + worker chain)
- `betterembeds.lua` (Darktable Lua API 9.x, companion tool, NOT in container) re-embeds the user's edited export JPEG into the matching DNG on `post-export-image`, gated by `auto_reeembed_on_export` (default off).
- Library-aware branching: `detect_library_mode` (raw|dng) + `resolve_target_dng` (raw → API `GET /api/v1/imports/by-source`; dng → DNG is target directly).
- Worker preference chain: **dnglab first** (`dnglab reembed`), then DNG SDK (`dng_previewembed.cpp`), then exiftool. Fixed `--seed` for determinism/idempotency.
- `POST /api/v1/imports/by-path/preview-updated` notifies the service so `output_hash` stays in sync.
- Lua callbacks MUST be `pcall`-wrapped; `api_version` guard (REQUIRED_API 9.0.0); `register_lib` signature fallback.

### 3.3 Nomenclature-Aware Reconciliation (`reconcile.go`)
On startup: scan `/output` + `/archive` for existing `IMG_{n}.dng`, register `legacy` placeholders (synthetic `legacy:IMG_{n}` source hash), reserve sequence past max on-disk `n` so new imports never collide.

### 3.4 MariaDB Store (`db.go`)
- DSN via `DB_DRIVER=mariadb` + `DB_HOST/PORT/USER/PASSWORD/NAME`.
- Embedded migration runner (`splitSQL` per-statement, no DELIMITER/proc support) tracked in `schema_migrations`.
- `AllocateSequence` = `AUTO_INCREMENT` + `LAST_INSERT_ID()`.
- Idempotency: `source_hash` dedup + `processing_locks` table (TTL) to catch in-flight/crashed jobs.
- Tables: `sequences`, `imports`, `reconversions`, `processing_locks`, `alerts`, `preview_edits`, `schema_migrations`.

### 3.5 REST API (`api.go`)
`GET /health`, `GET /api/v1/imports` (list+pagination+filters), `GET /api/v1/imports/:seq`, `GET /api/v1/imports/hash/:sha`, `POST /api/v1/imports/:seq/reconvert`, `GET /api/v1/stats`, `GET /api/v1/alerts`, `GET /api/v1/imports/by-source?path=`, `POST /api/v1/imports/by-path/preview-updated`.

### 3.6 Observability
JSON structured logs; Prometheus metrics (`rawimport_files_detected_total`, `rawimport_conversions_completed_total{status}`, `rawimport_conversion_duration_seconds`, `rawimport_queue_depth`, `rawimport_db_size_bytes`); persistent `alerts` table; optional ntfy/Gotify push via `ALERT_PUSH_URL` (off by default).

### 3.7 Docker / CI
Multi-stage build. Go base was `golang:1.26-bookworm`; C++ base = `debian:bookworm-slim` + CMake. Dual-registry publish: GHCR always + Docker Hub gated on `DOCKERHUB_TOKEN` secret (NOT yet enabled). Non-root `appuser` (UID 10001); `/db` container-only.

---

## 4. Deferred / Stubbed Features (must be addressed in C++ design)

| Feature | Go status | C++ requirement |
|---------|-----------|-----------------|
| **Web UI** | Not built (API-only) | Decide: embedded tiny SPA vs API-only. Default API-only in v1. |
| `libraw` engine | Stub | Defer or implement C++ libraw decode → DNG muxer. |
| `adobedng` engine | Opt-in (Wine+Adobe, EULA-isolated) | Keep as opt-in external module; base image Adobe-free. |
| Postgres/SQLite drivers | Deferred to v1.1 | Keep MariaDB-only in v1; abstraction allows later add. |
| `dng_previewembed.cpp` medium preview | Stub (reuses same JPEG, `fPreviewBytes=0`) | Promote to full multi-res pyramid OR rely on dnglab default; document. |
| EXIF `CameraModel` empty in test | exiftool path issue | Configurable `EXIFTOOL_BIN`; probe at startup. |
| Docker Hub dual-publish | Secrets not set | No code change; gate on secret. |

---

## 5. Cross-App Rotation Sync (PROMOTED TO IN-SCOPE)

### 5.1 Goal
A mechanism that reads rotation from any of Darktable/digiKam/Immich and reflects it natively in all apps, so each shows the same rotation without per-app rework. Feasibility confirmed: all three read rotation from the **EXIF `Orientation` tag (1–8)** in the file.

### 5.2 Critical Correctness Decision — `ROTATION_MODE=metadata`
Darktable's flip module **bakes** rotation into rendered pixels on export. To avoid **double-rotation** (pixels rotated + tag applied by viewer), the C++ service operates in:
- `ROTATION_MODE=metadata` (DEFAULT): write real EXIF `Orientation` 1–8, leave pixels unrotated. All apps show correct rotation via tag. This is the only safe mode for live sync.

`baked` mode (write `Orientation=1`, pixels already correct) is export-only and NOT used for live sync.

### 5.3 Live Trigger — Custom Bindable Darktable Rotate+Sync
Darktable's Lua API has **no built-in "on-rotate" / "image-modified" event** in the lighttable. The solution is a **custom companion-plugin trigger** that rotates + syncs atomically:
- `dt.register_event("shortcut", cb, "Rotate selected + sync to all apps")` — bindable hotkey. On fire: for each `img` in `dt.gui.action_images`, cycle `img.orientation` (standard EXIF cycle `1→6→3→8→1`, `2→5→4→7→2`), then `POST` the new orientation to the API.
- `dt.register_lib(...)` sidebar button "↻ Sync Rotation" running the same loop (discoverable + bindable).
- `pcall`-wrapped; never throws into Darktable.

### 5.4 Spam Protection — Server-Side Queue + Grace Coalescing
To prevent collisions with in-flight workers when a power user spams the hotkey:
- **Client:** update `image.orientation` locally immediately (instant UI); `POST` an **intent** on every press (`{source_path, orientation, client_id}`). No client-side grace needed.
- **Server (`RotationManager`):** per-image pending registry keyed by import ID, with a **grace timer** `ROTATION_GRACE_MS` (default **2000**). Every incoming intent overwrites the target orientation and **resets the timer**. When the timer fires (no new intent within grace), dispatch **exactly one coalesced job**.
- **Serialization:** the job acquires a `processing_locks`-equivalent lock (keyed by import ID, TTL) so it never runs concurrently with a re-embed/reconvert on the same DNG.
- **Job:** write EXIF `Orientation` to DNG (exiftool `-Orientation=N -n`, dnglab `reembed` fallback) → `UPDATE imports SET orientation=?` → fire Immich refresh-metadata + digiKam rescan → release lock.

### 5.5 Cross-App Propagation (inside dispatch)
- **Immich:** `POST {IMMICH_URL}/api/asset/refresh-metadata` with service token; on failure → `alerts` row.
- **digiKam:** `utimes`/mtime bump on the DNG (watch-folder rescan). `dbus` mode = Linux-only stub behind config.

### 5.6 C++ Implementation Breakdown (maps to Go A–H)
- **A. DB:** `ALTER TABLE imports ADD COLUMN orientation TINYINT UNSIGNED NULL;` + `UpdateOrientation(id, o)` + reuse lock helpers.
- **B. RotationManager:** `std::mutex` + `std::unordered_map<int64, PendingRot>`; `PendingRot{timer: std::chrono, orientation}`; `Queue()` resets timer; `dispatch()` runs on timer fire.
- **C. API:** `POST /api/v1/imports/by-source/rotation-updated` → resolve via by-source lookup → `202 Accepted`.
- **D. Config:** `ROTATION_GRACE_MS=2000`, `ROTATION_MODE=metadata`, `IMMICH_URL`, `IMMICH_TOKEN`, `DIGIKAM_RESCAN=touch|dbus`.
- **E. Lua:** `rotate_cw()` + bindable shortcut + lib button (port from `betterembeds.lua`).
- **F. Propagation:** Immich HTTP client + digiKam mtime bump.
- **G. Docs:** README + .env.example.
- **H. Verify:** spam test (20 rapid POSTs → 1 DNG rewrite, final orientation, no lock overlap).

---

## 6. Target C++ Architecture

### 6.1 Build & Toolchain
- **C++20**, CMake ≥ 3.25, `debian:bookworm-slim` runtime base.
- Keep `vibelabdng` (`dnglab`) as an **external subprocess** (Rust) invoked via `popen`/`std::process` — lowest risk, preserves the verified conversion/re-embed behavior. (Alternative: vendor a native C++ DNG muxer — out of scope for v1; `dng_previewembed.cpp` remains the DNG-SDK fallback.)
- Linters: `clang-tidy`, `clang-format`.

### 6.2 Library Map (replaces Go deps)
| Concern | Go (current) | C++20 proposal |
|---------|--------------|----------------|
| HTTP server | Echo | `drogon` or `cpp-httplib` (embeddable, header-only option) |
| MariaDB | `go-sql-driver/mysql` | `libmariadb` + `soci` or `mysql-connector-cpp` |
| JSON | `encoding/json` | `nlohmann/json` |
| SHA-256 | `crypto/sha256` | OpenSSL `EVP_sha256` or `Boost.Compute` |
| EXIF | exiftool (subprocess) | exiftool subprocess + `libexif` probe |
| Watch | fsnotify/poll | `std::filesystem` poll loop + `inotify` (local bind-mount only) |
| Concurrency | goroutines | `std::thread` + `std::mutex` + `std::condition_variable` + `std::chrono` timers |
| CLI arg parse | clap (Rust) | `cxxopts` / `CLI11` (for the service binary) |
| Logging | structured JSON | `spdlog` (JSON formatter) |
| Metrics | Prometheus | `prometheus-cpp` |

### 6.3 Component Modules (proposed layout)
```
src/
  main.cpp            // bootstrap: config, engine init, db open, migrate, reconcile, worker, api, graceful shutdown
  config.cpp/.h       // env-driven Config (port DEF_* + rotation/immich/digikam vars)
  db.cpp/.h           // MariaDB store: DSN, ping-retry, migration runner, AllocateSequence, CRUD, locks
  pipeline.cpp/.h     // ImportRecord, ReconversionJob, SHA-256, EXIF, folder schema, thumbnail extract
  converter.cpp/.h    // ConverterEngine interface + dnglab impl (default), adobedng/libraw stubs
  worker.cpp/.h       // poller (debounce, skip .part), serial converter, atomic insert, archive, reconvert drain
  rotation.cpp/.h     // RotationManager: pending registry, grace timer, coalesced dispatch, lock
  api.cpp/.h          // HTTP routes (port all + add rotation-updated)
  reconcile.cpp/.h    // nomenclature-aware legacy scan
  util.cpp/.h         // splitSQL, dotenv, helpers
tools/
  betterembeds.lua    // moved from repo root; rotate_cw + bindable shortcut + lib button
  dng_previewembed.cpp// promoted DNG SDK fallback embedder (fix medium-preview stub)
migrations/
  0001_init.sql       // ported MariaDB schema
  0002_orientation.sql// ADD COLUMN orientation
Dockerfile            // debian:bookworm-slim + CMake + dnglab binary + exiftool
docker-compose.yml
```

### 6.4 Concurrency Model
- **Single serial converter worker** (Wine+Adobe and dnglab are not concurrency-safe). Bounded in-memory queue (default 100); `queue_full` alert.
- **RotationManager** runs on its own timer thread; coalesced jobs serialized via the shared lock with the converter worker.
- Graceful shutdown: `std::atomic<bool>` stop flag; join worker + pending rotation timers.

---

## 7. Data Model (MariaDB)

Port the existing schema; add `orientation`. Idempotent `ADD COLUMN IF NOT EXISTS` for upgrade.

```sql
-- 0001_init.sql (port from Go, condensed)
CREATE TABLE IF NOT EXISTS sequences (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32) NOT NULL UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS imports (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    sequence_id BIGINT UNSIGNED NOT NULL,
    source_path TEXT NOT NULL,
    source_hash CHAR(64) NOT NULL,
    output_path TEXT NOT NULL,
    output_hash CHAR(64) NOT NULL,
    camera_model VARCHAR(64),
    capture_date DATE,
    capture_time TIME,
    folder_schema VARCHAR(16),
    conversion_settings JSON,
    status ENUM('pending','converting','completed','failed','restored','legacy') DEFAULT 'pending',
    date_source ENUM('exif','mtime') DEFAULT 'exif',
    orientation TINYINT UNSIGNED NULL,           -- ADDED (rotation sync)
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP NULL DEFAULT NULL,
    error_message TEXT,
    FOREIGN KEY (sequence_id) REFERENCES sequences(id) ON DELETE RESTRICT,
    INDEX idx_source_hash (source_hash),
    INDEX idx_output_hash (output_hash),
    INDEX idx_sequence_id (sequence_id),
    INDEX idx_capture_date (capture_date),
    INDEX idx_status_created (status, created_at),
    INDEX idx_camera_date (camera_model, capture_date)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS reconversions (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    import_id BIGINT UNSIGNED NOT NULL,
    previous_output_hash CHAR(64) NOT NULL,
    new_output_hash CHAR(64),
    conversion_settings JSON NOT NULL,
    triggered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP NULL DEFAULT NULL,
    status ENUM('pending','running','completed','failed') DEFAULT 'pending',
    error_message TEXT,
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE,
    INDEX idx_import_id (import_id),
    INDEX idx_status (status)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS processing_locks (
    import_id BIGINT UNSIGNED PRIMARY KEY,
    token VARCHAR(64) NOT NULL,
    expires_at TIMESTAMP NOT NULL,
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS alerts (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    level VARCHAR(16) NOT NULL,
    component VARCHAR(32),
    message TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_created (created_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS preview_edits (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    import_id BIGINT UNSIGNED NOT NULL,
    worker VARCHAR(32),
    width INT, height INT, quality TINYINT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version VARCHAR(64) PRIMARY KEY,
    applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;
```

```sql
-- 0002_orientation.sql (upgrade-safe; covered by IF NOT EXISTS above, kept explicit for clarity)
ALTER TABLE imports ADD COLUMN IF NOT EXISTS orientation TINYINT UNSIGNED NULL
  COMMENT 'EXIF Orientation 1-8 agreed across apps; NULL = unset';
```

---

## 8. API Surface

Port all Go endpoints; add rotation endpoint.

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | Liveness/readiness |
| GET | `/api/v1/imports` | List (pagination, `status`, `camera` filters) |
| GET | `/api/v1/imports/:seq` | Get by `IMG_{n}` |
| GET | `/api/v1/imports/hash/:sha` | Find by source/output hash |
| POST | `/api/v1/imports/:seq/reconvert` | Trigger re-conversion (archives prev DNG) |
| GET | `/api/v1/stats` | Counts, failure rates, queue depth |
| GET | `/api/v1/alerts` | Persistent alerts |
| GET | `/api/v1/imports/by-source?path=` | Resolve source RAW → DNG output_path |
| POST | `/api/v1/imports/by-path/preview-updated` | Hash-sync notify (output_hash) |
| POST | `/api/v1/imports/by-source/rotation-updated` | **NEW** rotation intent `{source_path, orientation, client_id}` → `202 Accepted` |

Reconvert body:
```json
{ "conversion_settings": { "compression":"-c", "preview":"-p1", "version":"-dng1.4", "linear":"" }, "reason": "Windows thumbnail compatibility fix" }
```

Rotation-updated body:
```json
{ "source_path": "/watch/IMG_1983.NRW", "orientation": 6, "client_id": "darktable-lua" }
```

---

## 9. Configuration & Environment

Port all `DEF_*` + add rotation/Immich/digiKam vars.

| Var | Default | Notes |
|-----|---------|-------|
| `DB_DRIVER` | `mariadb` | MariaDB-only in v1 |
| `DB_HOST/PORT/USER/PASSWORD/NAME` | — | MariaDB connection |
| `WATCH_DIR` | `/watch` | |
| `OUTPUT_DIR` | `/output` | |
| `ARCHIVE_DIR` | `/archive` | |
| `FOLDER_SCHEMA` | `%Y/%m` | |
| `FILE_PATTERN` | `IMG_{seq}` | |
| `CONVERTER_ENGINE` | `dnglab` | `adobedng`/`libraw` stubs |
| `EXIFTOOL_BIN` | `exiftool` | path override |
| `GEN_THUMB_JPEG` | `false` | sidecar thumbnail toggle |
| `DEF_COMPRESSION` | `lossless` | |
| `DEF_DNG_VERSION` | `1.4` | |
| `DEF_PREVIEW_MEDIUM` | `1024x1024` | |
| `DEF_PREVIEW_FULL` | `4000x3000` | |
| `DEF_JPEG_QUALITY` | `92` | |
| `DEF_LINEAR` | `false` | |
| `ALERT_PUSH_URL` | `` | ntfy/Gotify (off) |
| `ROTATION_GRACE_MS` | `2000` | rotation coalescing window |
| `ROTATION_MODE` | `metadata` | `metadata` only safe for live sync |
| `IMMICH_URL` | `` | Immich base URL |
| `IMMICH_TOKEN` | `` | service-account token |
| `DIGIKAM_RESCAN` | `touch` | `touch` (mtime) | `dbus` (Linux stub) |
| `RAWIMPORT_API_URL` | `http://localhost:8080` | used by Lua plugin |
| `API_TOKEN` | `` | optional bearer for Lua→API |

---

## 10. Docker / CI

- **Dockerfile:** `debian:bookworm-slim` + CMake build stage → runtime with `dnglab` binary, `exiftool`, `libexif12`, `ca-certificates`, optional DNG SDK build for `dng_preview_embed`. `WORKDIR /app`, `COPY migrations ./migrations/` (fix from 2026-07-14: migrations MUST be bundled). Non-root `appuser` UID 10001. `HEALTHCHECK` on `/health`.
- **docker-compose.yml:** MariaDB 10.11 sidecar, healthchecks, volumes (`/watch`,`/output`,`/archive`,`/db`), `depends_on: service_healthy`.
- **CI:** GitHub Actions builds + pushes GHCR always; Docker Hub gated on `DOCKERHUB_TOKEN` secret (`if: ${{ env.DOCKERHUB_TOKEN != '' }}`). Use Node-24-compatible action majors (`checkout@v7`, `build-push-action@v7`).
- **Branch:** commit/push to `main` only.

---

## 11. Migration from Go → C++

1. **DB compatibility:** C++ schema is a superset (adds `orientation`). Existing Go DBs upgrade via `0002_orientation.sql` (`ADD COLUMN IF NOT EXISTS`). No data loss.
2. **Behavior parity checklist:**
   - [ ] Poller debounce + skip rules identical
   - [ ] SHA-256 before/after, stored atomically
   - [ ] `IMG_{n}` sequence gaps-OK, never reused
   - [ ] dnglab `convert` CLI exact (no aborting flags)
   - [ ] Thumbnail extract SubIFD1→SubIFD2, `GEN_THUMB_JPEG` default false
   - [ ] Re-embed Lua flow + by-source API + preview-updated notify
   - [ ] Reconcile legacy library on startup
   - [ ] All REST endpoints return identical shapes
   - [ ] `processing_locks` serialization for reconvert/re-embed/rotation
3. **Cutover:** deploy C++ image; point at same MariaDB + volumes; verify `/health` + one live import + one re-embed + one rotation spam test.
4. **Lua plugin:** move `betterembeds.lua` to `tools/`, add rotation shortcut/button.

---

## 12. Open Questions / Risks

1. **C++ DNG muxer vs dnglab subprocess:** keep dnglab subprocess (lowest risk, verified) or invest in native C++ muxer? Recommend subprocess for v1.
2. **Web UI:** embed tiny SPA or API-only? Default API-only.
3. **`dng_previewembed.cpp` medium preview:** implement full pyramid or rely on dnglab? Document decision.
4. **Rotation double-rotation trap:** enforced via `ROTATION_MODE=metadata` default; must never silently switch to `baked` for live sync.
5. **digiKam D-Bus:** Linux-host-only; `touch` mode is the portable default.
6. **Immich token:** requires user to provision a service-account token; absent → emit `alerts` row instead of failing.

---

## 13. Glossary

| Term | Definition |
|------|------------|
| NRW/NEF/CR2/ARW | Camera RAW formats |
| DNG | Digital Negative (Adobe open raw format) |
| dnglab | Rust `vibelabdng` tool; default conversion + re-embed engine |
| SHA-256 | Cryptographic hash for integrity verification |
| Monotonic sequence | Numbers only increase, never reused |
| Atomic transaction | DB op succeeds completely or fails completely |
| Re-conversion | Re-run pipeline with different settings |
| Re-embed | Write edited preview JPEG into existing DNG |
| Rotation sync | Reflect EXIF `Orientation` 1–8 across Darktable/Immich/digiKam |
| Grace coalescing | Server-side debounce: spammy intents collapse to one job |

---

*End of C++ Rewrite PRD — generated from repo @ 6f9c705 + memory-bank, 2026-07-16.*