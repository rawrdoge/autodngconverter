# autodngconverter

Containerized RAW→DNG pipeline. Watches a folder, converts camera RAWs (NRW, NEF, CR2, ARW) to DNG via dnglab, assigns global monotonic `IMG_{n}` sequence, records SHA-256 of source and output in MariaDB. Darktable Lua plugin re-embeds edited previews and syncs hash.

Personal tool, not a hardened product. Use at your own risk.

## Quick Start

```bash
# Configure
cp .env.example .env
# edit .env with your NAS paths & DB passwords

# Run (published image, MariaDB 10.11)
docker compose up -d
```

The compose file mounts host directories via variables in `.env`:
- `WATCH_HOST` → `/watch` (input RAWs)
- `OUTPUT_HOST` → `/output` (DNGs)
- `ARCHIVE_HOST` → `/archive` (original RAWs)

Defaults: `/mnt/nas/photos/{watch,output,archive}`

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
| `DB_ROOT_PASSWORD` | — | **Required** for MariaDB init |
| `WATCH_HOST` | `/mnt/nas/photos/watch` | Host input directory |
| `OUTPUT_HOST` | `/mnt/nas/photos/output` | Host DNG output |
| `ARCHIVE_HOST` | `/mnt/nas/photos/archive` | Host RAW archive |
| `WATCH_DIR` | `/watch` | Container input |
| `OUTPUT_DIR` | `/output` | Container DNG output |
| `ARCHIVE_DIR` | `/archive` | Container RAW archive |
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

## License

MIT. See `LICENSE`, `NOTICE`.
