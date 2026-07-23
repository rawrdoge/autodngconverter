# autodngconverter

Containerized RAW→DNG pipeline. Watches a folder, converts camera RAWs (NRW, NEF, CR2, ARW) to DNG via dnglab, assigns global monotonic `IMG_{n}` sequence, records SHA-256 of source and output in MariaDB. Darktable Lua plugin re-embeds edited previews and syncs hash.

Personal tool, not a hardened product. Use at your own risk.

## Quick Start

```bash
# Configure
cp .env.example .env
# edit .env with DB_PASSWORD, DB_ROOT_PASSWORD

# Run (C++ service, MariaDB 10.11)
docker compose up -d
```

Volumes: `/watch` (input), `/output` (DNG), `/archive` (original RAW).

## Configuration

All via environment variables (`.env`):

| Variable | Default | Purpose |
|----------|---------|---------|
| `DB_DRIVER` | `mariadb` | Database backend |
| `DB_HOST` | `mariadb` | DB host |
| `DB_PORT` | `3306` | DB port |
| `DB_USER` | `rawimport` | DB user |
| `DB_PASSWORD` | — | **Required** |
| `DB_NAME` | `rawimport` | DB name |
| `DB_SSLMODE` | `disable` | TLS mode |
| `WATCH_DIR` | `/watch` | Input directory |
| `OUTPUT_DIR` | `/output` | DNG output |
| `ARCHIVE_DIR` | `/archive` | RAW archive |
| `FOLDER_SCHEMA` | `%Y/%m` | Output subfolder (strftime) |
| `FILE_PATTERN` | `IMG_{seq}` | Output filename |
| `CONVERTER_ENGINE` | `dnglab` | Converter binary |
| `EXIFTOOL_BIN` | `exiftool` | ExifTool path |
| `POLL_INTERVAL` | `10` | Watcher poll seconds |
| `LOG_LEVEL` | `info` | Log verbosity |
| `PORT` | `8080` | API port |
| `API_TOKEN` | — | Optional bearer token |
| `GEN_THUMB_JPEG` | `false` | Sidecar thumbnail |
| `DEF_COMPRESSION` | `lossless` | RAW compression |
| `DEF_DNG_VERSION` | `1.4` | DNG spec version |
| `DEF_PREVIEW_MEDIUM` | `1024x1024` | Medium preview |
| `DEF_PREVIEW_FULL` | `4000x3000` | Full preview |
| `DEF_JPEG_QUALITY` | `92` | JPEG quality |
| `DEF_LINEAR` | `false` | Linear DNG flag |

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
| `GET` | `/health` | Liveness/readiness |

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

Install `betterembeds.lua`, set `RAWIMPORT_API_URL` at top of script.

## Build (C++)

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
```

## Versioning

- **v2.x** (main): C++, MariaDB 10.11, `libmariadb`
- **v1.x** (tags `v1.0.0`, `v1.0.1`, `v1.0.3`): Go, MySQL 8.0 only

Legacy Go service frozen at v1.0.3. MariaDB unsupported in Go (pure-Go driver protocol incompatibility).

## License

MIT. See `LICENSE`, `NOTICE`.