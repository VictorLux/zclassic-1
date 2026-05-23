#!/usr/bin/env bash
# Gate #21: production supervisor registration must specify a domain.

set -e

HITS=$(grep -rnE '(^|[^A-Za-z0-9_])supervisor_register\s*\(' app/ lib/ config/ --include='*.c' \
    | grep -v 'supervisor_register_in_domain' \
    | grep -v 'lib/util/src/supervisor.c' \
    | grep -v 'lib/test/' \
    | grep -v '// supervisor-root-ok:' || true)

COUNT=$(printf "%s" "$HITS" | sed '/^$/d' | wc -l)
MODE="${ZCL_LINT_MODE:-FAIL}"

if [ "$COUNT" -gt 0 ]; then
    printf "%s\n" "$HITS"
    echo "[check_supervisor_domain] $COUNT violation(s) (mode: $MODE)"
    if [ "$MODE" = "FAIL" ]; then exit 1; fi
fi

echo "[check_supervisor_domain] PASS"
