---
id: 005
title: Filesystem read error event
type: task
status: open
blocked_by: []
---

## Question

Add `LogFilesystemReadError(sequence_id, path, errno, operation)` when open/read/stat fails during import (source file, hash computation, fingerprint). Should `operation` be an enum (open, read, stat, hash, fingerprint)? Include `errno` for programmatic handling.