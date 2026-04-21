#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_raw_sqlite.sh - ensure app code does not introduce raw sqlite3_step
# calls outside approved wrappers and documented exceptions.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

hits=$(grep -rn 'sqlite3_step\s*(' app/ tools/ --include='*.c' 2>/dev/null \
    | grep -v 'vendor/\|test/\|// raw-sql-ok\|AR_STEP_ROW\|AR_STEP_DONE\|AR_STEP_ROW_READONLY\|safe_alloc\|".*sqlite3_step' \
    || true)

if [[ -n "$hits" ]]; then
    echo "$hits"
    echo "FAIL: raw sqlite3_step in app code (use AR_STEP_ROW/AR_STEP_DONE/AR_STEP_ROW_READONLY or mark // raw-sql-ok: <scope>)"
    exit 1
fi

echo "check_raw_sqlite: clean - no raw sqlite3_step"
exit 0
