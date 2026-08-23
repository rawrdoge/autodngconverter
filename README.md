# autodngconverter

Containerized RAW→DNG pipeline. Watches a folder, converts camera RAWs (NRW, NEF, CR2, ARW) to DNG via dnglab, assigns global monotonic `IMG_{n}` sequences, records SHA-256 of source/output in MariaDB. Darktable Lua plugin re-embeds edited previews and syncs hashes.

Personal tool, not a hardened product. Use at your own risk.

## Quick Start

**1. Provision MariaDB 10.11+** (external; schema is migrated automatically on startup):

```sql
CREATE DATABASE rawimport CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'rawimport'@'%' IDENTIFIED BY 'your-password';
GRANT CREATE, ALTER, DROP, INDEX, INSERT, UPDATE, DELETE, SELECT, REFERENCES
  ON rawimport.* TO 'rawimport'@'%';
FLUSH PRIVILEGES;
```

**2. Configure** — `cp .env.example .env`, set the DB fields from step 1 (`DB_HOST`, `DB_USER`, `DB_PASSWORD`, `DB_NAME`).

**3. Map volumes** in `docker-compose.yml` to your NAS paths:

```yaml
volumes:
  - /your/watch/path:/watch
  - /your/output/path:/output
  - /your/archive/path:/archive
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
| `DB_DRIVER` | `mariadb` | Database backend |
| `DB_HOST` / `DB_PORT` / `DB_USER` / `DB_PASSWORD` / `DB_NAME` | — | MariaDB connection (**password required**) |
| `DB_SSLMODE` | `disable` | TLS mode |
| `WATCH_DIR` / `OUTPUT_DIR` / `ARCHIVE_DIR` | `/watch` `/output` `/archive` | Container paths |
| `FOLDER_SCHEMA` | `%Y/%m` | Output subfolder (strftime) |
| `FILE_PATTERN` | `IMG_{seq}` | Output filename |
| `CONVERTER_ENGINE` | `dnglab` | Engine name (`dnglab`, `adobedng`, `libraw`) |
| `CONVERTER_ENGINE_BIN` | — | Explicit engine binary path override |
| `APPDATA_DIR` | `/appdata` | Appdata root for engines/config |
| `EXIFTOOL_BIN` | `exiftool` | ExifTool path |
| `POLL_INTERVAL` | `10` | Watcher poll seconds (also accepts legacy `POLL_INTERVAL_SEC`) |
| `MAX_CONVERTER_WORKERS` | `0` | Parallel dnglab workers (0 = auto = CPU count, cap 8; forced 1 for `adobedng`) |
| `EXIFTOOL_DAEMON` | `true` | Persistent exiftool subprocess for EXIF extraction |
| `DEAD_LETTER_MAX_RETRIES` | `3` | Failures before a file moves to `/archive/failed/` |
| `FAST_FINGERPRINT` | `true` | Skip full SHA-256 when size+mtime+first-4KB fingerprint is unchanged |
| `LOG_LEVEL` / `PORT` / `API_TOKEN` | `info` / `8080` / — | Logging, API port, optional bearer token |
| `GEN_THUMB_JPEG` | `false` | Sidecar thumbnail |
| `DEF_COMPRESSION` / `DEF_DNG_VERSION` / `DEF_JPEG_QUALITY` / `DEF_LINEAR` | `lossless` / `1.4` / `92` / `false` | Conversion defaults |
| `DEF_PREVIEW_MEDIUM` / `DEF_PREVIEW_FULL` | `1024x1024` / `4000x3000` | Preview sizes |

### Engine Override

Binary resolution order: `CONVERTER_ENGINE_BIN` → `{APPDATA_DIR}/engines/{engine}` → `/usr/local/bin/{engine}` → `PATH`. Built-in dnglab: `/usr/local/bin/dnglab`.

Custom engine: enable the appdata volume in compose, place the binary at `{appdata}/engines/{engine_name}` (`chmod +x`), set `CONVERTER_ENGINE` accordingly.

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
mkdir build && cd build && cmake .. && make -j$(nproc)
```

## License

MIT. See `LICENSE`, `NOTICE`.