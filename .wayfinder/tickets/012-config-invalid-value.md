---
id: 012
title: Config invalid value event
type: task
status: open
blocked_by: []
---

## Question

Add `LogConfigInvalidValue(key, value, reason)` when runtime config validation fails (e.g. invalid port, nonexistent directory, bad DB connection string). Fire before startup abort. Mask secrets (`[REDACTED]`) in logged values.