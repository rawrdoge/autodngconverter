# Orchestration & Agent-Swarm Brief — RawImport Pipeline C++20 Rewrite
# Companion to: PRD_RawImport_Pipeline_CppRewrite.md (v2.0.0)
# Date: 2026-07-16
# Target: Cline Agent Teams (parallel multi-agent development)

## 0. Purpose
This document is the **execution companion** to the PRD. It defines how to parallelize
the C++20 rewrite of `autodngconverter` using Cline Agent Teams. The PRD is the *what*;
this brief is the *how* — agent assignments, contracts, and swarm coordination rules.

## 1. Hard Rules (all agents MUST obey)
- **Branch discipline:** remote default is `main`. Integration merges land on `main` only.
  No `master` branch. Agents work on feature branches, PR back to `main`.
- **C++20**, CMake ≥ 3.25, `debian:bookworm-slim` runtime.
- **Do NOT modify headers** (`.h`) written in the bootstrap phase — implement against them.
- **Do NOT touch `main.cpp`, `CMakeLists.txt`, or `migrations/`** unless explicitly assigned.
- **dnglab is an external subprocess** (Rust `vibelabdng`). Invoke via `popen`/`std::process`.
  Authoritative CLI: `dnglab convert --input <SRC> --output <DST> -c <lossless|uncompressed>
  --keep-mtime <true|false> -f`. NEVER pass `--preview-medium/--preview-full/--dng-version/
  --jpeg-quality/--compress/--linear/--seed` (they abort conversion).
- **Single serial converter worker** at runtime — no concurrent conversion. Parallelism is
  DEV-only (multiple agents), not runtime.
- **`processing_locks`** serializes reconvert / re-embed / rotation jobs on the same DNG.

## 2. Phase 0 — Serial Bootstrap (LEAD agent only, before any swarm)
The lead writes the interface contract so parallel agents cannot diverge:
1. All `.h` headers: `config.h`, `util.h`, `db.h`, `pipeline.h`, `converter.h`,
   `rotation.h`, `reconcile.h`, `api.h`, `worker.h` (declarations + structs only).
2. `CMakeLists.txt` with all deps pinned: `libmariadb`/`soci`, `nlohmann/json`,
   `openssl`, `spdlog`, `prometheus-cpp`, `cxxopts`. C++20 standard.
3. `migrations/0001_init.sql` + `migrations/0002_orientation.sql` (from PRD §7).
4. `Dockerfile` (toolchain: cmake + g++-12 + libs + dnglab binary + exiftool).
5. `CLAUDE.md` (module contract — see §5).
→ Gate: headers compile standalone (`g++ -fsyntax-only *.h`). Then fan out.

## 3. Agent Swarm — Wave 1 (PARALLEL, 5 agents)
Each agent gets ONE module + its header + the PRD section. Independent files; safe to run concurrently.
| Agent | File | PRD ref | Key contract |
|-------|------|---------|--------------|
| A1 | `src/config.cpp` | §9 | Parse all env vars; `Config` struct from `config.h` |
| A2 | `src/util.cpp` | §6.3 | `splitSQL`, dotenv, helpers from `util.h` |
| A3 | `src/db.cpp` | §7 | MariaDB store, `AllocateSequence`, locks, `UpdateOrientation` |
| A4 | `src/pipeline.cpp` | §3.1 | SHA-256 (OpenSSL), EXIF, folder schema, thumbnail extract |
| A5 | `tools/betterembeds.lua` | §3.2,§5.3 | re-embed + rotate_cw + bindable shortcut/lib button |

## 4. Agent Swarm — Wave 2 (PARALLEL, 5 agents)
After Wave 1 merged + compiled.
| Agent | File | PRD ref | Key contract |
|-------|------|---------|--------------|
| B1 | `src/converter.cpp` | §3.1,§6.2 | `ConverterEngine` iface + dnglab impl; adobedng/libraw stubs |
| B2 | `src/rotation.cpp` | §5 | `RotationManager`: pending map + grace timer (2000ms) + lock |
| B3 | `src/reconcile.cpp` | §3.3 | legacy scan, reserve sequence past max on-disk n |
| B4 | `src/api.cpp` | §8 | all HTTP routes + `POST /api/v1/imports/by-source/rotation-updated` |
| B5 | `tools/dng_previewembed.cpp` | §4,§6.1 | promote DNG SDK fallback; fix medium-preview stub |

## 5. Module Contract (CLAUDE.md excerpt — shared by all agents)
- `ImportRecord` (pipeline.h): id, sequence_id, source_path, source_hash, output_path,
  output_hash, camera_model, capture_date, capture_time, folder_schema,
  conversion_settings(JSON string), status, date_source, orientation(int), timestamps.
- `Config` (config.h): all PRD §9 vars as fields.
- `ConverterEngine` (converter.h): `Name()`, `Available()`, `Convert(ctx,src,dst,settings)`.
- `RotationManager` (rotation.h): `Queue(importID, orientation)`, `Stop()`.
- Lock helper (db.h): `AcquireLock(importID, ttl)`, `ReleaseLock(importID)`.
- All agents include `<spdlog/spdlog.h>` for JSON logs; no `std::cout` in prod paths.

## 6. Phase 3 — Serial Integration (LEAD only)
1. Write `src/worker.cpp` (poller + serial converter + atomic insert + archive + reconvert drain).
2. Write `src/main.cpp` (bootstrap: config→engine→db→migrate→reconcile→worker→api→graceful shutdown).
3. `cmake --build` ; fix all compile/link errors.
4. Merge Wave 1+2+integration to `main`.

## 7. Phase 4 — Serial Verification (LEAD only)
1. `docker build` + run against live MariaDB 10.11 sidecar.
2. Drop `DSCN6496.NRW` → assert `IMG_1.dng` produced, DB row `status=completed`, hashes stored.
3. Re-embed test (lua export) → `output_hash` synced.
4. **Rotation spam test:** 20 rapid `POST .../rotation-updated` (orientations 6→3→8→…)
   → assert EXACTLY ONE DNG EXIF rewrite, final `orientation` = last value, no `processing_locks` overlap.
5. `cmake --build` clean; no warnings treated as errors.

## 8. Swarm Coordination Notes
- Agents MUST NOT edit files outside their assigned module (prevents merge conflicts).
- Shared types live in headers (Phase 0) — agents implement, never redefine.
- If an agent needs a header change, it files an issue to the LEAD, not a direct edit.
- Each agent commits to its own branch with message `feat(<module>): <summary>`; PR to `main`.
- LEAD reviews + merges Wave 1 before launching Wave 2 (interface stability).