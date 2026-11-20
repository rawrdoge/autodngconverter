---
id: 001
title: ExifTool extraction failure event
type: task
status: open
blocked_by: []
---

## Question

Add `LogExifExtractionFailed(sequence_id, source_path, error, daemon_restarted)` to LogSink for when ExifTool crashes, returns non-zero, or times out during metadata extraction. Should this fire on every failure or only after daemon restart? What fields distinguish transient vs permanent failures?