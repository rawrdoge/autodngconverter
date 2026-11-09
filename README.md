# autodngconverter

Containerized RAW→DNG pipeline. Watches a folder, converts camera RAWs (NRW, NEF, CR2, ARW) to DNG via dnglab, assigns global monotonic `IMG_{n}` sequences, records SHA-256 of source/output in PostgreSQL. Darktable Lua plugin re-embeds edited previews and syncs hashes.

## Quick Start

**1. Provision PostgreSQL 15+** (external; schema is migrated automatically on startup):

```sql
CREATE DATABASE rawimport;
CREATE USER rawimport WITH PASSWORD 'your-password';
GRANT ALL PRIVILEGES ON DATABASE rawimport TO rawimport;
```

**2. Configure** — `cp .env.example .env`, set the database DSN (`RAWIMPORT_DB_DRIVER=postgresql`, `RAWIMPORT_DB_CONNECTION=postgresql://rawimport:***@localhost:5432/rawimport`).

**3. Map volumes** in `docker-compose.yml` to your NAS paths:

```yaml
volumes:
  - /host/watch/path:/watch
  - /host/output/path:/output
  - /host/archive/path:/archive
```

**4. Run**

```bash
docker compose up -d   # published image includes dnglab; no manual downloads
```

Drop RAWs in `/watch` → DNGs appear in `/output/<schema>/IMG_{n}.dng`, originals archived to `/archive/<schema>/`, failures dead-lettered to `/archive/failed/`.

## Configuration

All via environment variables (`.env`); host paths via `docker-compose.yml` volumes:

| Variable | Default | Purpose |
|----------|---------|---------|
| `RAWIMPORT_DB_DRIVER` | `postgresql` | Database backend (`postgresql`, `mariadb`) |
| `RAWIMPORT_DB_CONNECTION` / `RAWIMPORT_DATABASE_URL` | — | Database connection string DSN |
| `RAWIMPORT_DB_SSLMODE` | `disable` | TLS mode |
| `RAWIMPORT_INPUT_DIR` / `RAWIMPORT_OUTPUT_DIR` / `RAWIMPORT_ARCHIVE_DIR` | `/watch` `/output` `/archive` | Container paths |
| `RAWIMPORT_FOLDER_SCHEMA` | `%Y/%m` | Output subfolder (strftime) |
| `RAWIMPORT_FILE_PATTERN` | `IMG_{seq}` | Output filename |
| `RAWIMPORT_CONVERTER_ENGINE` | `dnglab` | Engine name (`dnglab`, `adobedng`) |
| `RAWIMPORT_CONVERTER_ENGINE_BIN` | — | Explicit engine binary path override |
| `RAWIMPORT_APPDATA_DIR` | `/appdata` | Appdata root for engines/config |
| `RAWIMPORT_EXIFTOOL_BIN` | `exiftool` | ExifTool path |
| `RAWIMPORT_POLL_INTERVAL` | `10` | Watcher poll seconds |
| `RAWIMPORT_MAX_CONVERTER_WORKERS` | `0` | Parallel dnglab workers (0 = auto = CPU count, cap 8; forced 1 for `adobedng`) |
| `RAWIMPORT_EXIFTOOL_DAEMON` | `true` | Persistent exiftool subprocess for EXIF extraction |
| `RAWIMPORT_DEAD_LETTER_MAX_RETRIES` | `3` | Failures before a file moves to `/archive/failed/` |
| `RAWIMPORT_FAST_FINGERPRINT` | `true` | Skip full SHA-256 when size+mtime+first-4KB fingerprint is unchanged |
| `RAWIMPORT_LOG_LEVEL` / `RAWIMPORT_PORT` / `RAWIMPORT_API_TOKEN` | `info` / `8080` / — | Logging, API port, optional bearer token |
| `RAWIMPORT_GEN_THUMB_JPEG` | `false` | Sidecar thumbnail |
| `RAWIMPORT_DEF_COMPRESSION` / `RAWIMPORT_DEF_DNG_VERSION` / `RAWIMPORT_DEF_JPEG_QUALITY` / `RAWIMPORT_DEF_LINEAR` | `lossless` / `1.4` / `92` / `false` | Conversion defaults |
| `RAWIMPORT_DEF_PREVIEW_MEDIUM` / `RAWIMPORT_DEF_PREVIEW_FULL` | `1024x1024` / `4000x3000` | Preview sizes |

### Engine Override

Binary resolution order: `CONVERTER_ENGINE_BIN` → `{APPDATA_DIR}/engines/{engine}` → `/usr/local/bin/{engine}` → `PATH`. Built-in dnglab: `/usr/local/bin/dnglab`.

Custom engine: enable the appdata volume in compose, place the binary at `{appdata}/engines/{engine_name}` (`chmod +x`), set `CONVERTER_ENGINE` accordingly.

## Legacy Migration CLI

The migration CLI (`rawimport migrate --guide`) is retained as a legacy tool for moving data between database drivers. It's exposed through the hexagonal layer but not part of the primary workflow:

```bash
RAWIMPORT_DB_DRIVER=mariadb rawimport migrate --guide
```

## API

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/v1/imports` | List imports (page, limit, status) |
| `GET` | `/api/v1/imports/{sequence}` | Get record by `IMG_{n}` |
| `GET` | `/api/v1/imports/hash/{sha256}` | Lookup by source/output hash |
| `POST` | `/api/v1/imports/{sequence}/reconvert` | Re-convert with new settings |
| `GET` | `/api/v1/stats` | Counts & failure rates |
| `POST` | `/api/v1/imports/by-path/preview-updated` | Darktable callback (hash sync) |
| `GET` | `/api/v1/imports/by-source?path=` | Resolve source→DNG |
| `POST` | `/api/v1/imports/by-source/rotation-updated` | Orientation sync |
| `GET` | `/metrics` | Prometheus metrics |
| `GET` | `/health` | Liveness (process up; no DB check) |
| `GET` | `/ready` | Readiness (verifies DB connectivity) |

Re-convert body:
```json
{
  "conversion_settings": {
    "compression": "lossless",
    "version": "1.4",
    "preview_medium": "1024x1024",
    "preview_full": "4000x3000",
    "jpeg_quality": 92,
    "linear": false
  },
  "reason": "Windows thumbnail fix"
}
```

## Darktable Plugin

Install `tools/betterembeds.lua` as a Darktable Lua script, set `RAWIMPORT_API_URL` near the top (base URL, no `/api/v1` suffix).

## Build (C++)

```bash
mkdir build && cd build && cmake .. -DWITH_DNGLAB=ON -DBUILD_TESTING=ON && make -j$(nproc) && ctest
```
## License

MIT. See `LICENSE`, `NOTICE`.