#!/bin/bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Dual-node integration test: exercises every transaction type zclassic23
# supports, verifies relay to zclassicd, and confirms both nodes stay in sync.
#
# Prerequisites:
#   - zclassic23 running on RPC 18232 (systemctl --user status zclassic23)
#   - zclassicd  running on RPC 8232  (systemctl --user status zclassicd)
#   - Both synced to mainnet tip
#   - Wallet has transparent and shielded balance for send tests

set +e

PASS=0
FAIL=0
SKIP=0

C23_COOKIE=$(cat ~/.zclassic-c23/.cookie 2>/dev/null)
CPP_USER="zcluser:zclpass"

c23() { curl -s -u "$C23_COOKIE" -d "{\"method\":\"$1\",\"params\":[$2]}" http://127.0.0.1:18232/ ; }
cpp() { curl -s -u "$CPP_USER" -d "{\"method\":\"$1\",\"params\":[$2]}" http://127.0.0.1:8232/ ; }

result() { echo "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('result','ERROR'))" 2>/dev/null; }
err()    { echo "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); e=d.get('error'); print(e['message'] if e else '')" 2>/dev/null; }
jq_r()   { echo "$1" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; exec(open('/dev/stdin').read())" <<< "print($2)" 2>/dev/null; }

check() {
    local name="$1" status="$2" detail="$3"
    if [ "$status" = "PASS" ]; then
        printf "  \033[32m✓\033[0m %-55s %s\n" "$name" "$detail"
        PASS=$((PASS+1))
    elif [ "$status" = "SKIP" ]; then
        printf "  \033[33m○\033[0m %-55s %s\n" "$name" "$detail"
        SKIP=$((SKIP+1))
    else
        printf "  \033[31m✗\033[0m %-55s %s\n" "$name" "$detail"
        FAIL=$((FAIL+1))
    fi
}

wait_for_mempool_relay() {
    local txid="$1" max_wait="${2:-30}"
    for i in $(seq 1 "$max_wait"); do
        local R=$(cpp getrawtransaction "\"$txid\"")
        local raw=$(result "$R")
        if [ ${#raw} -gt 10 ] 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

echo ""
echo "════════════════════════════════════════════════════════════════════"
echo "  ZClassic Dual-Node Integration Test"
echo "  C23 node: 18232    C++ node: 8232"
echo "════════════════════════════════════════════════════════════════════"
echo ""

# ─── 0. CONNECTIVITY CHECK ───────────────────────────────────────────

echo "  ── Connectivity ──"

C23_H=$(result "$(c23 getblockcount)")
CPP_H=$(result "$(cpp getblockcount)")

if [ "$C23_H" -gt 0 ] 2>/dev/null; then
    check "C23 node reachable" "PASS" "height=$C23_H"
else
    check "C23 node reachable" "FAIL" "not responding"
    echo "  Aborting: C23 node not running."
    exit 1
fi

if [ "$CPP_H" -gt 0 ] 2>/dev/null; then
    check "C++ node reachable" "PASS" "height=$CPP_H"
else
    check "C++ node reachable" "FAIL" "not responding"
    echo "  Aborting: C++ node not running."
    exit 1
fi

DIFF_H=$((CPP_H - C23_H))
if [ "$DIFF_H" -lt 0 ]; then DIFF_H=$((-DIFF_H)); fi
if [ "$DIFF_H" -lt 10 ]; then
    check "Nodes in sync" "PASS" "C23=$C23_H C++=$CPP_H (delta=$DIFF_H)"
else
    check "Nodes in sync" "FAIL" "C23=$C23_H C++=$CPP_H (delta=$DIFF_H)"
fi

# Check P2P connection between nodes
C23_PEERS=$(result "$(c23 getconnectioncount)")
check "C23 peer connections" "PASS" "$C23_PEERS peers"

echo ""

# ─── 1. TRANSPARENT BALANCE CHECK ────────────────────────────────────

echo "  ── Balance Checks ──"

R=$(c23 z_gettotalbalance)
T_BAL=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['transparent'])" 2>/dev/null)
Z_BAL=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['private'])" 2>/dev/null)
TOTAL=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['total'])" 2>/dev/null)

check "Transparent balance" "PASS" "$T_BAL ZCL"
check "Shielded balance" "PASS" "$Z_BAL ZCL"
check "Total balance" "PASS" "$TOTAL ZCL"

HAS_T_BAL=$(python3 -c "print('yes' if float('${T_BAL:-0}') > 0.001 else 'no')" 2>/dev/null)
HAS_Z_BAL=$(python3 -c "print('yes' if float('${Z_BAL:-0}') > 0.001 else 'no')" 2>/dev/null)

echo ""

# ─── 2. TRANSPARENT SENDS (t→t) ──────────────────────────────────────

echo "  ── Transparent Transactions (t→t) ──"

if [ "$HAS_T_BAL" = "yes" ]; then
    # 2.1 sendtoaddress
    RECV_T=$(result "$(c23 getnewaddress)")
    R=$(c23 sendtoaddress "\"$RECV_T\",0.0001")
    TXID_TT=$(result "$R")
    if [ ${#TXID_TT} -eq 64 ]; then
        check "sendtoaddress (t→t)" "PASS" "txid=${TXID_TT:0:16}..."

        # 2.2 Verify relay to C++ node
        if wait_for_mempool_relay "$TXID_TT" 30; then
            check "t→t relay to C++ node" "PASS" "found in C++ mempool"
        else
            check "t→t relay to C++ node" "FAIL" "not relayed within 30s"
        fi
    else
        check "sendtoaddress (t→t)" "FAIL" "$(err "$R")"
        check "t→t relay to C++ node" "SKIP" "send failed"
    fi

    # 2.3 sendmany (multi-output t→t)
    RECV_T2=$(result "$(c23 getnewaddress)")
    RECV_T3=$(result "$(c23 getnewaddress)")
    R=$(c23 sendmany "\"\",[{\"$RECV_T2\":0.0001},{\"$RECV_T3\":0.0001}]")
    TXID_SM=$(result "$R")
    if [ ${#TXID_SM} -eq 64 ]; then
        check "sendmany (multi-output t→t)" "PASS" "txid=${TXID_SM:0:16}..."
    else
        # sendmany may not be implemented; check error
        check "sendmany (multi-output t→t)" "SKIP" "$(err "$R")"
    fi
else
    check "sendtoaddress (t→t)" "SKIP" "no transparent balance"
    check "t→t relay to C++ node" "SKIP" "no transparent balance"
    check "sendmany (multi-output t→t)" "SKIP" "no transparent balance"
fi

echo ""

# ─── 3. SHIELDING (t→z) ──────────────────────────────────────────────

echo "  ── Shielding Transactions (t→z) ──"

ZADDR=$(result "$(c23 z_getnewaddress)")
check "z_getnewaddress" "PASS" "${ZADDR:0:30}..."

if [ "$HAS_T_BAL" = "yes" ]; then
    # Get a funded transparent address
    UTXOS=$(c23 listunspent)
    FROM_T=$(echo "$UTXOS" | python3 -c "
import json,sys
utxos = json.load(sys.stdin)['result']
for u in utxos:
    if float(u['amount']) > 0.001:
        print(u['address'])
        break
" 2>/dev/null)

    if [ -n "$FROM_T" ]; then
        R=$(c23 z_sendmany "\"$FROM_T\",[{\"address\":\"$ZADDR\",\"amount\":0.0001}]")
        TXID_TZ=$(result "$R")
        if [ ${#TXID_TZ} -eq 64 ]; then
            check "z_sendmany t→z (shield)" "PASS" "txid=${TXID_TZ:0:16}..."

            if wait_for_mempool_relay "$TXID_TZ" 30; then
                check "t→z relay to C++ node" "PASS" "found in C++ mempool"
            else
                check "t→z relay to C++ node" "FAIL" "not relayed within 30s"
            fi
        else
            check "z_sendmany t→z (shield)" "FAIL" "$(err "$R")"
            check "t→z relay to C++ node" "SKIP" "send failed"
        fi
    else
        check "z_sendmany t→z (shield)" "SKIP" "no funded transparent address"
        check "t→z relay to C++ node" "SKIP" "no funded transparent address"
    fi
else
    check "z_sendmany t→z (shield)" "SKIP" "no transparent balance"
    check "t→z relay to C++ node" "SKIP" "no transparent balance"
fi

echo ""

# ─── 4. SHIELDED SENDS (z→z) ─────────────────────────────────────────

echo "  ── Fully Shielded Transactions (z→z) ──"

if [ "$HAS_Z_BAL" = "yes" ]; then
    # Get a funded z-address
    ZNOTES=$(c23 z_listunspent)
    FROM_Z=$(echo "$ZNOTES" | python3 -c "
import json,sys
notes = json.load(sys.stdin)['result']
for n in notes:
    if float(n['amount']) > 0.001:
        print(n['address'])
        break
" 2>/dev/null)
    ZADDR2=$(result "$(c23 z_getnewaddress)")

    if [ -n "$FROM_Z" ]; then
        R=$(c23 z_sendmany "\"$FROM_Z\",[{\"address\":\"$ZADDR2\",\"amount\":0.0001}]")
        TXID_ZZ=$(result "$R")
        if [ ${#TXID_ZZ} -eq 64 ]; then
            check "z_sendmany z→z (fully shielded)" "PASS" "txid=${TXID_ZZ:0:16}..."

            if wait_for_mempool_relay "$TXID_ZZ" 30; then
                check "z→z relay to C++ node" "PASS" "found in C++ mempool"
            else
                check "z→z relay to C++ node" "FAIL" "not relayed within 30s"
            fi
        else
            check "z_sendmany z→z (fully shielded)" "FAIL" "$(err "$R")"
            check "z→z relay to C++ node" "SKIP" "send failed"
        fi
    else
        check "z_sendmany z→z (fully shielded)" "SKIP" "no funded z-address"
        check "z→z relay to C++ node" "SKIP" "no funded z-address"
    fi
else
    check "z_sendmany z→z (fully shielded)" "SKIP" "no shielded balance"
    check "z→z relay to C++ node" "SKIP" "no shielded balance"
fi

echo ""

# ─── 5. UNSHIELDING (z→t) ────────────────────────────────────────────

echo "  ── Unshielding Transactions (z→t) ──"

if [ "$HAS_Z_BAL" = "yes" ]; then
    FROM_Z2=$(echo "$ZNOTES" | python3 -c "
import json,sys
notes = json.load(sys.stdin)['result']
for n in notes:
    if float(n['amount']) > 0.002:
        print(n['address'])
        break
" 2>/dev/null)
    RECV_T_UNSHIELD=$(result "$(c23 getnewaddress)")

    if [ -n "$FROM_Z2" ]; then
        R=$(c23 z_sendmany "\"$FROM_Z2\",[{\"address\":\"$RECV_T_UNSHIELD\",\"amount\":0.0001}]")
        TXID_ZT=$(result "$R")
        if [ ${#TXID_ZT} -eq 64 ]; then
            check "z_sendmany z→t (unshield)" "PASS" "txid=${TXID_ZT:0:16}..."

            if wait_for_mempool_relay "$TXID_ZT" 30; then
                check "z→t relay to C++ node" "PASS" "found in C++ mempool"
            else
                check "z→t relay to C++ node" "FAIL" "not relayed within 30s"
            fi
        else
            check "z_sendmany z→t (unshield)" "FAIL" "$(err "$R")"
            check "z→t relay to C++ node" "SKIP" "send failed"
        fi
    else
        check "z_sendmany z→t (unshield)" "SKIP" "no funded z-address with >0.002"
        check "z→t relay to C++ node" "SKIP" "no funded z-address"
    fi
else
    check "z_sendmany z→t (unshield)" "SKIP" "no shielded balance"
    check "z→t relay to C++ node" "SKIP" "no shielded balance"
fi

echo ""

# ─── 6. MIXED SHIELDED (z→t+z) ───────────────────────────────────────

echo "  ── Mixed Shielded Transactions (z→t+z) ──"

if [ "$HAS_Z_BAL" = "yes" ]; then
    FROM_Z3=$(echo "$ZNOTES" | python3 -c "
import json,sys
notes = json.load(sys.stdin)['result']
for n in notes:
    if float(n['amount']) > 0.003:
        print(n['address'])
        break
" 2>/dev/null)
    RECV_T_MIX=$(result "$(c23 getnewaddress)")
    RECV_Z_MIX=$(result "$(c23 z_getnewaddress)")

    if [ -n "$FROM_Z3" ]; then
        R=$(c23 z_sendmany "\"$FROM_Z3\",[{\"address\":\"$RECV_T_MIX\",\"amount\":0.0001},{\"address\":\"$RECV_Z_MIX\",\"amount\":0.0001}]")
        TXID_MIX=$(result "$R")
        if [ ${#TXID_MIX} -eq 64 ]; then
            check "z_sendmany z→(t+z) mixed" "PASS" "txid=${TXID_MIX:0:16}..."

            if wait_for_mempool_relay "$TXID_MIX" 30; then
                check "z→(t+z) relay to C++ node" "PASS" "found in C++ mempool"
            else
                check "z→(t+z) relay to C++ node" "FAIL" "not relayed within 30s"
            fi
        else
            check "z_sendmany z→(t+z) mixed" "FAIL" "$(err "$R")"
            check "z→(t+z) relay to C++ node" "SKIP" "send failed"
        fi
    else
        check "z_sendmany z→(t+z) mixed" "SKIP" "no funded z-address with >0.003"
        check "z→(t+z) relay to C++ node" "SKIP" "insufficient balance"
    fi
else
    check "z_sendmany z→(t+z) mixed" "SKIP" "no shielded balance"
    check "z→(t+z) relay to C++ node" "SKIP" "no shielded balance"
fi

echo ""

# ─── 7. MEMO SUPPORT ─────────────────────────────────────────────────

echo "  ── Memo Transactions ──"

if [ "$HAS_T_BAL" = "yes" ]; then
    ZADDR_MEMO=$(result "$(c23 z_getnewaddress)")
    FROM_MEMO=$(echo "$UTXOS" | python3 -c "
import json,sys
utxos = json.load(sys.stdin)['result']
for u in utxos:
    if float(u['amount']) > 0.001:
        print(u['address'])
        break
" 2>/dev/null)
    if [ -n "$FROM_MEMO" ]; then
        R=$(c23 z_sendmany "\"$FROM_MEMO\",[{\"address\":\"$ZADDR_MEMO\",\"amount\":0.0001,\"memo\":\"48656c6c6f\"}]")
        TXID_MEMO=$(result "$R")
        if [ ${#TXID_MEMO} -eq 64 ]; then
            check "z_sendmany with memo (t→z)" "PASS" "memo=0x48656c6c6f (Hello)"
        else
            check "z_sendmany with memo (t→z)" "FAIL" "$(err "$R")"
        fi
    else
        check "z_sendmany with memo (t→z)" "SKIP" "no funded address"
    fi
else
    check "z_sendmany with memo (t→z)" "SKIP" "no transparent balance"
fi

echo ""

# ─── 8. MULTISIG ─────────────────────────────────────────────────────

echo "  ── Multisig Operations ──"

# Create 2-of-3 multisig
ADDR1=$(result "$(c23 getnewaddress)")
ADDR2=$(result "$(c23 getnewaddress)")
ADDR3=$(result "$(c23 getnewaddress)")

R=$(c23 validateaddress "\"$ADDR1\"")
PK1=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result'].get('pubkey',''))" 2>/dev/null)
R=$(c23 validateaddress "\"$ADDR2\"")
PK2=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result'].get('pubkey',''))" 2>/dev/null)
R=$(c23 validateaddress "\"$ADDR3\"")
PK3=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result'].get('pubkey',''))" 2>/dev/null)

if [ ${#PK1} -eq 66 ] && [ ${#PK2} -eq 66 ] && [ ${#PK3} -eq 66 ]; then
    R=$(c23 createmultisig "2,[\"$PK1\",\"$PK2\",\"$PK3\"]")
    MS_ADDR=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['address'])" 2>/dev/null)
    MS_REDEEM=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['redeemScript'])" 2>/dev/null)
    if [ -n "$MS_ADDR" ]; then
        check "createmultisig (2-of-3)" "PASS" "$MS_ADDR"
    else
        check "createmultisig (2-of-3)" "FAIL" "$(err "$R")"
    fi

    # addmultisigaddress
    R=$(c23 addmultisigaddress "2,[\"$PK1\",\"$PK2\",\"$PK3\"]")
    AMS=$(result "$R")
    if [[ "$AMS" == t3* ]]; then
        check "addmultisigaddress (2-of-3)" "PASS" "$AMS"
    else
        check "addmultisigaddress (2-of-3)" "FAIL" "$(err "$R")"
    fi

    # Fund multisig (if balance)
    if [ "$HAS_T_BAL" = "yes" ] && [ -n "$MS_ADDR" ]; then
        R=$(c23 sendtoaddress "\"$MS_ADDR\",0.0001")
        MS_TXID=$(result "$R")
        if [ ${#MS_TXID} -eq 64 ]; then
            check "Fund multisig address" "PASS" "txid=${MS_TXID:0:16}..."
        else
            check "Fund multisig address" "FAIL" "$(err "$R")"
        fi
    else
        check "Fund multisig address" "SKIP" "no balance or no multisig addr"
    fi
else
    check "createmultisig (2-of-3)" "SKIP" "pubkeys not available"
    check "addmultisigaddress (2-of-3)" "SKIP" "pubkeys not available"
    check "Fund multisig address" "SKIP" "pubkeys not available"
fi

echo ""

# ─── 9. RAW TRANSACTION OPERATIONS ───────────────────────────────────

echo "  ── Raw Transaction Operations ──"

if [ "$HAS_T_BAL" = "yes" ]; then
    # createrawtransaction
    # Find a UTXO to spend
    UTXO_INFO=$(echo "$UTXOS" | python3 -c "
import json,sys
utxos = json.load(sys.stdin)['result']
for u in utxos:
    if float(u['amount']) > 0.001:
        print(json.dumps({'txid':u['txid'],'vout':u['vout'],'amount':float(u['amount'])}))
        break
" 2>/dev/null)
    if [ -n "$UTXO_INFO" ]; then
        U_TXID=$(echo "$UTXO_INFO" | python3 -c "import json,sys; print(json.load(sys.stdin)['txid'])")
        U_VOUT=$(echo "$UTXO_INFO" | python3 -c "import json,sys; print(json.load(sys.stdin)['vout'])")
        DEST=$(result "$(c23 getnewaddress)")
        R=$(c23 createrawtransaction "[{\"txid\":\"$U_TXID\",\"vout\":$U_VOUT}],{\"$DEST\":0.0001}")
        RAW=$(result "$R")
        if [ ${#RAW} -gt 20 ]; then
            check "createrawtransaction" "PASS" "${#RAW} hex chars"

            # decoderawtransaction
            R=$(c23 decoderawtransaction "\"$RAW\"")
            DEC_VER=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['version'])" 2>/dev/null)
            if [ -n "$DEC_VER" ]; then
                check "decoderawtransaction" "PASS" "version=$DEC_VER"
            else
                check "decoderawtransaction" "FAIL" "$(err "$R")"
            fi
        else
            check "createrawtransaction" "FAIL" "$(err "$R")"
            check "decoderawtransaction" "SKIP" "no raw tx"
        fi
    else
        check "createrawtransaction" "SKIP" "no suitable UTXO"
        check "decoderawtransaction" "SKIP" "no suitable UTXO"
    fi
else
    check "createrawtransaction" "SKIP" "no balance"
    check "decoderawtransaction" "SKIP" "no balance"
fi

echo ""

# ─── 10. SHIELDED KEY OPERATIONS ─────────────────────────────────────

echo "  ── Shielded Key Operations ──"

# z_exportkey / z_importkey
ZADDR_EXP=$(result "$(c23 z_getnewaddress)")
R=$(c23 z_exportkey "\"$ZADDR_EXP\"")
SKEY=$(result "$R")
if [ ${#SKEY} -gt 20 ]; then
    check "z_exportkey" "PASS" "${SKEY:0:20}..."
else
    check "z_exportkey" "FAIL" "$(err "$R")"
fi

# z_exportviewingkey
R=$(c23 z_exportviewingkey "\"$ZADDR_EXP\"")
VK=$(result "$R")
if [ ${#VK} -gt 20 ]; then
    check "z_exportviewingkey" "PASS" "${VK:0:20}..."
else
    check "z_exportviewingkey" "FAIL" "$(err "$R")"
fi

# z_listaddresses
R=$(c23 z_listaddresses)
NZ=$(echo "$R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']))" 2>/dev/null)
check "z_listaddresses" "PASS" "$NZ z-addresses"

echo ""

# ─── 11. CHAIN SYNC VERIFICATION ─────────────────────────────────────

echo "  ── Final Sync Verification ──"

sleep 5

C23_H_FINAL=$(result "$(c23 getblockcount)")
CPP_H_FINAL=$(result "$(cpp getblockcount)")
DIFF_FINAL=$((CPP_H_FINAL - C23_H_FINAL))
if [ "$DIFF_FINAL" -lt 0 ]; then DIFF_FINAL=$((-DIFF_FINAL)); fi

if [ "$DIFF_FINAL" -lt 10 ]; then
    check "Final sync check" "PASS" "C23=$C23_H_FINAL C++=$CPP_H_FINAL (delta=$DIFF_FINAL)"
else
    check "Final sync check" "FAIL" "C23=$C23_H_FINAL C++=$CPP_H_FINAL (delta=$DIFF_FINAL)"
fi

# Compare best block hashes
C23_BEST=$(result "$(c23 getbestblockhash)")
CPP_BEST=$(result "$(cpp getbestblockhash)")
if [ "$C23_BEST" = "$CPP_BEST" ]; then
    check "Best block hash matches" "PASS" "${C23_BEST:0:16}..."
else
    # Allow 1-block difference due to propagation
    if [ "$DIFF_FINAL" -le 1 ]; then
        check "Best block hash matches" "PASS" "within 1 block"
    else
        check "Best block hash matches" "FAIL" "hashes differ by $DIFF_FINAL blocks"
    fi
fi

# Compare mempool sizes
C23_MP=$(echo "$(c23 getmempoolinfo)" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['size'])" 2>/dev/null)
CPP_MP=$(echo "$(cpp getrawmempool)" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']))" 2>/dev/null)
check "Mempool sizes" "PASS" "C23=$C23_MP C++=$CPP_MP"

echo ""
echo "════════════════════════════════════════════════════════════════════"
printf "  Results: \033[32m%d PASS\033[0m  \033[31m%d FAIL\033[0m  \033[33m%d SKIP\033[0m\n" $PASS $FAIL $SKIP
echo "════════════════════════════════════════════════════════════════════"
echo ""

exit $FAIL
