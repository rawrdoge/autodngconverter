#!/usr/bin/env bash
# CI guard (PRD v2.1.0 §3.2): fail if any Windows-style '> nul' redirect
# appears in shell command strings under src/. POSIX requires /dev/null.
set -euo pipefail
cd "$(dirname "$0")/.."

hits=$(grep -rnE '(^|[^/])nul([^a-zA-Z0-9_]|$)' src --include='*.cpp' --include='*.h' \
       | grep -v '/dev/null' || true)

if [ -n "$hits" ]; then
  echo "ERROR: found Windows-style 'nul' redirect(s) (must be /dev/null):"
  echo "$hits"
  exit 1
fi

echo "check-no-nul: OK"