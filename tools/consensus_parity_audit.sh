#!/bin/bash
# Consensus parity audit — compare zclassic23 (C23) vs zclassicd (C++)
# Usage: ./tools/consensus_parity_audit.sh
#
# Requires both nodes running:
#   C23:  RPC on localhost:18232 (cookie auth from ~/.zclassic-c23/.cookie)
#   C++:  RPC on localhost:8232  (rpcuser/rpcpassword from ~/.zclassic/zclassic.conf)

set -euo pipefail

ZCL_RPC="./tools/zcl-rpc"
CPP_USER="zclrhett"
CPP_PASS="zclrhettpass2026"
CPP_PORT="8232"

AUDIT_HEIGHTS=(1 100 1000 10000 100000 500000 1000000 1500000 2000000)

# ── Helpers ──────────────────────────────────────────────

c23_rpc() {
    $ZCL_RPC "$@" 2>/dev/null | python3 -c "import sys,json; print(json.load(sys.stdin)['result'])"
}

cpp_rpc() {
    local method="$1"; shift
    local params="${1:-[]}"
    curl -sf --user "$CPP_USER:$CPP_PASS" \
        --data-binary "{\"jsonrpc\":\"1.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}" \
        "http://127.0.0.1:$CPP_PORT/" 2>/dev/null \
        | python3 -c "import sys,json; print(json.load(sys.stdin)['result'])"
}

# ── Check node availability ──────────────────────────────

echo "=== Consensus Parity Audit ==="
echo ""

C23_HEIGHT=$(c23_rpc getblockcount 2>/dev/null || echo "OFFLINE")
echo "C23  node: height=$C23_HEIGHT"

CPP_HEIGHT=$(cpp_rpc getblockcount 2>/dev/null || echo "OFFLINE")
echo "C++  node: height=$CPP_HEIGHT"
echo ""

if [ "$C23_HEIGHT" = "OFFLINE" ]; then
    echo "FATAL: C23 node not responding on :18232"
    exit 1
fi

# ── Add current tip to audit set ─────────────────────────

AUDIT_HEIGHTS+=("$C23_HEIGHT")

# ── Run audit ────────────────────────────────────────────

PASS=0
FAIL=0
SKIP=0

printf "%-10s %-66s %-66s %s\n" "Height" "C23 Block Hash" "C++ Block Hash" "Result"
printf "%-10s %-66s %-66s %s\n" "------" "--------------" "--------------" "------"

for h in "${AUDIT_HEIGHTS[@]}"; do
    c23_hash=$(c23_rpc getblockhash "$h" 2>/dev/null || echo "N/A")

    if [ "$CPP_HEIGHT" = "OFFLINE" ] || [ "$CPP_HEIGHT" -lt "$h" ] 2>/dev/null; then
        cpp_hash="(not synced)"
        result="SKIP"
        SKIP=$((SKIP + 1))
    else
        cpp_hash=$(cpp_rpc getblockhash "[${h}]" 2>/dev/null || echo "ERROR")
        if [ "$c23_hash" = "$cpp_hash" ]; then
            result="MATCH"
            PASS=$((PASS + 1))
        else
            result="MISMATCH"
            FAIL=$((FAIL + 1))
        fi
    fi

    printf "%-10s %-66s %-66s %s\n" "$h" "$c23_hash" "$cpp_hash" "$result"
done

echo ""
echo "=== Summary: $PASS match, $FAIL mismatch, $SKIP skipped ==="

# ── coins_best_block self-consistency ────────────────────

echo ""
echo "=== C23 Internal Consistency ==="
BEST_HASH=$(c23_rpc getbestblockhash 2>/dev/null)
BEST_HEIGHT=$(c23_rpc getblockcount 2>/dev/null)
TIP_HASH=$(c23_rpc getblockhash "$BEST_HEIGHT" 2>/dev/null)

echo "  bestblockhash:          $BEST_HASH"
echo "  getblockhash(tip):      $TIP_HASH"

if [ "$BEST_HASH" = "$TIP_HASH" ]; then
    echo "  coins_best_block:       CONSISTENT (matches chain tip)"
else
    echo "  coins_best_block:       INCONSISTENT!"
    FAIL=$((FAIL + 1))
fi

echo ""
if [ "$FAIL" -gt 0 ]; then
    echo "AUDIT FAILED ($FAIL mismatches)"
    exit 1
else
    echo "AUDIT PASSED"
    exit 0
fi
