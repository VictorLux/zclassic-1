#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_observability_pairing.sh -- reject new stderr diagnostics that
# are neither observable nor explicitly justified.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

if [[ "$#" -gt 0 ]]; then
    files=("$@")
elif [[ "${ZCL_OBS_SCAN_ALL:-0}" = "1" ]]; then
    mapfile -t files < <(find app lib -type f -name '*.c' \
        ! -path 'lib/test/*' | sort)
else
    base=""
    if git rev-parse --verify origin/main >/dev/null 2>&1; then
        base="$(git merge-base HEAD origin/main 2>/dev/null || true)"
    fi
    if [[ -n "$base" ]]; then
        mapfile -t files < <(git diff --name-only --diff-filter=ACMR "$base" -- app lib \
            | grep '\.c$' | grep -v '^lib/test/' || true)
    else
        mapfile -t files < <(git diff --name-only --diff-filter=ACMR -- app lib \
            | grep '\.c$' | grep -v '^lib/test/' || true)
    fi
fi

if [[ "${#files[@]}" -eq 0 ]]; then
    echo "check_observability_pairing: clean -- no changed app/lib C files"
    exit 0
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

awk '
function allowed(i, j, line) {
    if (lines[i] ~ /\/\/[[:space:]]*obs-ok:[^[:space:]]+/) return 1
    for (j = i - 3; j <= i + 3; j++) {
        if (j < 1 || j > n) continue
        line = lines[j]
        if (line ~ /event_emitf?[[:space:]]*\(/) return 1
        if (j >= i && line ~ /return[[:space:]]+(false|-1|1|NULL)[[:space:]]*;/) return 1
        if (j >= i && line ~ /(exit|abort)[[:space:]]*\(/) return 1
    }
    return 0
}
FNR == 1 {
    if (n > 0) flush_file()
    file = FILENAME
    n = 0
}
{
    lines[++n] = $0
}
function flush_file(    i) {
    for (i = 1; i <= n; i++) {
        if (lines[i] ~ /fprintf[[:space:]]*\([[:space:]]*stderr/) {
            if (!allowed(i)) {
                printf("%s:%d:%s\n", file, i, lines[i])
                bad = 1
            }
        }
    }
    delete lines
    n = 0
}
END {
    if (n > 0) flush_file()
    exit bad ? 1 : 0
}
' "${files[@]}" >"$tmp"

if [[ -s "$tmp" ]]; then
    echo "check_observability_pairing: unpaired stderr diagnostics found"
    echo
    cat "$tmp"
    echo
    echo "Pair stderr with event_emit/event_emitf, terminal propagation, or // obs-ok:<reason>."
    exit 1
fi

echo "check_observability_pairing: clean -- stderr diagnostics are observable or justified"
exit 0
