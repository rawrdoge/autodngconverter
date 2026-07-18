# autodngconverter

A containerized pipeline that watches a folder for camera RAW files (NRW, NEF,
CR2, ARW), converts them to DNG, assigns a global monotonic `IMG_{n}` sequence,
and records SHA-256 hashes of source and output in a MariaDB database. A
Darktable Lua plugin lets you re-embed an edited preview back into a DNG and
keep the database hash in sync.

This project was built with AI assistance (vibe coded). It is a personal tool,
not a hardened product. Expect rough edges, missing tests, and behavior that
has only been exercised on the author's own hardware. Use it at your own risk
and verify your files before trusting it with anything you cannot lose.

## Repository layout

This is one of two repositories:

- `autodngconverter` (this repo): the Go service, the Darktable Lua plugin,
  the SQL migrations, and the Docker files.
- `vibelabdng`: a fork of [DNGLab](https://github.com/dnglab/dnglab) that adds
  a `reembed` subcommand. It is pulled in here as a git submodule under
  `vibelabdng/`.

## Container image

The image is built automatically and published to GitHub Container Registry on
every push to `main`:

```
ghcr.io/rawrdoge/autodngconverter:latest
```

The `dnglab` converter from the `vibelabdng` submodule is compiled into the
image at build time, so `DNGLAB_BIN` already points at the baked-in binary. You
do not need to build or mount anything to convert files.

## Running with Docker

The service is designed to run as a container alongside MariaDB. A
`docker-compose.yml` is included that pulls the published image.

### docker-compose.yml

```yaml
services:
  rawimport:
    image: ghcr.io/rawrdoge/autodngconverter:latest
    container_name: rawimport
    restart: unless-stopped
    environment:
      - DB_DRIVER=mariadb
      - DB_HOST=mariadb
      - DB_PORT=3306
      - DB_USER=rawimport
      - DB_PASSWORD=${DB_PASSWORD}
      - DB_NAME=rawimport
      - DB_SSLMODE=disable
      - WATCH_DIR=/watch
      - OUTPUT_DIR=/output
      - ARCHIVE_DIR=/archive
      - CONVERTER_ENGINE=dnglab
      - EXIFTOOL_BIN=exiftool
      - FOLDER_SCHEMA=%Y/%m
      - FILE_PATTERN=IMG_{seq}
      - POLL_INTERVAL=10
      - LOG_LEVEL=info
      - PORT=8080
    volumes:
      - /mnt/nas/photos/watch:/watch
      - /mnt/nas/photos/output:/output
      - /mnt/nas/photos/archive:/archive
    ports:
      - "8080:8080"
    depends_on:
      mariadb:
        condition: service_healthy

  mariadb:
    image: mariadb:10.11-jammy
    container_name: rawimport_mariadb
    restart: unless-stopped
    environment:
      - MARIADB_ROOT_PASSWORD=${DB_ROOT_PASSWORD}
      - MARIADB_DATABASE=rawimport
      - MARIADB_USER=rawimport
      - MARIADB_PASSWORD=${DB_PASSWORD}
    volumes:
      - mariadb-data:/var/lib/mysql
    healthcheck:
      test: ["CMD", "healthcheck.sh", "--connect", "--innodb_initialized"]
      interval: 10s
      timeout: 5s
      retries: 3
      start_period: 30s

volumes:
  mariadb-data:
```

Set `DB_PASSWORD` and `DB_ROOT_PASSWORD` in a `.env` file next to the compose
file (it is gitignored and never committed). Then:

```
docker compose up -d
```

The watcher picks up files dropped into the `watch` volume, converts them, and
writes DNGs to `output` and the originals to `archive`.

## Environment variables

All configuration is via environment variables. Defaults are shown in
parentheses.

| Variable | Purpose | Default |
|----------|---------|---------|
| `DB_DRIVER` | Database backend. Only `mariadb` is implemented in v1. | `mariadb` |
| `DB_HOST` | MariaDB host. | `mariadb` |
| `DB_PORT` | MariaDB port. | `3306` |
| `DB_USER` | MariaDB user. | (empty) |
| `DB_PASSWORD` | MariaDB password. | (empty) |
| `DB_NAME` | MariaDB database name. | `rawimport` |
| `DB_SSLMODE` | TLS mode for the DB connection. | (empty) |
| `WATCH_DIR` | Directory the watcher scans for new RAW files. | `/watch` |
| `OUTPUT_DIR` | Directory DNGs are written to. | `/output` |
| `ARCHIVE_DIR` | Directory original RAW files are moved to after conversion. | `/archive` |
| `CONVERTER_ENGINE` | Converter to use. `dnglab` is the default. | `dnglab` |
| `DNGLAB_BIN` | Path to the vibelabdng/dnglab binary. | `dnglab` |
| `EXIFTOOL_BIN` | Path to the exiftool binary (used by the re-embed path). | `exiftool` |
| `FOLDER_SCHEMA` | Output subfolder layout, `strftime` format. | `%Y/%m` |
| `FILE_PATTERN` | Output filename pattern. `{seq}` becomes the monotonic `IMG_{n}`. | `IMG_{seq}` |
| `POLL_INTERVAL` | Watcher poll interval in seconds. | `10` |
| `LOG_LEVEL` | Log verbosity (`debug`, `info`, `warn`, `error`). | `info` |
| `PORT` | Port the REST API listens on. | `8080` |
| `API_TOKEN` | Optional bearer token. If set, the preview-updated notify endpoint requires `Authorization: Bearer <API_TOKEN>`. | (empty) |
| `ALERT_PUSH_URL` | Optional URL to push alert webhooks to. | (empty) |
| `GEN_THUMB_JPEG` | Write a standalone `IMG_{n}.thumb.jpg` sidecar next to each DNG in `/output`. **Off by default** so the output library contains only DNGs. | `false` |
| `DEF_COMPRESSION` | Default RAW compression for new imports: `lossless` \| `uncompressed`. | `lossless` |
| `DEF_DNG_VERSION` | Default DNG spec version for new imports. | `1.4` |
| `DEF_PREVIEW_MEDIUM` | Default medium preview size `WxH`. | `1024x1024` |
| `DEF_PREVIEW_FULL` | Default full preview size `WxH`. | `4000x3000` |
| `DEF_JPEG_QUALITY` | Default JPEG preview quality (0-100). | `92` |
| `DEF_LINEAR` | Default linear (demosaiced) DNG flag. | `false` |

## Commands and endpoints

The container runs a single binary that starts the watcher, converter, hasher,
and a small REST API. There is no CLI subcommand surface; interaction is
through the API and the Darktable plugin.

REST API (listening on `PORT`):

- `GET /api/v1/imports?page=1&limit=50&status=completed` — list imports.
- `GET /api/v1/imports/{sequence}` — get one record by `IMG_{n}`.
- `GET /api/v1/imports/hash/{sha256}` — find a record by source or output hash.
- `POST /api/v1/imports/{sequence}/reconvert` — queue a re-conversion. Body:
  `{ "conversion_settings": { "compression": "lossless", "version": "1.4",
  "preview_medium": "1024x1024", "preview_full": "4000x3000", "jpeg_quality": 92,
  "linear": false, "seed": "<optional>" }, "reason": "..." }`. These settings are
  forwarded verbatim to the dnglab `convert` subcommand; omitted fields fall back
  to the `DEF_*` env defaults.
- `GET /api/v1/stats` — conversion counts and failure rates.
- `GET /api/v1/alerts` — recent alert rows.
- `POST /api/v1/imports/by-path/preview-updated` — notify endpoint the Darktable plugin calls after a preview re-embed (updates the stored `output_hash`).
- `GET /api/v1/imports/by-source?path=<source RAW path>` — resolve a source RAW path to its DNG output path (used by the plugin's export-hook).
- `POST /api/v1/imports/by-source/rotation-updated` — notify endpoint the Darktable plugin calls after a rotation edit. Body: `{ "source_path": "<RAW path>", "orientation": <1-8>, "client_id": "<id>" }`. Rapid intents for the same file are coalesced into a single DNG rewrite (last orientation wins) after a short grace timer.
- `GET /metrics` — Prometheus-format metrics: `rawimport_files_detected_total`, `rawimport_conversions_completed_total{status="completed|failed"}`, `rawimport_conversion_duration_seconds` (histogram), `rawimport_queue_depth`, `rawimport_db_size_bytes`.
- `GET /health` — liveness/readiness probe.

Darktable plugin (`betterembeds.lua`):

Install the script in Darktable, select DNGs in Lighttable, and click
"Export & Embed Previews". By default it uses ExifTool (fast tag swap). A
dropdown switches to the `vibelabdng reembed` worker for a native
multi-resolution Adobe preview. After a successful embed the plugin POSTs to
the API so the stored `output_hash` stays correct and the corruption monitor
does not false-positive.

### Pointing the plugin at the API

The API base URL and optional token are configured at the **top of the script
file** — no environment variables are required. Open `betterembeds.lua` and
edit the `USER CONFIG` block:

```lua
-- USER CONFIG
local RAWIMPORT_API_URL = "http://localhost:8080"   -- base URL, NO trailing slash, NO "/api/v1"
local API_TOKEN         = ""                        -- optional; must match the server's API_TOKEN
```

- `RAWIMPORT_API_URL` is the base URL of the API (e.g. `http://192.168.1.50:8080`
  when the service runs on your NAS). Do **not** include a trailing slash or the
  `/api/v1` prefix — the plugin appends the path itself.
- `API_TOKEN` is only needed if the server was started with `API_TOKEN` set. If
  the server runs with no token (the default), leave this as `""`.

If you prefer environment variables, leave both constants as `""` and the
plugin falls back to `RAWIMPORT_API_URL` / `API_TOKEN` from the process that
launched Darktable. The in-script constant always wins when set.

After editing, restart Darktable. A successful re-embed prints
`Notified API: preview hash updated`; a `WARN: API notify failed (...)` means
the URL is unreachable or the token does not match.

## Building without Docker

```
go build -o rawimport-pipeline .
```

Copy `.env.example` to `.env` and fill in your MariaDB credentials.

## Goals

The pipeline exists to solve a few concrete problems with ad-hoc RAW
workflows:

- Run the conversion as a containerized service on a NAS instead of a fragile
  shell script tied to one host.
- Convert camera RAW files (NRW, NEF, CR2, ARW) to DNG with a verifiable
  preview structure.
- Assign a global, monotonic `IMG_{n}` sequence so every output has a unique,
  stable name.
- Record SHA-256 of both the source RAW and the output DNG, linked to the
  sequence, so corruption months later can be detected and traced.
- Support re-conversion (for example, to change the preview for Windows
  compatibility) without losing the provenance of the original file.

## Deferred and future work

Out of scope for the current version:

- Cloud storage backends (S3, B2).
- ML-based duplicate detection.
- Automatic RAW development via Darktable CLI.
- Multi-tenant isolation.
- Real-time sync to Immich or DigiKam.

Possible later work, in rough order:

- PostgreSQL backend for multi-node setups, plus CR2/ARW/RAF support and
  EXIF-based auto-tagging.
- Darktable CLI integration for automatic development, an Immich API webhook
  to push after conversion, and a cold-storage tier for old RAW files.
- A distributed job queue for scaling the converter, ML-based scene
  detection for folder organization, and a mobile monitoring app.

## Status

**v1.0.1** — adds the two core features that were deferred from v1.0.0:
- **Rotation / orientation sync** (`POST /api/v1/imports/by-source/rotation-updated`):
  the Darktable plugin posts an orientation intent; a coalescing
  `RotationManager` (grace timer, last-orientation-wins) writes EXIF
  `Orientation` to the DNG via exiftool under a `processing_locks` guard,
  then syncs the DB row — PRD §5, ORCHESTRATION §7.4.
- **Prometheus `/metrics`** endpoint exposing the five required series
  (`rawimport_files_detected_total`, `rawimport_conversions_completed_total`,
  `rawimport_conversion_duration_seconds`, `rawimport_queue_depth`,
  `rawimport_db_size_bytes`) — PRD §3.6.

**v1.0.0** — first tagged release of the Go service. Core pipeline is
functional: watcher (poll + 2s debounce + partial-file skip), dnglab
conversion via vibelabdng, SHA-256 of source and output, global
monotonic `IMG_{n}` sequence, re-conversion API, preview-updated hash
sync, thumbnail sidecar (`GEN_THUMB_JPEG`), the REST API, and the
Darktable Lua re-embed plugin.

The service remains a personal tool, shared as-is under free and
open source licenses.

## Credits and license

The `vibelabdng/` subtree is a fork of DNGLab, originally created in 2021 by
Daniel Vogelbacher and released under the GNU Lesser General Public License
v2.1. The fork keeps the original `LICENSE` and `AUTHORS` files intact. DNGLab
credits these contributors:

- Alfred Gutierrez (BMFF box parsing from mp4-rust)
- Andrew Baldwin (lossless JPEG-92 compression)
- Pedro Corte-Real (rawloader rust library)
- Alexey Danilchenko and Alex Tutubalin / LibRaw LLC (crx decoder from libraw)

The original code in this repository (the Go service, the Lua plugin, and the
docs) is licensed under the MIT License. See `LICENSE` and `NOTICE`.
