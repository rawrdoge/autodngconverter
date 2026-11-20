---
id: 003
title: Converter engine unavailable event
type: task
status: open
blocked_by: []
---

## Question

Add `LogConverterEngineUnavailable(engine_name, reason)` when the converter binary is not found, not executable, version mismatch, or missing dependencies. Should this fire at startup (health_check) or lazily on first convert()? What `reason` enum values cover all cases?