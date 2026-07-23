# Legacy Go Service (v1.x, frozen at v1.0.3)

This directory contains the **legacy Go implementation** of autodngconverter, frozen at **v1.0.3**.

## Status

**DEPRECATED** — The Go service is no longer the primary implementation. As of v2.0.0, the **C++ service** (at repository root) is the primary implementation for MariaDB.

The Go service remains available for MySQL 8.0 users but **will not receive new features or MariaDB support**.

### Why Go is legacy

- **MariaDB incompatibility**: The pure-Go MySQL driver (`go-sql-driver/mysql`) cannot frame MariaDB 10.11 protocol responses over container TCP. Every query stalls indefinitely. This was proven via C++ parity test (same network, same DB, C++ works, Go stalls).
- **No official MariaDB Go driver**: `github.com/mariadb/go-mariadb` does not exist as a public module (404 on GitHub, GitLab, Go proxy).
- **C++ service validated**: The C++ service uses `libmariadb` (C connector) and works correctly against MariaDB 10.11.

## Container image

```
ghcr.io/rawrdoge/autodngconverter:v1.0.3    # Go legacy (MySQL-only, frozen)
```

**No `latest` tag** — use explicit version `v1.0.3`.

## Running with Docker (MySQL only)

Use the `docker-compose.go-e2e.yml` in this directory:

```bash
cd legacy/go
docker compose -f docker-compose.go-e2e.yml up -d
```

This pulls `ghcr.io/rawrdoge/autodngconverter:v1.0.3` and runs against `mysql:8.0`.

## Building without Docker

```bash
cd legacy/go
go build -o rawimport-pipeline .
```

## Environment variables

Same as the main README, but with these defaults for Go legacy:

| Variable | Default (Go legacy) |
|----------|---------------------|
| `DB_DRIVER` | `mysql` |
| `DB_HOST` | `mysql` |

## API

Identical to the C++ service — see main [README.md](../README.md#commands-and-endpoints).

## Migration to C++ service

If you need MariaDB support or want the actively maintained implementation:

1. Switch to the C++ service at repository root (`main` branch)
2. Use `docker-compose.yml` (MariaDB 10.11)
3. Image: `ghcr.io/rawrdoge/autodngconverter-cpp:latest`

The database schema is compatible (MariaDB/MySQL). The `processing_locks` table in the C++ service uses `import_id` as primary key instead of `source_hash` — see `docs/ORCHESTRATION.md` for details.

## History

- **v1.0.3** — Final Go release: metrics bounds fix, MySQL-only declaration, NULL-safe timestamps
- **v1.0.1** — Rotation/orientation sync + Prometheus /metrics
- **v1.0.0** — First tagged Go release

## License

MIT — see root [LICENSE](../LICENSE) and [NOTICE](../NOTICE).