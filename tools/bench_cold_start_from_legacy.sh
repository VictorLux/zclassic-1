#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# bench_cold_start_from_legacy.sh — FS8 acceptance benchmark.
#
# Measures the wall-clock time from a fresh empty datadir to a fully-
# synced zclassic23 node when -importfromlegacy=~/.zclassic is used.
# Pass criterion: tip >= MIN_HEIGHT within MAX_SECS.
#
# Run from the zclassic23 repo root after a clean `make`.
#
# Env knobs:
#   ZCL_BENCH_LEGACY_DATADIR   path to legacy zclassicd datadir
#                              (default: $HOME/.zclassic)
#   ZCL_BENCH_TMP              tmp datadir root (default: /tmp/zcl-bench-cold)
#   ZCL_BENCH_RPC_PORT         RPC port for the test instance (default: 18333)
#   ZCL_BENCH_P2P_PORT         P2P port (default: 8044)
#   ZCL_BENCH_MIN_HEIGHT       acceptance height floor (default: 3000000)
#   ZCL_BENCH_MAX_SECS         acceptance time ceiling (default: 600 = 10 min)
#   ZCL_BENCH_KEEP             keep the tmp datadir after run (default: no)
#
# Pass criteria (matches plan section "Cold-start benchmark"):
#   getblockcount >= ZCL_BENCH_MIN_HEIGHT within ZCL_BENCH_MAX_SECS

set -euo pipefail

LEGACY="${ZCL_BENCH_LEGACY_DATADIR:-$HOME/.zclassic}"
TMP="${ZCL_BENCH_TMP:-/tmp/zcl-bench-cold}"
RPC_PORT="${ZCL_BENCH_RPC_PORT:-18333}"
P2P_PORT="${ZCL_BENCH_P2P_PORT:-8044}"
MIN_HEIGHT="${ZCL_BENCH_MIN_HEIGHT:-3000000}"
MAX_SECS="${ZCL_BENCH_MAX_SECS:-600}"
KEEP="${ZCL_BENCH_KEEP:-}"

if [ ! -d "$LEGACY/blocks" ]; then
    echo "FAIL: legacy datadir $LEGACY/blocks not found" >&2
    exit 2
fi

if [ ! -x "./zclassic23" ]; then
    echo "FAIL: ./zclassic23 not built (run make first)" >&2
    exit 2
fi

if [ ! -x "./tools/zcl-rpc" ]; then
    echo "FAIL: ./tools/zcl-rpc not built (run make first)" >&2
    exit 2
fi

# Clean slate
rm -rf "$TMP"
mkdir -p "$TMP"

echo "── bench_cold_start_from_legacy ────────────────────────────"
echo "  legacy_datadir:   $LEGACY"
echo "  test_datadir:     $TMP"
echo "  rpc_port:         $RPC_PORT"
echo "  acceptance:       height >= $MIN_HEIGHT within ${MAX_SECS}s"
echo

start_ns=$(date +%s%N)

# Boot the node. -tor=0 keeps Tor cold so the only cost is the ingest.
# -nobgvalidation defers historical Equihash + Groth16 reverification
# to a background pass; the at-tip ordering invariant still applies.
./zclassic23 \
    -datadir="$TMP" \
    -importfromlegacy="$LEGACY" \
    -bodypull-from-legacy="$LEGACY" \
    -nofilesync \
    -rpcport="$RPC_PORT" \
    -port="$P2P_PORT" \
    -nobgvalidation \
    > "$TMP/node.stdout" 2> "$TMP/node.stderr" &
NODE_PID=$!
trap 'kill -TERM "$NODE_PID" 2>/dev/null || true; wait "$NODE_PID" 2>/dev/null || true' EXIT

echo "  node pid:         $NODE_PID  (logs: $TMP/node.std{out,err})"

# Poll getblockcount once a second; bail when we hit the floor or
# blow past the ceiling.
H=0
while :; do
    now_ns=$(date +%s%N)
    elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))
    if [ "$elapsed_ms" -ge "$((MAX_SECS * 1000))" ]; then
        echo
        echo "FAIL: $MAX_SECS s elapsed, height=$H (floor=$MIN_HEIGHT)"
        echo "      last 20 lines of node.stderr:"
        tail -20 "$TMP/node.stderr" | sed 's/^/        /'
        exit 1
    fi
    H_raw=$(ZCL_RPCPORT="$RPC_PORT" ZCL_DATADIR="$TMP" ./tools/zcl-rpc getblockcount 2>/dev/null || echo "")
    H=$(printf '%s\n' "$H_raw" | python3 -c '
import json, sys
s = sys.stdin.read().strip()
try:
    v = json.loads(s)
    r = v.get("result")
    print(int(r) if isinstance(r, int) else 0)
except Exception:
    print(int(s) if s.isdigit() else 0)
')
    : "${H:=0}"
    printf "\r  elapsed=%5.1fs  height=%d  " "$(awk "BEGIN{print $elapsed_ms/1000}")" "$H"
    if [ "$H" -ge "$MIN_HEIGHT" ]; then
        break
    fi
    sleep 1
done

elapsed_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
echo
echo
echo "PASS: height=$H >= $MIN_HEIGHT in $(awk "BEGIN{print $elapsed_ms/1000}") s"

# Surface a few diagnostic bits for the operator.
echo
echo "  zcl_status snapshot:"
ZCL_RPCPORT="$RPC_PORT" ZCL_DATADIR="$TMP" ./tools/zcl-rpc status 2>/dev/null | head -40 | sed 's/^/    /' || true

echo
echo "  local_chain_ingest dump:"
ZCL_RPCPORT="$RPC_PORT" ZCL_DATADIR="$TMP" ./tools/zcl-rpc dumpstate '"local_ingest"' 2>/dev/null \
    | head -20 | sed 's/^/    /' || true

echo
echo "  header_probe dump:"
ZCL_RPCPORT="$RPC_PORT" ZCL_DATADIR="$TMP" ./tools/zcl-rpc dumpstate '"header_probe"' 2>/dev/null \
    | head -20 | sed 's/^/    /' || true

echo
echo "  rolling_anchor dump:"
ZCL_RPCPORT="$RPC_PORT" ZCL_DATADIR="$TMP" ./tools/zcl-rpc dumpstate '"rolling_anchor"' 2>/dev/null \
    | head -20 | sed 's/^/    /' || true

echo
echo "  oracle_policy dump:"
ZCL_RPCPORT="$RPC_PORT" ZCL_DATADIR="$TMP" ./tools/zcl-rpc dumpstate '"oracle_policy"' 2>/dev/null \
    | head -20 | sed 's/^/    /' || true

kill -TERM "$NODE_PID" 2>/dev/null || true
wait "$NODE_PID" 2>/dev/null || true
trap - EXIT

if [ -z "$KEEP" ]; then
    rm -rf "$TMP"
else
    echo
    echo "  kept tmp datadir at $TMP"
fi
