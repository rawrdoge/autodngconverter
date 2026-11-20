---
id: 006
title: Filesystem write error event
type: task
status: open
blocked_by: []
---

## Question

Add `LogFilesystemWriteError(sequence_id, path, errno, operation)` when write/rename/mkdir fails for output DNG or sidecars. Should this trigger dead-letter immediately or retry? Distinguish `operation`: write, rename, mkdir, sync.