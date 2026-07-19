# CLAUDE.md — RawImport Pipeline C++20 Rewrite (Agent Contract)
# Companion to PRD_RawImport_Pipeline_CppRewrite.md + ORCHESTRATION_CppRewrite.md.
# All Cline Agent Team members MUST read this before editing any file.

## Scope
Full rewrite of `autodngconverter` from Go → C++20. Single binary `rawimport-pipeline`.
dnglab (Rust `vibelabdng` submodule) stays an **external subprocess**.

## Hard Rules
- **C++20**, CMake ≥ 3.25. Standard library + pinned deps only (see CMakeLists.txt).
- **Branch discipline:** remote default is `main`. Work on a feature branch; PR to `main`.
  No `master` branch ever.
- **Headers are frozen** after Phase 0 bootstrap (LEAD). Agents implement `.cpp` against
  the existing `.h` in `src/`. Do NOT modify `.h`, `main.cpp`, `CMakeLists.txt`, or
  `migrations/` unless your assignment explicitly says so.
- **dnglab convert CLI is fixed:** `dnglab convert --input <SRC> --output <DST> -c <lossless|uncompressed> --keep-mtime <true|false> -f`. NEVER add `--preview-medium/--preview-full/--dng-version/--jpeg-quality/--compress/--linear/--seed` (aborts conversion).
- **Single serial converter worker** at runtime — no concurrent conversion. Parallelism is
  DEV-only (multiple agents).
- **`processing_locks`** serializes reconvert / re-embed / rotation jobs on the same DNG.
- Logs via `<spdlog/spdlog.h>` JSON; no `std::cout` in production paths.

## Shared Types (do not redefine)
- `rawimport::ImportRecord` (pipeline.h) — DB `imports` row mirror.
- `rawimport::ConversionSettings` (pipeline.h) — dnglab settings.
- `rawimport::Config` (config.h) — all env vars (PRD §9).
- `rawimport::Store` (db.h) — MariaDB store; uses pImpl.
- `rawimport::ConverterEngine` / `PreviewEmbedder` (converter.h).
- `rawimport::RotationManager` (rotation.h) — `Queue(import_id, dng_path, orientation, client_id)`, `Stop()`.
- `rawimport::Worker` (worker.h) — `Start()`, `Stop()`, `QueueReconvert()`.

## Module Ownership (per ORCHESTRATION §3–§4)
| File | Owner | Phase |
|------|-------|-------|
| src/config.cpp | A1 | Wave 1 |
| src/util.cpp | A2 | Wave 1 |
| src/db.cpp | A3 | Wave 1 |
| src/pipeline.cpp | A4 | Wave 1 |
| tools/betterembeds.lua | A5 | Wave 1 |
| src/converter.cpp | B1 | Wave 2 |
| src/rotation.cpp | B2 | Wave 2 |
| src/reconcile.cpp | B3 | Wave 2 |
| src/api.cpp | B4 | Wave 2 |
| tools/dng_previewembed.cpp | B5 | Wave 2 |
| src/worker.cpp | LEAD | Phase 3 |
| src/main.cpp | LEAD | Phase 3 |

## Build / Verify (LEAD only)
- `cmake -S . -B build && cmake --build build` must pass with `-Wall -Wextra`.
- Headers must compile standalone: `g++ -fsyntax-only -std=c++20 src/*.h`.
- Final e2e (Phase 4): docker build + MariaDB 10.11 sidecar; drop DSCN6496.NRW;
  rotation spam test (20 rapid POSTs → 1 DNG rewrite, final orientation, no lock overlap).

## Out of Scope for v1
Web UI (API-only), libraw/adobedng engines (stubs), Postgres/SQLite, full DNG muxer.