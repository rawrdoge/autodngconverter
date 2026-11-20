---
id: 009
title: Reconversion lock failed event
type: task
status: open
blocked_by: []
---

## Question

Add `LogReconversionLockFailed(import_id, holder, age_seconds)` when reconvert CLI command cannot acquire the lock for an existing import record. Should it include the holding process PID/hostname? Provide lock timeout parameter.