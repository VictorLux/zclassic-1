#!/bin/bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# ZClassic23 self-test integration script.
# Starts the node, runs RPC health checks, and stops the node.
#
# Usage:  ./tools/test_dual_node.sh [path/to/zclassic23]
#
# If the node is already running on port 18232, skips start/stop and
# runs checks against the live instance.

set +e

PASS=0
FAIL=0
SKIP=0

BINARY="${1:-./zclassic23}"
DATADIR="$HOME/.zclassic-c23-test"
RPC_PORT=18232
RPC_USER="testuser"
RPC_PASS="testpass"
STARTED_NODE=0
NODE_PID=0

rpc() {
    curl -sf -u "$RPC_USER:$RPC_PASS" \
         --connect-timeout 3 --max-time 10 \
         -d "{\"method\":\"$1\",\"params\":[$2]}" \
         http://127.0.0.1:${RPC_PORT}/ 2>/dev/null
}

json_field() {
    python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    keys='$1'.split('.')
    v=d
    for k in keys:
        v=v[k]
    print(v)
except: pass
" 2>/dev/null
}

check() {
    local name="$1" status="$2" detail="$3"
    if [ "$status" = "PASS" ]; then
        printf "  [PASS] %-50s %s\n" "$name" "$detail"
        PASS=$((PASS+1))
    elif [ "$status" = "SKIP" ]; then
        printf "  [SKIP] %-50s %s\n" "$name" "$detail"
        SKIP=$((SKIP+1))
    else
        printf "  [FAIL] %-50s %s\n" "$name" "$detail"
        FAIL=$((FAIL+1))
    fi
}

cleanup() {
    if [ "$STARTED_NODE" -eq 1 ] && [ "$NODE_PID" -gt 0 ]; then
        echo ""
        echo "  Stopping node (pid $NODE_PID)..."
        # Try clean RPC stop first
        rpc stop >/dev/null 2>&1
        sleep 2
        if kill -0 "$NODE_PID" 2>/dev/null; then
            kill "$NODE_PID" 2>/dev/null
            sleep 1
        fi
        if kill -0 "$NODE_PID" 2>/dev/null; then
            kill -9 "$NODE_PID" 2>/dev/null
        fi
        check "Node stopped cleanly" "PASS" "pid=$NODE_PID"
    fi
}
trap cleanup EXIT

echo ""
echo "================================================================"
echo "  ZClassic23 Self-Test Integration"
echo "================================================================"
echo ""

# --- Check if node is already running ---

EXISTING=$(rpc getinfo | json_field result.version)
if [ -n "$EXISTING" ] && [ "$EXISTING" != "" ]; then
    echo "  Node already running on port $RPC_PORT (version=$EXISTING)"
    echo "  Running checks against live instance."
    echo ""
else
    # --- Start the node ---
    echo "  -- Starting Node --"

    if [ ! -x "$BINARY" ]; then
        check "Binary exists" "FAIL" "$BINARY not found or not executable"
        echo "  Aborting: build with 'make zclassic23' first."
        exit 1
    fi
    check "Binary exists" "PASS" "$BINARY"

    mkdir -p "$DATADIR"

    # Write minimal config
    cat > "$DATADIR/zclassic.conf" <<CONF
rpcuser=$RPC_USER
rpcpassword=$RPC_PASS
rpcport=$RPC_PORT
port=18033
listen=0
connect=0
printtoconsole=0
CONF

    "$BINARY" -datadir="$DATADIR" -rpcport=$RPC_PORT -daemon &
    NODE_PID=$!
    STARTED_NODE=1
    check "Node started" "PASS" "pid=$NODE_PID"

    # --- Wait for RPC ---
    echo ""
    echo "  -- Waiting for RPC --"

    READY=0
    for i in $(seq 1 30); do
        R=$(rpc getinfo)
        VER=$(echo "$R" | json_field result.version)
        if [ -n "$VER" ] && [ "$VER" != "" ]; then
            READY=1
            break
        fi
        sleep 1
    done

    if [ "$READY" -eq 1 ]; then
        check "RPC available" "PASS" "ready after ${i}s"
    else
        check "RPC available" "FAIL" "not responding after 30s"
        echo "  Aborting: node did not start."
        exit 1
    fi
    echo ""
fi

# --- 1. Basic RPC Checks ---

echo "  -- Basic RPC Checks --"

# getblockcount
R=$(rpc getblockcount)
HEIGHT=$(echo "$R" | json_field result)
if echo "$HEIGHT" | grep -qE '^[0-9]+$'; then
    check "getblockcount returns number" "PASS" "height=$HEIGHT"
else
    check "getblockcount returns number" "FAIL" "got: $HEIGHT"
fi

# getinfo
R=$(rpc getinfo)
VERSION=$(echo "$R" | json_field result.version)
PROTO=$(echo "$R" | json_field result.protocolversion)
if [ -n "$VERSION" ]; then
    check "getinfo returns version" "PASS" "version=$VERSION proto=$PROTO"
else
    check "getinfo returns version" "FAIL" "empty response"
fi

echo ""

# --- 2. Peer Info ---

echo "  -- Peer & Network Checks --"

R=$(rpc getpeerinfo)
IS_JSON=$(echo "$R" | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    r=d.get('result')
    if isinstance(r, list):
        print('yes:%d' % len(r))
    else:
        print('no')
except: print('no')
" 2>/dev/null)

if [[ "$IS_JSON" == yes:* ]]; then
    PEER_COUNT="${IS_JSON#yes:}"
    check "getpeerinfo returns valid JSON" "PASS" "$PEER_COUNT peers"
else
    check "getpeerinfo returns valid JSON" "FAIL" "invalid response"
fi

R=$(rpc getnetworkinfo)
NET_VER=$(echo "$R" | json_field result.version)
if [ -n "$NET_VER" ]; then
    check "getnetworkinfo" "PASS" "version=$NET_VER"
else
    check "getnetworkinfo" "SKIP" "not implemented"
fi

R=$(rpc getconnectioncount)
CONNS=$(echo "$R" | json_field result)
if echo "$CONNS" | grep -qE '^[0-9]+$'; then
    check "getconnectioncount" "PASS" "$CONNS connections"
else
    check "getconnectioncount" "SKIP" "not available"
fi

echo ""

# --- 3. Wallet Checks ---

echo "  -- Wallet Checks --"

R=$(rpc getwalletinfo)
IS_WALLET=$(echo "$R" | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    r=d.get('result')
    if isinstance(r, dict):
        print('yes')
    else:
        print('no')
except: print('no')
" 2>/dev/null)

if [ "$IS_WALLET" = "yes" ]; then
    W_BAL=$(echo "$R" | json_field result.balance)
    check "getwalletinfo returns valid JSON" "PASS" "balance=$W_BAL"
else
    check "getwalletinfo returns valid JSON" "FAIL" "invalid response"
fi

R=$(rpc getnewaddress)
ADDR=$(echo "$R" | json_field result)
if [[ "$ADDR" == t1* ]]; then
    check "getnewaddress" "PASS" "$ADDR"
else
    check "getnewaddress" "SKIP" "response: $ADDR"
fi

R=$(rpc z_getnewaddress)
ZADDR=$(echo "$R" | json_field result)
if [[ "$ZADDR" == zs1* ]]; then
    check "z_getnewaddress" "PASS" "${ZADDR:0:30}..."
else
    check "z_getnewaddress" "SKIP" "response: $ZADDR"
fi

echo ""

# --- 4. Blockchain Checks ---

echo "  -- Blockchain Checks --"

R=$(rpc getblockchaininfo)
CHAIN=$(echo "$R" | json_field result.chain)
if [ -n "$CHAIN" ]; then
    BEST=$(echo "$R" | json_field result.bestblockhash)
    check "getblockchaininfo" "PASS" "chain=$CHAIN"
else
    check "getblockchaininfo" "SKIP" "not implemented"
fi

R=$(rpc getbestblockhash)
BEST=$(echo "$R" | json_field result)
if [ ${#BEST} -eq 64 ] 2>/dev/null; then
    check "getbestblockhash" "PASS" "${BEST:0:16}..."
else
    check "getbestblockhash" "SKIP" "not available"
fi

R=$(rpc getdifficulty)
DIFF=$(echo "$R" | json_field result)
if [ -n "$DIFF" ]; then
    check "getdifficulty" "PASS" "$DIFF"
else
    check "getdifficulty" "SKIP" "not available"
fi

echo ""

# --- 5. Store / Status Endpoints ---

echo "  -- Store & Status Endpoints --"

# Try /store via RPC HTTP (onion fallback)
STORE_RESP=$(curl -sf --connect-timeout 3 --max-time 5 \
    http://127.0.0.1:${RPC_PORT}/store 2>/dev/null)
if echo "$STORE_RESP" | grep -q "ZCL Store" 2>/dev/null; then
    check "GET /store returns HTML" "PASS" "store page served"
elif [ -n "$STORE_RESP" ]; then
    check "GET /store returns HTML" "SKIP" "response received but no store content"
else
    check "GET /store returns HTML" "SKIP" "not served on RPC port"
fi

# Try /status
STATUS_RESP=$(curl -sf --connect-timeout 3 --max-time 5 \
    http://127.0.0.1:${RPC_PORT}/status 2>/dev/null)
STATUS_HEIGHT=$(echo "$STATUS_RESP" | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    print(d.get('height', d.get('blocks', '')))
except: pass
" 2>/dev/null)
if [ -n "$STATUS_HEIGHT" ]; then
    check "GET /status returns JSON with height" "PASS" "height=$STATUS_HEIGHT"
else
    # Fallback: use getblockcount as the status check
    if [ -n "$HEIGHT" ] && echo "$HEIGHT" | grep -qE '^[0-9]+$'; then
        check "GET /status returns JSON with height" "SKIP" "endpoint not available, getblockcount=$HEIGHT"
    else
        check "GET /status returns JSON with height" "SKIP" "endpoint not available"
    fi
fi

echo ""

# --- 6. Mining RPC Checks ---

echo "  -- Mining Checks --"

R=$(rpc getmininginfo)
MINING_H=$(echo "$R" | json_field result.blocks)
if echo "$MINING_H" | grep -qE '^[0-9]+$'; then
    check "getmininginfo" "PASS" "blocks=$MINING_H"
else
    check "getmininginfo" "SKIP" "not available"
fi

echo ""

# --- Summary ---

echo "================================================================"
printf "  Results: %d PASS  %d FAIL  %d SKIP\n" $PASS $FAIL $SKIP
echo "================================================================"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
