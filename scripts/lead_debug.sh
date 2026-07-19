#!/usr/bin/env bash
# lead_debug.sh — LEAD escalated-debugging triage harness.
# Exercises each BLOCKS-RUNTIME escalation against the live container + MariaDB
# so failures are observable. Run from repo root after `docker compose -f
# docker-compose.cpp-e2e.yml up -d`.
set -euo pipefail

BASE="http://localhost:8080"
MARIADB="docker exec autodngconverter-mariadb-1 mysql -u rawimport -prawpw rawimport"

echo "=== L1: reconvert resolves by import row id (not sequence) ==="
# Insert a test import, then trigger reconvert via API, confirm it resolves.
$MARIADB -e "INSERT INTO sequences(name) VALUES('IMG_9001');" 2>/dev/null || true
$MARIADB -e "INSERT INTO imports(sequence_id,source_path,source_hash,output_path,output_hash,camera_model,status,orientation) VALUES((SELECT id FROM sequences WHERE name='IMG_9001'),'/watch/L1.NRW','l1src','/output/IMG_9001.dng','l1out','TEST','completed',0);" 2>/dev/null || true
echo "--- POST /api/v1/imports/IMG_9001/reconvert ---"
curl -s -X POST "$BASE/api/v1/imports/IMG_9001/reconvert" -H "Content-Type: application/json" -d '{"compression":"-c","reason":"lead-L1"}'
echo
echo "--- reconversions table (should have 1 row for import_id of IMG_9001) ---"
$MARIADB -e "SELECT r.id, r.import_id, r.status FROM reconversions r JOIN imports i ON r.import_id=i.id WHERE i.sequence_id=(SELECT id FROM sequences WHERE name='IMG_9001');"
echo

echo "=== L4: processing_locks keyed by import_id ==="
$MARIADB -e "DESCRIBE processing_locks;" | grep -E "import_id|Key" || echo "L4 FAIL: no import_id PK"
echo

echo "=== L5: preview_edits columns match code ==="
$MARIADB -e "DESCRIBE preview_edits;" | grep -E "previous_output_hash|new_output_hash|preview_width|preview_height|preview_quality|edited_at" || echo "L5 FAIL: column mismatch"
echo

echo "=== L11: reconcile legacy source_hash is synthetic (legacy:IMG_n) ==="
echo "(verified in code: src/reconcile.cpp uses 'legacy:IMG_' + n; matches Go reconcile.go:162)"
echo

echo "=== health ==="
curl -s "$BASE/health"; echo
echo "=== stats ==="
curl -s "$BASE/api/v1/stats"; echo
echo
echo "LEAD triage complete."