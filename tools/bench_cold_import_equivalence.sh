#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# bench_cold_import_equivalence.sh -- PR-3 acceptance harness.
#
# Runs two fresh -cold-import boots from the same legacy datadir:
#   1. serial baseline with ZCL_BLOCK_SCAN_WORKERS=1
#   2. parallel scan with the default worker policy, or an explicit
#      ZCL_BENCH_PARALLEL_WORKERS value
#
# It then compares tip height, tip hash, UTXO SHA3 commitment, and UTXO count.
# A mismatch means the parallel blk*.dat marking path is not byte-equivalent to
# the serial path and must not be treated as accepted.
#
# Env knobs:
#   ZCL_BENCH_LEGACY_DATADIR    source zclassicd datadir (default: $HOME/.zclassic)
#   ZCL_BENCH_TMP               tmp root (default: /tmp/zcl-bench-cold-equiv)
#   ZCL_BENCH_SERIAL_DIR        serial datadir override
#   ZCL_BENCH_PARALLEL_DIR      parallel datadir override
#   ZCL_BENCH_RPC_PORT_BASE     serial RPC port; parallel uses +1 (default: 18433)
#   ZCL_BENCH_P2P_PORT_BASE     serial P2P port; parallel uses +1 (default: 8044)
#   ZCL_BENCH_MIN_HEIGHT        acceptance height floor (default: 3000000)
#   ZCL_BENCH_MAX_SECS          per-run ceiling (default: 900)
#   ZCL_BENCH_PARALLEL_WORKERS  explicit parallel worker count; unset uses default
#   ZCL_BENCH_KEEP              keep tmp datadirs after run (default: no)

set -euo pipefail

LEGACY="${ZCL_BENCH_LEGACY_DATADIR:-$HOME/.zclassic}"
TMP_ROOT="${ZCL_BENCH_TMP:-/tmp/zcl-bench-cold-equiv}"
SERIAL_DIR="${ZCL_BENCH_SERIAL_DIR:-$TMP_ROOT/serial}"
PARALLEL_DIR="${ZCL_BENCH_PARALLEL_DIR:-$TMP_ROOT/parallel}"
RPC_BASE="${ZCL_BENCH_RPC_PORT_BASE:-18433}"
P2P_BASE="${ZCL_BENCH_P2P_PORT_BASE:-8044}"
MIN_HEIGHT="${ZCL_BENCH_MIN_HEIGHT:-3000000}"
MAX_SECS="${ZCL_BENCH_MAX_SECS:-900}"
PARALLEL_WORKERS="${ZCL_BENCH_PARALLEL_WORKERS:-}"
KEEP="${ZCL_BENCH_KEEP:-}"

if [ ! -d "$LEGACY/blocks" ]; then
    echo "FAIL: legacy datadir $LEGACY/blocks not found" >&2
    exit 2
fi
if [ ! -x "./zclassic23" ]; then
    echo "FAIL: ./zclassic23 not built (run make zclassic23)" >&2
    exit 2
fi
if [ ! -x "./tools/zcl-rpc" ]; then
    echo "FAIL: ./tools/zcl-rpc not built (run make zcl-rpc)" >&2
    exit 2
fi

json_result_field() {
    local field="$1"
    python3 -c '
import json
import sys

field = sys.argv[1]
text = sys.stdin.read().strip()
try:
    value = json.loads(text)
except Exception:
    print(text)
    raise SystemExit(0)

if isinstance(value, dict) and "result" in value:
    value = value["result"]
if field:
    if isinstance(value, dict):
        value = value.get(field, "")
    else:
        value = ""
print(value if value is not None else "")
' "$field"
}

rpc_call() {
    local datadir="$1"
    local port="$2"
    shift 2
    ZCL_DATADIR="$datadir" ZCL_RPCPORT="$port" ./tools/zcl-rpc "$@"
}

wait_for_height() {
    local datadir="$1"
    local port="$2"
    local start_ns="$3"
    local height=0

    while :; do
        local now_ns elapsed_ms raw
        now_ns=$(date +%s%N)
        elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))
        if [ "$elapsed_ms" -ge "$((MAX_SECS * 1000))" ]; then
            echo
            echo "FAIL: ${MAX_SECS}s elapsed for $datadir, height=$height" >&2
            tail -40 "$datadir/node.stderr" >&2 || true
            return 1
        fi

        raw=$(rpc_call "$datadir" "$port" getblockcount 2>/dev/null || true)
        height=$(printf '%s' "$raw" | json_result_field "")
        case "$height" in
            ''|*[!0-9]*) height=0 ;;
        esac
        printf "\r  elapsed=%5.1fs height=%d  " \
            "$(awk "BEGIN{print $elapsed_ms/1000}")" "$height"
        if [ "$height" -ge "$MIN_HEIGHT" ]; then
            echo
            return 0
        fi
        sleep 1
    done
}

extract_scan_line() {
    local datadir="$1"
    grep -h "Block file scan:" "$datadir/node.stdout" "$datadir/node.stderr" \
        2>/dev/null | tail -1 || true
}

run_case() {
    local label="$1"
    local workers="$2"
    local datadir="$3"
    local rpc_port="$4"
    local p2p_port="$5"

    rm -rf "$datadir"
    mkdir -p "$datadir"

    echo
    echo "== $label =="
    echo "  datadir: $datadir"
    echo "  workers: ${workers:-default}"
    echo "  rpc_port: $rpc_port"

    local start_ns pid
    start_ns=$(date +%s%N)
    if [ -n "$workers" ]; then
        ZCL_BLOCK_SCAN_WORKERS="$workers" ./zclassic23 \
            -datadir="$datadir" \
            -cold-import="$LEGACY" \
            -nofilesync \
            -rpcport="$rpc_port" \
            -port="$p2p_port" \
            -nobgvalidation \
            > "$datadir/node.stdout" 2> "$datadir/node.stderr" &
    else
        ./zclassic23 \
            -datadir="$datadir" \
            -cold-import="$LEGACY" \
            -nofilesync \
            -rpcport="$rpc_port" \
            -port="$p2p_port" \
            -nobgvalidation \
            > "$datadir/node.stdout" 2> "$datadir/node.stderr" &
    fi
    pid=$!

    cleanup_pid() {
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    }
    trap cleanup_pid RETURN

    wait_for_height "$datadir" "$rpc_port" "$start_ns"

    local height tip_hash commitment sha3 utxo_count elapsed_ms scan_line
    height=$(rpc_call "$datadir" "$rpc_port" getblockcount | json_result_field "")
    tip_hash=$(rpc_call "$datadir" "$rpc_port" getblockhash "$height" | json_result_field "")
    commitment=$(rpc_call "$datadir" "$rpc_port" getutxocommitment)
    sha3=$(printf '%s' "$commitment" | json_result_field "sha3_hash")
    utxo_count=$(printf '%s' "$commitment" | json_result_field "utxo_count")
    elapsed_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
    scan_line=$(extract_scan_line "$datadir")

    cleanup_pid
    trap - RETURN

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$height" "$tip_hash" "$sha3" "$utxo_count" \
        "$elapsed_ms" "$scan_line" > "$datadir/result.tsv"

    echo "  height: $height"
    echo "  tip_hash: $tip_hash"
    echo "  utxo_sha3: $sha3"
    echo "  utxo_count: $utxo_count"
    echo "  elapsed: $(awk "BEGIN{print $elapsed_ms/1000}")s"
    echo "  scan: ${scan_line:-not found}"
}

rm -rf "$SERIAL_DIR" "$PARALLEL_DIR"
mkdir -p "$TMP_ROOT"

echo "== cold-import equivalence benchmark =="
echo "  legacy: $LEGACY"
echo "  min_height: $MIN_HEIGHT"
echo "  max_secs/run: $MAX_SECS"

run_case "serial" "1" "$SERIAL_DIR" "$RPC_BASE" "$P2P_BASE"
run_case "parallel" "$PARALLEL_WORKERS" "$PARALLEL_DIR" \
    "$((RPC_BASE + 1))" "$((P2P_BASE + 1))"

serial_result=$(cat "$SERIAL_DIR/result.tsv")
parallel_result=$(cat "$PARALLEL_DIR/result.tsv")

IFS=$'\t' read -r _ s_height s_tip s_sha3 s_utxos s_ms s_scan <<<"$serial_result"
IFS=$'\t' read -r _ p_height p_tip p_sha3 p_utxos p_ms p_scan <<<"$parallel_result"

echo
echo "== comparison =="
echo "  serial:   height=$s_height tip=$s_tip utxo_sha3=$s_sha3 utxos=$s_utxos elapsed=$(awk "BEGIN{print $s_ms/1000}")s"
echo "            scan=${s_scan:-not found}"
echo "  parallel: height=$p_height tip=$p_tip utxo_sha3=$p_sha3 utxos=$p_utxos elapsed=$(awk "BEGIN{print $p_ms/1000}")s"
echo "            scan=${p_scan:-not found}"

if [ "$s_height" != "$p_height" ] ||
   [ "$s_tip" != "$p_tip" ] ||
   [ "$s_sha3" != "$p_sha3" ] ||
   [ "$s_utxos" != "$p_utxos" ]; then
    echo
    echo "FAIL: serial and parallel cold-import evidence differs" >&2
    exit 1
fi

echo
echo "PASS: serial and parallel cold-import evidence match"
echo
echo "Benchmark row seed:"
echo "  $(date +%F) | $(git rev-parse --short HEAD) | #1 cold sync PR-3 serial-vs-parallel | serial $(awk "BEGIN{print $s_ms/1000}")s; parallel $(awk "BEGIN{print $p_ms/1000}")s | serial scan: ${s_scan:-not found}; parallel scan: ${p_scan:-not found}; tip=$s_tip; utxo_sha3=$s_sha3; utxos=$s_utxos |"

if [ -z "$KEEP" ]; then
    rm -rf "$SERIAL_DIR" "$PARALLEL_DIR"
else
    echo "Kept datadirs:"
    echo "  $SERIAL_DIR"
    echo "  $PARALLEL_DIR"
fi
