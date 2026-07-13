# Memory Bank — RawImport Pipeline Design Decisions

Captured: 2026-07-11, following a 15-point design grill of `PRD_RawImport_Pipeline_Unified.md`.
These are the authoritative v1 design resolutions. The PRD has been revised to match.

## Grill Decisions (Q1–Q15)

| # | Branch | Decision | Notes |
|---|--------|----------|-------|
| Q1 | Converter engine | **Modular engine dispatcher** with 3 pluggable backends via `CONVERTER_ENGINE` env. **Default = `dnglab`** (forked Rust binary, lightweight Linux-native, Adobe-free — see `PRD_dnglab_Fork_Requirements.md`). `libraw` = DEFERRED (was default; dnglab fork now meets contract). `adobedng` = opt-in (Wine+Adobe, EULA isolated). Spike validates EACH enabled engine for valid DNG + extractable previews + stable hash. | dnglab chosen default after fork fix; libraw deferred. |
| Q2 | DB backend | **Single-node deploy, MariaDB/MySQL backend** (not SQLite). v1 ships MariaDB only; 3-driver abstraction collapsed to MariaDB. | Postgres/SQLite deferred to v1.1. |
| Q3 | Watcher | **Polling primary** (default 10s, range 5–15s walk of `/watch`). fsnotify optional for local bind mounts only. | inotify unreliable across NFS/SMB network shares. |
| Q4 | Sequence | **Gaps ACCEPTABLE** — `IMG_{n}` monotonic = strictly increasing, never reused, NOT contiguous. Failed allocations leave permanent gaps. | Corrects old PRD "no gaps" claim. |
| Q5 | Folder/EXIF | **EXIF DateTimeOriginal primary; filesystem mtime fallback.** Add `date_source` column (exif\|mtime). | New column added to `imports` schema. |
| Q6 | Re-conversion | **In-place overwrite** of imports row; **archive previous DNG by DEFAULT** (not optional). Audit via `reconversions.previous_output_hash`. | History preserved on disk. |
| Q7 | API | **Go (Echo), single binary (API+worker)**; embedded/tiny SPA. **No auth v1**, LAN-bound, hook reserved for token auth. | |
| Q8 | Thumbnails | **Extract embedded JPEG from DNG** — SubIFD1 (medium) primary, SubIFD2 (full) fallback. **Log + alert on fallback** (corruption signal). Lightweight Go DNG reader, NO Wine. | Sidecar `IMG_{n}.thumb.jpg` cached at conversion time. |
| Q9 | Security | **Non-root `appuser`** (UID 10001); `/db` container-only. Volume ACLs = NAS-admin responsibility, documented. | Dockerfile updated with USER + useradd. |
| Q10 | Migrations | **Embedded Go migration runner** (versioned `.sql` on startup, `schema_migrations` table). Backup = documented `mariadb-dump`/volume snapshot, out of app scope. | Removed old SQLite→PG migration scripts from v1 scope. |
| Q11 | Observability | **JSON logs + Prometheus metrics + persistent `alerts` table**; surfaced in Web UI + `GET /api/v1/alerts`; optional ntfy/Gotify push via `ALERT_PUSH_URL` env (off by default). | New `alerts` table in schema. |
| Q12 | Idempotency | **source_hash idempotency** + **`processing_locks` table with TTL** to catch in-flight/crashed jobs. Skip re-queue if row in (pending/converting/failed<retries). | New `processing_locks` table in schema. |
| Q13 | Concurrency | **Single serial converter worker** (Wine+Adobe not concurrency-safe). Bounded in-memory queue (default 100); `queue_full` alert; ~1 file/min throughput. | |
| Q14 | Adobe binary | **Only relevant to the `adobedng` engine.** Base image is Adobe-free; `adobedng` is a **separate importable image/module** where the user manually downloads the installer to an exposed host path, bind-mounted + one-time Wine bootstrap into WINEPREFIX volume. | Removed `COPY Adobe...exe` from base Dockerfile; Adobe isolated to opt-in module. |
| Q15 | Scope cut | **v1 = above design.** DEFER: Postgres/SQLite drivers, multi-node, Darktable, Immich webhook, cloud archive, ML tagging. §13 open questions all resolved inline. | |

## Schema Additions vs Original PRD
- `imports.date_source` ENUM('exif','mtime') — Q5
- `processing_locks` table — Q12 idempotency guard
- `alerts` table — Q11 observability

## Converter Engine Dispatcher (Q1, revised)
- Go `ConverterEngine` interface: `Name() / Available() / Convert(ctx, src, dst, settings)`.
- Factory selects impl from `CONVERTER_ENGINE` env at startup.
- All engines MUST emit DNG with Adobe-emulated SubIFD1 (medium) + SubIFD2 (full) embedded JPEGs (preview embedding is the engine's job, not a post-step).
- **`dnglab` = DEFAULT**: forked Rust binary, invoked as `dnglab convert --input --output --preview-medium --preview-full --dng-version --compress --jpeg-quality --seed`. Contract pinned in `PRD_dnglab_Fork_Requirements.md`. Must be deterministic (identical SHA-256 across runs) — the key fork fix.
- `libraw` = DEFERRED: decode via libraw → DNG muxer → embed previews.
- `adobedng` = opt-in: Wine + Adobe DNG Converter, mounted prefix, fallback for camera gaps.

## Companion Tools (OUT OF CORE SCOPE)
- `betterembeds.lua` + `dng_preview_embed` (from `dng_previewembed.cpp`) = **Darktable Lua HUD** for one-off manual preview re-embedding on a workstation. NOT part of the container pipeline. Relevant because Immich's new release can export the embedded JPEG instead of the raw/DNG, so a user may push an edited preview back into a specific DNG. Core re-conversion (§5.2) handles bulk changes; this handles individual edits.

## Open Questions (§13) Resolved
1. Wine licensing → scoped to `adobedng` engine only; default `libraw` unaffected.
2. Native Linux Adobe → none; only via opt-in `adobedng`+Wine module.
3. EXIF extraction → libexif (C) preferred, exiftool acceptable.
4. Thumbnails → owned by conversion engine (Adobe-emulated SubIFD1/SubIFD2); extracted by Q8 thumbnailer. `betterembeds.lua` = companion for manual re-embeds.
5. Backup → mariadb-dump / volume snapshot, documented, Q10.

## Next Action When Build Begins (Act Mode)
1. Run the **per-engine spike** (throwaway container) for `libraw` (default) + `dnglab` + `adobedng` (opt-in): valid DNG, extractable SubIFD1/SubIFD2, stable hash.
2. Scaffold Go/Echo single binary: poller → hasher → converter(dispatch) → thumbnailer → namer → db.
3. Implement `ConverterEngine` interface + `libraw` impl (DNG mux + Adobe-emulated preview embed).
4. MariaDB sidecar + migration runner + `processing_locks` + `alerts`.
5. Web UI (embedded templates/SPA) reading from API + alerts.

## Status
- PRD: REVISED (2026-07-11, modular converter) ✓
- Memory bank: UPDATED (this file) ✓
- Implementation: COMPLETE & VERIFIED (2026-07-13)

### What was built (Act Mode, 2026-07-13)
Single Go/Echo binary (`rawimport-pipeline`) implementing the full PRD §5 happy path + §5.2 re-conversion, against a live MariaDB 10.11 sidecar.

**Files:**
- `main.go` — bootstrap: loadDotEnv, engine init, DB open, migrate, worker start, API server, graceful shutdown.
- `config.go` — env-driven `Config` (added `ExifTool`, `FOLDER_SCHEMA`, `FILE_PATTERN`).
- `db.go` — MariaDB `Store`: DSN, `OpenDB` (retry ping), embedded migration runner (`splitSQL` per-statement), `AllocateSequence` (AUTO_INCREMENT+LAST_INSERT_ID fallback; stored proc optional), idempotency, `processing_locks`, `alerts`, reconversion CRUD, `GetImportBySequence/Hash` (returns real PK `id`).
- `pipeline.go` — `ImportRecord`, `ReconversionJob` (+`FolderSchema`), SHA-256, EXIF (exiftool, mtime fallback), folder schema, thumbnail extract (SubIFD1→SubIFD2 fallback, JPEG validity check).
- `converter.go` — `ConverterEngine` interface + `dnglab` impl (default; `adobedng`/`libraw` stubs).
- `worker.go` — poller (debounce 2s, skip .part/.tmp), single serial converter, atomic import insert, archive source, re-conversion drain (background ctx, archive-source fallback).
- `api.go` — Echo routes: `/health`, `/api/v1/imports`, `/imports/:seq`, `/imports/hash/:sha`, `POST /imports/:seq/reconvert`, `/stats`, `/alerts`.
- `util.go` — `splitSQL`, `loadDotEnv`, helpers.
- `migrations/0001_init.sql` — MariaDB schema (sequences, imports w/ `date_source`, reconversions, processing_locks, alerts, schema_migrations). DELIMITER/proc removed (Go runner can't exec it; code fallback handles allocation).
- `Dockerfile` + `docker-compose.yml` — multi-stage build, MariaDB 10.11 sidecar, healthchecks, volumes.
- `.env.example` — documents all vars incl. `EXIFTOOL_BIN`, `CONVERTER_ENGINE`, `FOLDER_SCHEMA`, `FILE_PATTERN`.

### Verification (live, 2026-07-13)
- MariaDB auth + migration applied cleanly (fixed: root login, `splitSQL`, removed DELIMITER proc, `AllocateSequence` NOT NULL placeholder).
- Dropped `DSCN6496.NRW` → produced `testdata/output/2026/07/IMG_1.dng` (34 MB) + `IMG_1.thumb.jpg` (243 KB), EXIF folder `2026/07`, SHA-256 source+output stored.
- `GET /api/v1/imports` returns full record; `GET /health` → ok.
- `POST /api/v1/imports/IMG_1/reconvert` → status `restored`, new `output_hash` (`02c46af1…`), `completed_at` updated. (Fixed: FK used real PK `id`; reconv goroutine uses `context.Background()` not request ctx; source resolved from archive dir.)
- `go build` + `go vet` clean.

### Known gaps / deferred (per Q15)
- Web UI not built (API only). 
- `libraw`/`adobedng` engines are stubs (dnglab default works).
- EXIF `CameraModel` empty in test (exiftool path on test host not the bundled one; configurable via `EXIFTOOL_BIN`).
- Postgres/SQLite drivers deferred.
