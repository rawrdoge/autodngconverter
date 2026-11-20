---
id: 014
title: Watch stability timeout event
type: task
status: open
blocked_by: []
---

## Question

Add `LogWatchStabilityTimeout(sequence_id, path, checks)` when file copy into watch directory doesn't complete within `WATCH_MAX_STABILITY_CHECKS * WATCH_STABILITY_INTERVAL_MS`. Should file be skipped or moved to failed/?