#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_raw_sqlite.sh - ensure code outside vendored/test paths does not use
# raw sqlite3_step() outside the AR_* wrappers (activerecord.h).
#
# Scans app/, tools/, and lib/ for `sqlite3_step(` outside:
#   - vendor/
#   - any test/ directory
#   - the AR_STEP_ROW / AR_STEP_DONE / AR_STEP_ROW_READONLY macros themselves
#     (which textually contain `sqlite3_step` inside their #define bodies)
#   - lines annotated with `// raw-sql-ok: <reason>`
#
# Files listed in tools/scripts/raw_sqlite_allowlist.txt are grandfathered
# during the Wave 3 migration (sqlite3_step → AR_BEGIN_SAVE). The list is a
# ratchet: entries come off as each subsystem completes migration. Once
# empty, the allowlist is removed and the lint becomes unconditional.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

ALLOWLIST="$SCRIPT_DIR/raw_sqlite_allowlist.txt"

declare -A ALLOWED=()
if [[ -f "$ALLOWLIST" ]]; then
    while IFS= read -r line; do
        # strip comments and blanks
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -z "$line" ]] && continue
        ALLOWED["$line"]=1
    done < "$ALLOWLIST"
fi

raw_hits=$(grep -rn 'sqlite3_step\s*(' app/ tools/ lib/ --include='*.c' 2>/dev/null \
    | grep -v 'vendor/\|/test/\|// raw-sql-ok\|AR_STEP_ROW\|AR_STEP_DONE\|AR_STEP_ROW_READONLY\|safe_alloc\|".*sqlite3_step' \
    || true)

violations=""
allowed_total=0
while IFS= read -r hit; do
    [[ -z "$hit" ]] && continue
    path="${hit%%:*}"
    if [[ -n "${ALLOWED[$path]:-}" ]]; then
        allowed_total=$((allowed_total + 1))
        continue
    fi
    violations="${violations}${hit}"$'\n'
done <<< "$raw_hits"

if [[ -n "${violations//[[:space:]]/}" ]]; then
    echo "$violations"
    echo "FAIL: raw sqlite3_step in production code"
    echo "  Use AR_STEP_ROW / AR_STEP_DONE / AR_STEP_ROW_READONLY (see"
    echo "  app/models/include/models/activerecord.h), wrap in AR_BEGIN_SAVE /"
    echo "  AR_EXEC_BOOL, or — for unavoidable cases like schema bootstrap —"
    echo "  add a // raw-sql-ok: <reason> comment on the line."
    echo "  Allowlisted files (still pending Wave 3 migration) accounted for:"
    echo "    $allowed_total raw call sites across $(wc -l < <(grep -v '^[[:space:]]*#\|^[[:space:]]*$' "$ALLOWLIST" 2>/dev/null || true)) files"
    exit 1
fi

if (( allowed_total > 0 )); then
    file_count=$(grep -cv '^[[:space:]]*#\|^[[:space:]]*$' "$ALLOWLIST" 2>/dev/null || echo 0)
    echo "check_raw_sqlite: clean outside allowlist"
    echo "  Allowlisted: $allowed_total raw call sites across $file_count files"
    echo "  (drives to zero as Wave 3 sqlite3_step → AR_BEGIN_SAVE migration lands)"
else
    echo "check_raw_sqlite: clean - no raw sqlite3_step in production code"
fi
exit 0
