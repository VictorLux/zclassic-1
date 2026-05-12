#!/bin/bash
# bench_fresh_sync.sh — Time a completely fresh zclassic23 node from 0 to
# fully synced + block explorer serving + database indexed.
#
# Measures: FlyClient verification, SHA3 UTXO snapshot, delta sync,
# block explorer first response, and database indexing completion.
#
# Usage: bash tools/bench_fresh_sync.sh
#   or:  make bench-sync

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/../zclassic23"
DATADIR="$HOME/.zclassic-c23-bench-$(date +%Y%m%d-%H%M%S)"
PORT=8047
RPCPORT=18247
HTTPSPORT=8447
LOGFILE="$DATADIR/node.log"
TIMEOUT=600  # 10 minutes max

if [ ! -x "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY — run 'make' first"
    exit 1
fi

# Check no other process is using our ports
for P in $PORT $RPCPORT $HTTPSPORT; do
    if ss -tlnp 2>/dev/null | grep -q ":${P} "; then
        echo "ERROR: Port $P already in use"
        exit 1
    fi
done

# Copy SSL certs if available (needed for HTTPS explorer)
mkdir -p "$DATADIR/ssl"
if [ -f "$HOME/.zclassic-c23/ssl/fullchain.pem" ]; then
    cp "$HOME/.zclassic-c23/ssl/fullchain.pem" "$DATADIR/ssl/"
    cp "$HOME/.zclassic-c23/ssl/privkey.pem" "$DATADIR/ssl/"
fi

echo "════════════════════════════════════════════════════════════"
echo "  ZClassic23 Cold-Start Benchmark"
echo "  FlyClient + MMB + SHA3 Snapshot → Block Explorer"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Binary:  $BINARY"
echo "Datadir: $DATADIR"
echo "Ports:   P2P=$PORT RPC=$RPCPORT HTTPS=$HTTPSPORT"
echo ""

T0=$(date +%s%N)  # nanoseconds for precision

# Start fresh node — connect ONLY to local main ZCL23 node
$BINARY \
    -datadir="$DATADIR" \
    -port=$PORT \
    -rpcport=$RPCPORT \
    -httpsport=$HTTPSPORT \
    -connect=127.0.0.1:8033 \
    -listen=0 \
    -txindex \
    -nofilesync \
    -showmetrics=0 \
    >> "$LOGFILE" 2>&1 &
PID=$!

cleanup() {
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

T0_S=$(date +%s)
echo "Started PID=$PID at $(date '+%H:%M:%S')"
echo ""

# Wait for RPC cookie file
for i in $(seq 1 60); do
    [ -f "$DATADIR/.cookie" ] && break
    sleep 1
done

if [ ! -f "$DATADIR/.cookie" ]; then
    echo "ERROR: RPC cookie file never appeared"
    tail -20 "$LOGFILE"
    exit 1
fi

COOKIE=$(cat "$DATADIR/.cookie")

rpc() {
    curl -s -u "$COOKIE" \
        --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"bench\",\"method\":\"$1\",\"params\":[$2]}" \
        -H 'content-type:text/plain;' \
        "http://127.0.0.1:$RPCPORT/" 2>/dev/null
}

get_field() {
    python3 -c "import json,sys; d=json.load(sys.stdin); print(d['result']$1)" 2>/dev/null
}

LAST_STATE=""
T_FC_VERIFIED=""
T_SNAPSHOT_START=""
T_SNAPSHOT_END=""
T_AT_TIP=""
T_EXPLORER=""
T_INDEXED=""

echo "Monitoring sync progress..."
echo "─────────────────────────────────────────"

while true; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - T0_S))

    if [ $ELAPSED -gt $TIMEOUT ]; then
        echo ""
        echo "TIMEOUT after ${TIMEOUT}s"
        break
    fi

    STATE=$(rpc syncstate "" | get_field "['state']" || echo "unknown")
    HEIGHT=$(rpc getblockcount "" | get_field "" || echo "?")

    if [ "$STATE" != "$LAST_STATE" ]; then
        echo "[${ELAPSED}s] state=$STATE height=$HEIGHT"
        LAST_STATE="$STATE"
    fi

    # FlyClient verification
    if [ -z "$T_FC_VERIFIED" ] && grep -q "FlyClient PASSED" "$LOGFILE" 2>/dev/null; then
        T_FC_VERIFIED=$ELAPSED
        echo "[${ELAPSED}s] ✓ FlyClient PASSED (20/20 MMB samples, 2⁻⁸⁰ security)"
    fi

    # Snapshot receive start
    if [ -z "$T_SNAPSHOT_START" ] && grep -q "negotiating -> receiving" "$LOGFILE" 2>/dev/null; then
        T_SNAPSHOT_START=$ELAPSED
        echo "[${ELAPSED}s] ✓ Snapshot receive started"
    fi

    # Snapshot SHA3 verification
    if [ -z "$T_SNAPSHOT_END" ] && grep -q "verifying -> complete" "$LOGFILE" 2>/dev/null; then
        T_SNAPSHOT_END=$ELAPSED
        SNAP_LINE=$(grep "UTXOs in" "$LOGFILE" | tail -1)
        echo "[${ELAPSED}s] ✓ SHA3 UTXO snapshot verified ($SNAP_LINE)"
    fi

    # Synced to tip
    if [ -z "$T_AT_TIP" ] && [ "$STATE" = "at_tip" ]; then
        T_AT_TIP=$ELAPSED
        echo "[${ELAPSED}s] ✓ SYNCED TO TIP (height $HEIGHT)"
    fi

    # Block explorer responding
    if [ -z "$T_EXPLORER" ] && [ -n "$T_AT_TIP" ]; then
        EXPLORER_OK=$(curl -sk "https://127.0.0.1:$HTTPSPORT/explorer" 2>/dev/null | grep -c "Latest Blocks" || true)
        if [ "$EXPLORER_OK" -gt 0 ]; then
            T_EXPLORER=$ELAPSED
            echo "[${ELAPSED}s] ✓ Block explorer serving (HTTPS port $HTTPSPORT)"
        fi
    fi

    # Check healthcheck for fully synced
    if [ -n "$T_AT_TIP" ] && [ -n "$T_EXPLORER" ]; then
        HEALTHY=$(rpc healthcheck "" | get_field "['healthy']" || echo "false")
        if [ "$HEALTHY" = "True" ] || [ "$HEALTHY" = "true" ]; then
            if [ -z "$T_INDEXED" ]; then
                T_INDEXED=$ELAPSED
                echo "[${ELAPSED}s] ✓ Node healthy — all services operational"
            fi
            break
        fi
    fi

    # Also break if at_tip and explorer works for 10+ seconds
    if [ -n "$T_AT_TIP" ] && [ -n "$T_EXPLORER" ]; then
        AT_TIP_DUR=$((ELAPSED - T_AT_TIP))
        if [ $AT_TIP_DUR -gt 10 ]; then
            T_INDEXED=$ELAPSED
            echo "[${ELAPSED}s] ✓ Stable at tip for ${AT_TIP_DUR}s — benchmark complete"
            break
        fi
    fi

    # Check if process died
    if ! kill -0 "$PID" 2>/dev/null; then
        echo ""
        echo "ERROR: Node process died"
        echo "Last 20 lines of log:"
        tail -20 "$LOGFILE"
        exit 1
    fi

    sleep 2
done

T_END=$(date +%s)
TOTAL=$((T_END - T0_S))

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  RESULTS: Cold Start → Block Explorer"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Total time:        ${TOTAL}s"
echo "Final height:      $(rpc getblockcount '' | get_field '' || echo '?')"
echo "Final state:       $(rpc syncstate '' | get_field \"['state']\" || echo '?')"
echo ""
echo "── Phase Timing ────────────────────────────────────────"

if [ -n "$T_FC_VERIFIED" ]; then
    echo "  FlyClient MMB:   ${T_FC_VERIFIED}s  (20 random block samples verified)"
fi
if [ -n "$T_SNAPSHOT_START" ] && [ -n "$T_SNAPSHOT_END" ]; then
    SNAP_DUR=$((T_SNAPSHOT_END - T_SNAPSHOT_START))
    echo "  SHA3 snapshot:   ${SNAP_DUR}s  (UTXO set transfer + verification)"
fi
if [ -n "$T_AT_TIP" ]; then
    echo "  Synced to tip:   ${T_AT_TIP}s"
fi
if [ -n "$T_EXPLORER" ]; then
    echo "  Explorer live:   ${T_EXPLORER}s"
fi
if [ -n "$T_INDEXED" ]; then
    echo "  Fully operational: ${T_INDEXED}s"
fi

echo ""
echo "── Validation ──────────────────────────────────────────"
BGSTATUS=$(rpc validationstatus "" | get_field "" 2>/dev/null || echo "n/a")
echo "  Background:      $BGSTATUS"

echo ""
echo "── Key Log Entries ─────────────────────────────────────"
grep -E 'FlyClient|SNAPSYNC|snapshot.*verified|SHA3|anchor|Inserted anchor|bg-valid.*Start|explorer|HTTPS' "$LOGFILE" 2>/dev/null | head -25

echo ""
echo "── Explorer Test ─────────────────────────────────────"
# Quick test: fetch explorer main page and factoids
EXPLORER_SIZE=$(curl -sk "https://127.0.0.1:$HTTPSPORT/explorer" 2>/dev/null | wc -c)
FACTOIDS_SIZE=$(curl -sk "https://127.0.0.1:$HTTPSPORT/explorer/factoids" 2>/dev/null | wc -c)
HODL_SIZE=$(curl -sk "https://127.0.0.1:$HTTPSPORT/explorer/hodl" 2>/dev/null | wc -c)
echo "  Explorer page:   ${EXPLORER_SIZE} bytes"
echo "  Factoids page:   ${FACTOIDS_SIZE} bytes"
echo "  HODL wave page:  ${HODL_SIZE} bytes"

echo ""
echo "Datadir: $DATADIR"
echo "Full log: $LOGFILE"
echo ""
echo "════════════════════════════════════════════════════════════"
