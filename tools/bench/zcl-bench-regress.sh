#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Fails when the newest numeric row for a primary benchmark regresses more than
# a threshold versus the previous numeric row for the same benchmark.

set -euo pipefail

CSV="${ZCL_BENCH_HISTORY:-docs/bench-history.csv}"
THRESHOLD="${ZCL_BENCH_REGRESSION_THRESHOLD:-20}"

if [ ! -f "$CSV" ]; then
    echo "[bench-regress] no history at $CSV; skipping"
    exit 0
fi

python3 - "$CSV" "$THRESHOLD" <<'PY'
import csv
import math
import sys

path = sys.argv[1]
threshold = float(sys.argv[2])
rows_by_bench = {}

with open(path, newline="") as f:
    filtered = (line for line in f if not line.startswith("#"))
    for row in csv.DictReader(filtered):
        bench = (row.get("bench") or "").strip()
        value_text = (row.get("value") or "").strip()
        if not bench or not value_text:
            continue
        try:
            value = float(value_text)
        except ValueError:
            continue
        if not math.isfinite(value):
            continue
        rows_by_bench.setdefault(bench, []).append((value, row))

failed = False
for bench, values in sorted(rows_by_bench.items()):
    if len(values) < 2:
        continue
    previous, prev_row = values[-2]
    current, cur_row = values[-1]
    if previous <= 0:
        continue
    pct = ((current - previous) / previous) * 100.0
    if pct > threshold:
        failed = True
        print(
            f"[bench-regress] FAIL {bench}: {previous:g} -> {current:g} "
            f"({pct:.1f}% > {threshold:g}%)"
        )
        print(f"  previous: {prev_row}")
        print(f"  current:  {cur_row}")

if failed:
    sys.exit(1)

print(f"[bench-regress] OK: no numeric primary regressed > {threshold:g}%")
PY
