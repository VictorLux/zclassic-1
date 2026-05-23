#!/usr/bin/env bash
# Gate #20: controllers should not prepare/exec SQLite directly.
# Mode: WARN | FAIL (controlled by ZCL_LINT_MODE; default WARN for Phase 0)
set -euo pipefail

MODE="${ZCL_LINT_MODE:-WARN}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$ROOT"

roots=()
for root in app/controllers tools/mcp/controllers; do
    [[ -d "$root" ]] && roots+=("$root")
done

matches=$(
    grep -rn --include='*.c' --include='*.h' \
        -E '\bsqlite3_prepare_v2\s*\(|\bsqlite3_exec\s*\(' \
        "${roots[@]}" 2>/dev/null \
    | grep -v '// raw-controller-sql-ok' \
    || true
)

violations=0
if [[ -n "${matches//[[:space:]]/}" ]]; then
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        violations=$((violations + 1))
        echo "$line" >&2
    done <<< "$matches"
fi

echo "[check_no_raw_sqlite_in_controllers] $violations violation(s) found (mode: $MODE)"
echo "[check_no_raw_sqlite_in_controllers] use projection_* or models, or add // raw-controller-sql-ok for a documented exception"

if (( violations > 0 )) && [[ "$MODE" == "FAIL" ]]; then
    exit 1
fi
exit 0
