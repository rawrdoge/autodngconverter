---
id: 002
title: ExifTool daemon restarted event
type: task
status: open
blocked_by: []
---

## Question

Add `LogExifToolDaemonRestarted(reason, restart_count)` when the persistent ExifTool subprocess is restarted (health check fails, crash, OOM). Should this include the previous daemon's uptime? Should it correlate with in-flight extractions that were interrupted?