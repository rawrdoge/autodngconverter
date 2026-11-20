## Destination

Extend the LogSink event taxonomy with 14+ new structured logging events covering ExifTool, Converter, File System, Hash, Reconversion, Rotation, Config, and Watch failures — so operators can diagnose every failure mode without reading source code.

## Notes

- Domain: `autodngconverter` (hexagonal C++20 RAW→DNG pipeline)
- Skills: `code-quality-principles` (KISS/YAGNI/SOLID), `wayfinder` (this map)
- Standing preferences: strict information density, zero embellishment, version bump on commit, `RAWIMPORT_` prefix for all config
- Current taxonomy: 13 events in `include/domain/ports/log_sink.hpp` (collision, conversion, import, dead_letter, watch, db)
- Each new event = pure virtual in LogSink + impl in SpdlogLogSink + no-op in NoopLogSink/MockLogSink + call site(s) in use_cases/cli

## Decisions so far

<!-- populated as tickets resolve -->

## Not yet specified

- ExifTool: extraction failures, daemon restarts
- Converter: engine unavailable, unsupported format
- File System: read/write/archive errors
- Hash: verification failures
- Reconversion: lock failures, source missing
- Rotation: push failures
- Config: invalid values at runtime
- Watch: scan failures, stability timeouts

## Out of scope

- Metrics/Prometheus counters (separate sink)
- Alerting/webhooks (separate sink)
- Distributed tracing (correlation IDs already exist)