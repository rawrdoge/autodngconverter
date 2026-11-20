---
id: 007
title: Filesystem archive failed event
type: task
status: open
blocked_by: []
---

## Question

Add `LogFilesystemArchiveFailed(sequence_id, source, dest, error)` when moving original to archive/ or failed/ fails. Should this block the import from completing? What happens if archive succeeds but DB update fails (orphaned file)?