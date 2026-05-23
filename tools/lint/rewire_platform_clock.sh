#!/usr/bin/env bash
# Mechanical helper for Phase 1 platform.clock/platform.rng rewires.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

files=()
while IFS= read -r -d '' file; do
    [[ "$file" == lib/platform/* ]] && continue
    [[ "$file" == app/conditions/* ]] && continue
    [[ "$file" == app/jobs/* ]] && continue
    [[ "$file" == app/supervisors/* ]] && continue
    [[ "$file" == lib/framework/* ]] && continue
    [[ "$file" == app/services/src/header_admit_stage.c ]] && continue
    [[ "$file" == app/services/src/header_probe_service.c ]] && continue
    files+=("$file")
done < <(find app lib config tools -type f \( -name '*.c' -o -name '*.h' \) -print0 2>/dev/null)

for file in "${files[@]}"; do
    before="$(mktemp)"
    cp "$file" "$before"

    perl -0pi -e '
        s/\bclock_gettime\s*\(\s*CLOCK_MONOTONIC\s*,\s*([^)]+)\)/platform_time_monotonic_timespec($1)/g;
        s/\bclock_gettime\s*\(\s*CLOCK_REALTIME\s*,\s*([^)]+)\)/platform_time_realtime_timespec($1)/g;
        s/\btime\s*\(\s*NULL\s*\)/platform_time_wall_time_t()/g;
    ' "$file"

    if ! cmp -s "$before" "$file"; then
        if ! grep -q '#include "platform/time_compat.h"' "$file"; then
            awk '
                BEGIN { inserted = 0 }
                /^#include / && !inserted {
                    print "#include \"platform/time_compat.h\"";
                    inserted = 1;
                }
                { print }
                END {
                    if (!inserted)
                        print "#include \"platform/time_compat.h\"";
                }
            ' "$file" > "$file.tmp"
            mv "$file.tmp" "$file"
        fi
    fi

    rm -f "$before"
done
