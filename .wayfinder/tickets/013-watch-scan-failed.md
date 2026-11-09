---
id: 013
title: Watch scan failed event
type: task
status: open
blocked_by: []
---

## Question

Add `LogWatchScanFailed(watch_dir, error)` when watch loop directory iteration throws filesystem exception (permission denied, unmounted drive). How does watch loop recover (sleep + retry)?