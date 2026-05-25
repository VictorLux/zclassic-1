#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# zcl-bench.sh -- one entry point for the five user-facing performance
# benchmarks. Defaults are safe for a developer workstation: use an isolated
# datadir and ports, never mutate the live systemd service, and mark full-chain
# measurements pending when their prerequisites are unavailable.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

HISTORY="${ZCL_BENCH_HISTORY:-docs/bench-history.csv}"
TMP_ROOT="${ZCL_BENCH_DIR:-/tmp/zcl23-bench-${USER:-user}}"
RPC_PORT="${ZCL_BENCH_RPC_PORT:-28232}"
P2P_PORT="${ZCL_BENCH_P2P_PORT:-28033}"
RSS_SECS="${ZCL_BENCH_RSS_SECS:-10}"
RECOVERY_ITERS="${ZCL_BENCH_RECOVERY_ITERS:-1}"
RUN_COLD="${ZCL_BENCH_RUN_COLD:-0}"
SOURCE_DATADIR="${ZCL_BENCH_SOURCE_DATADIR:-}"
LEGACY_DATADIR="${ZCL_BENCH_LEGACY_DATADIR:-$HOME/.zclassic}"
KEEP="${ZCL_BENCH_KEEP:-0}"

DATE="$(date -u +%FT%TZ)"
COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
BENCH_DATADIR="$TMP_ROOT/node"

json_result_int() {
    python3 -c '
import json, sys
text = sys.stdin.read().strip()
try:
    value = json.loads(text)
    value = value.get("result", value)
    print(int(value) if isinstance(value, int) else "")
except Exception:
    print("")
'
}

live_height() {
    ZCL_DATADIR="${ZCL_LIVE_DATADIR:-$HOME/.zclassic-c23}" \
        ZCL_RPCPORT="${ZCL_LIVE_RPCPORT:-18232}" \
        ./tools/zcl-rpc getblockcount 2>/dev/null | json_result_int || true
}

csv_escape() {
    python3 - "$1" <<'PY'
import csv, io, sys
buf = io.StringIO()
csv.writer(buf).writerow([sys.argv[1]])
print(buf.getvalue().strip())
PY
}

append_row() {
    local bench="$1" value="$2" unit="$3" notes="$4"
    printf '%s,%s,%s,%s,%s,%s\n' \
        "$(csv_escape "$DATE")" \
        "$(csv_escape "$COMMIT")" \
        "$(csv_escape "$bench")" \
        "$(csv_escape "$value")" \
        "$(csv_escape "$unit")" \
        "$(csv_escape "$notes")" >> "$HISTORY"
}

print_row() {
    printf '  %-31s %10s %-8s %s\n' "$1" "${2:---}" "$3" "$4"
}

ensure_history() {
    if [ -f "$HISTORY" ]; then
        return
    fi
    cat > "$HISTORY" <<'EOF'
# zclassic23 benchmark history.
# Columns: date, commit, bench, value, unit, notes.
# Numeric value rows are regression-gated by tools/bench/zcl-bench-regress.sh.
# Empty value rows are pending/skipped measurements and are ignored by the gate.
# Bench names follow docs/USER_BENCHMARKS.md primaries #1..#5.
# Append-only: do not rewrite old rows; add a correction row instead.
date,commit,bench,value,unit,notes
EOF
}

require_binary() {
    if [ ! -x ./zclassic23 ]; then
        echo "FAIL: ./zclassic23 not built; run make zclassic23" >&2
        exit 2
    fi
    if [ ! -x ./tools/zcl-rpc ]; then
        echo "FAIL: ./tools/zcl-rpc missing or not executable" >&2
        exit 2
    fi
}

cleanup_pid() {
    local pid="${1:-}"
    if [ -n "$pid" ]; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

wait_for_rpc() {
    local datadir="$1" port="$2" deadline="$3"
    local end=$(( $(date +%s) + deadline ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if ZCL_DATADIR="$datadir" ZCL_RPCPORT="$port" \
            ./tools/zcl-rpc getblockcount >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_isolated_node() {
    local datadir="$1" rpc_port="$2" p2p_port="$3"
    mkdir -p "$datadir"
    ./zclassic23 \
        -datadir="$datadir" \
        -rpcport="$rpc_port" \
        -port="$p2p_port" \
        -listen=0 \
        -connect=127.0.0.1:9 \
        -allow-degraded \
        -nobgvalidation \
        -nofilesync \
        -showmetrics=0 \
        > "$datadir/node.stdout" 2> "$datadir/node.stderr" &
    echo "$!"
}

bench_cold() {
    if [ "$RUN_COLD" != "1" ]; then
        append_row "#1 cold-start to operational" "" "s" "pending P0/full-chain run; set ZCL_BENCH_RUN_COLD=1 to run cold import against $LEGACY_DATADIR"
        print_row "#1 cold-start" "--" "s" "pending P0/full-chain run"
        return
    fi
    if [ ! -d "$LEGACY_DATADIR/blocks" ]; then
        append_row "#1 cold-start to operational" "" "s" "skipped: legacy blocks dir missing at $LEGACY_DATADIR"
        print_row "#1 cold-start" "--" "s" "skipped: no legacy blocks"
        return
    fi
    local log="$TMP_ROOT/cold.log"
    local start end elapsed
    start=$(date +%s)
    ZCL_BENCH_TMP="$TMP_ROOT/cold" \
        ZCL_BENCH_RPC_PORT="$((RPC_PORT + 10))" \
        ZCL_BENCH_P2P_PORT="$((P2P_PORT + 10))" \
        ZCL_BENCH_LEGACY_DATADIR="$LEGACY_DATADIR" \
        bash tools/bench_cold_start_from_legacy.sh > "$log" 2>&1
    end=$(date +%s)
    elapsed=$((end - start))
    append_row "#1 cold-start to operational" "$elapsed" "s" "tools/bench_cold_start_from_legacy.sh; log=$log"
    print_row "#1 cold-start" "$elapsed" "s" "cold import"
}

bench_warm() {
    if [ -z "$SOURCE_DATADIR" ] || [ ! -d "$SOURCE_DATADIR" ]; then
        append_row "#2 warm-start to operational" "" "s" "pending: set ZCL_BENCH_SOURCE_DATADIR to a synced seed datadir"
        print_row "#2 warm-start" "--" "s" "pending seed datadir"
        return
    fi
    local datadir="$TMP_ROOT/warm"
    rm -rf "$datadir"
    mkdir -p "$datadir"
    cp -a "$SOURCE_DATADIR/." "$datadir/"
    local start pid elapsed
    start=$(date +%s)
    pid=$(start_isolated_node "$datadir" "$((RPC_PORT + 20))" "$((P2P_PORT + 20))")
    if ! wait_for_rpc "$datadir" "$((RPC_PORT + 20))" 120; then
        cleanup_pid "$pid"
        append_row "#2 warm-start to operational" "" "s" "failed: RPC not ready within 120s; datadir=$datadir"
        print_row "#2 warm-start" "--" "s" "failed RPC deadline"
        return 1
    fi
    elapsed=$(( $(date +%s) - start ))
    cleanup_pid "$pid"
    append_row "#2 warm-start to operational" "$elapsed" "s" "isolated copy of $SOURCE_DATADIR"
    print_row "#2 warm-start" "$elapsed" "s" "isolated copy"
}

bench_mtbf() {
    append_row "#3 stay-in-sync MTBF" "" "s" "pending soak: requires healthy source peer after P0 deploy"
    print_row "#3 stay-in-sync MTBF" "--" "s" "pending soak"
}

bench_rss() {
    if [ -z "$SOURCE_DATADIR" ] || [ ! -d "$SOURCE_DATADIR" ]; then
        append_row "#4 RAM steady-state" "" "MB" "pending: set ZCL_BENCH_SOURCE_DATADIR to a seeded isolated datadir"
        print_row "#4 RSS" "--" "MB" "pending seed datadir"
        return
    fi
    local datadir="$BENCH_DATADIR-rss"
    rm -rf "$datadir"
    mkdir -p "$datadir"
    cp -a "$SOURCE_DATADIR/." "$datadir/"
    local pid rss_kb rss_mb
    pid=$(start_isolated_node "$datadir" "$((RPC_PORT + 30))" "$((P2P_PORT + 30))")
    if ! wait_for_rpc "$datadir" "$((RPC_PORT + 30))" 60; then
        cleanup_pid "$pid"
        append_row "#4 RAM steady-state" "" "MB" "pending: isolated empty datadir did not reach RPC; provide ZCL_BENCH_SOURCE_DATADIR for a seeded run"
        print_row "#4 RSS" "--" "MB" "pending seeded datadir"
        return
    fi
    sleep "$RSS_SECS"
    rss_kb=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || echo "")
    cleanup_pid "$pid"
    if [ -z "$rss_kb" ]; then
        append_row "#4 RAM steady-state" "" "MB" "failed: VmRSS unavailable for isolated node"
        print_row "#4 RSS" "--" "MB" "VmRSS unavailable"
        return
    fi
    rss_mb=$(awk -v kb="$rss_kb" 'BEGIN { printf "%.1f", kb / 1024 }')
    append_row "#4 RAM steady-state" "$rss_mb" "MB" "isolated node RSS after ${RSS_SECS}s; datadir=$datadir"
    print_row "#4 RSS" "$rss_mb" "MB" "isolated ${RSS_SECS}s sample"
}

bench_recovery() {
    if [ -z "$SOURCE_DATADIR" ] || [ ! -d "$SOURCE_DATADIR" ]; then
        append_row "#5 kill-9 recovery" "" "s" "pending: set ZCL_BENCH_SOURCE_DATADIR to a seeded isolated datadir"
        print_row "#5 kill-9 recovery" "--" "s" "pending seed datadir"
        return
    fi
    local datadir="$BENCH_DATADIR-recovery"
    rm -rf "$datadir"
    mkdir -p "$datadir"
    cp -a "$SOURCE_DATADIR/." "$datadir/"
    local total=0 best="" worst=0 i pid start elapsed port p2p
    for i in $(seq 1 "$RECOVERY_ITERS"); do
        port=$((RPC_PORT + 40 + i))
        p2p=$((P2P_PORT + 40 + i))
        pid=$(start_isolated_node "$datadir" "$port" "$p2p")
        if ! wait_for_rpc "$datadir" "$port" 60; then
            cleanup_pid "$pid"
            append_row "#5 kill-9 recovery" "" "s" "pending: isolated empty datadir did not reach RPC; provide ZCL_BENCH_SOURCE_DATADIR for a seeded run"
            print_row "#5 kill-9 recovery" "--" "s" "pending seeded datadir"
            return
        fi
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        start=$(date +%s)
        pid=$(start_isolated_node "$datadir" "$port" "$p2p")
        if ! wait_for_rpc "$datadir" "$port" 60; then
            cleanup_pid "$pid"
            append_row "#5 kill-9 recovery" "" "s" "failed: RPC not ready after kill -9 within 60s; datadir=$datadir"
            print_row "#5 kill-9 recovery" "--" "s" "failed RPC deadline"
            return
        fi
        elapsed=$(( $(date +%s) - start ))
        cleanup_pid "$pid"
        total=$((total + elapsed))
        [ -z "$best" ] || [ "$elapsed" -lt "$best" ] && best="$elapsed"
        [ "$elapsed" -gt "$worst" ] && worst="$elapsed"
    done
    local avg
    avg=$(awk -v total="$total" -v n="$RECOVERY_ITERS" 'BEGIN { printf "%.1f", total / n }')
    append_row "#5 kill-9 recovery" "$avg" "s" "isolated RPC-ready recovery; iterations=$RECOVERY_ITERS best=${best}s worst=${worst}s"
    print_row "#5 kill-9 recovery" "$avg" "s" "isolated RPC-ready avg"
}

main() {
    require_binary
    ensure_history
    rm -rf "$TMP_ROOT"
    mkdir -p "$TMP_ROOT"

    local live_before live_after
    live_before="$(live_height)"

    echo "zclassic23 benchmark harness"
    echo "  commit:        $COMMIT"
    echo "  history:       $HISTORY"
    echo "  bench datadir: $TMP_ROOT"
    echo "  bench ports:   rpc=$RPC_PORT p2p=$P2P_PORT"
    echo "  live height before: ${live_before:-unavailable}"
    echo

    bench_cold
    bench_warm
    bench_mtbf
    bench_rss
    bench_recovery

    live_after="$(live_height)"
    echo
    echo "  live height after:  ${live_after:-unavailable}"
    if [ -n "${live_before:-}" ] && [ -n "${live_after:-}" ]; then
        echo "  live height delta:  $((live_after - live_before))"
    fi

    if [ "$KEEP" != "1" ]; then
        rm -rf "$TMP_ROOT"
    else
        echo "  kept bench datadir: $TMP_ROOT"
    fi
}

main "$@"
