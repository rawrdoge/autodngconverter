---
id: 008
title: Hash verification failed event
type: task
status: open
blocked_by: []
---

## Question

Add `LogHashVerificationFailed(sequence_id, expected_hash, actual_hash, stage)` when post-conversion output DNG hash doesn't match expected or pre-computed hash. Differentiate `stage`: post_conversion, database_verify, sidecar_verify.