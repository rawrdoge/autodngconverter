---
id: 004
title: Converter unsupported format event
type: task
status: open
blocked_by: []
---

## Question

Add `LogConverterUnsupportedFormat(sequence_id, source_path, extension)` when a file's extension is not in `supported_extensions()`. Should this be a WARN (skipped) or ERROR (dead-lettered)? Where does the check live — Scan or Import?