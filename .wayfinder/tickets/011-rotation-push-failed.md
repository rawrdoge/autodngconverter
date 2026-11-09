---
id: 011
title: Rotation push failed event
type: task
status: open
blocked_by: []
---

## Question

Add `LogRotationPushFailed(import_id, sink, error)` when Immich or downstream rotation sink returns HTTP error, timeout, or invalid payload. Should this retry with exponential backoff or dead-letter the notification?