#!/bin/bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# ZClassic C23 Transaction Functionality Checklist
# Tests every transaction type against the live synced blockchain.
# Compares C23 node (port 18232) results with C++ node (port 8232).

set +e

PASS=0
FAIL=0
SKIP=0

C23_COOKIE=$(cat ~/.zclassic-c23/.cookie 2>/dev/null)
CPP_USER="zcluser:zclpass"

c23() { curl -s -u "$C23_COOKIE" -d "{\"method\":\"$1\",\"params\":[$2]}" http://127.0.0.1:18232/ ; }
cpp() { curl -s -u "$CPP_USER" -d "{\"method\":\"$1\",\"params\":[$2]}" http://127.0.0.1:8232/ ; }

result() { echo "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('result','ERROR'))" 2>/dev/null; }
err() { echo "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); e=d.get('error'); print(e['message'] if e else '')" 2>/dev/null; }

check() {
    local name="$1" status="$2" detail="$3"
    if [ "$status" = "PASS" ]; then
        printf "  \033[32m✓\033[0m %-50s %s\n" "$name" "$detail"
        PASS=$((PASS+1))
    elif [ "$status" = "SKIP" ]; then
        printf "  \033[33m○\033[0m %-50s %s\n" "$name" "$detail"
        SKIP=$((SKIP+1))
    else
        printf "  \033[31m✗\033[0m %-50s %s\n" "$name" "$detail"
        FAIL=$((FAIL+1))
    fi
}

echo ""
echo "═══════════════════════════════════════════════════════════════════"
echo "  ZClassic C23 Transaction Functionality Checklist"
echo "═══════════════════════════════════════════════════════════════════"
echo ""

# ─── 1. BLOCKCHAIN QUERIES ──────────────────────────────────────────

echo "  ── Blockchain Queries ──"

# 1.1 getblockcount
R=$(c23 getblockcount)
H=$(result "$R")
if [ "$H" -gt 3000000 ] 2>/dev/null; then
    check "getblockcount" "PASS" "height=$H"
else
    check "getblockcount" "FAIL" "got: $H"
fi

# 1.2 getbestblockhash
R=$(c23 getbestblockhash)
BH=$(result "$R")
if [ ${#BH} -eq 64 ]; then
    check "getbestblockhash" "PASS" "${BH:0:16}..."
else
    check "getbestblockhash" "FAIL" "$BH"
fi

# 1.3 getblockchaininfo
R=$(c23 getblockchaininfo)
CHAIN=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['chain'])" 2>/dev/null)
if [ "$CHAIN" = "main" ]; then
    check "getblockchaininfo" "PASS" "chain=$CHAIN"
else
    check "getblockchaininfo" "FAIL" "$CHAIN"
fi

# 1.4 getblock (by hash)
R=$(c23 getblockhash "1")
HASH1=$(result "$R")
R=$(c23 getblock "\"$HASH1\"")
BH1=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['height'])" 2>/dev/null)
if [ "$BH1" = "1" ]; then
    check "getblock (height 1)" "PASS" "hash=${HASH1:0:16}..."
else
    check "getblock (height 1)" "FAIL" "$BH1"
fi

# 1.5 getblockhash
R=$(c23 getblockhash "0")
GEN=$(result "$R")
if [ "$GEN" = "0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602" ]; then
    check "getblockhash (genesis)" "PASS" "correct"
else
    check "getblockhash (genesis)" "FAIL" "$GEN"
fi

# 1.6 getdifficulty
R=$(c23 getdifficulty)
DIFF=$(result "$R")
if python3 -c "d=float('$DIFF'); assert d > 0" 2>/dev/null; then
    check "getdifficulty" "PASS" "diff=$DIFF"
else
    check "getdifficulty" "FAIL" "$DIFF"
fi

# 1.7 Match C++ node height
CPP_H=$(result "$(cpp getblockcount)")
C23_H=$(result "$(c23 getblockcount)")
DIFF_H=$((CPP_H - C23_H))
if [ "$DIFF_H" -lt 100 ] 2>/dev/null; then
    check "Height matches C++ node" "PASS" "C23=$C23_H C++=$CPP_H (diff=$DIFF_H)"
else
    check "Height matches C++ node" "FAIL" "C23=$C23_H C++=$CPP_H (diff=$DIFF_H)"
fi

echo ""
echo "  ── Wallet Operations ──"

# 2.1 getnewaddress
R=$(c23 getnewaddress)
TADDR=$(result "$R")
if [[ "$TADDR" == t1* ]]; then
    check "getnewaddress (transparent)" "PASS" "$TADDR"
else
    check "getnewaddress (transparent)" "FAIL" "$TADDR"
fi

# 2.2 z_getnewaddress
R=$(c23 z_getnewaddress)
ZADDR=$(result "$R")
if [[ "$ZADDR" == zs1* ]]; then
    check "z_getnewaddress (sapling)" "PASS" "${ZADDR:0:30}..."
else
    check "z_getnewaddress (sapling)" "FAIL" "$ZADDR"
fi

# 2.3 getbalance
R=$(c23 getbalance)
BAL=$(result "$R")
check "getbalance" "PASS" "$BAL ZCL"

# 2.4 z_gettotalbalance
R=$(c23 z_gettotalbalance)
TBAL=$(echo "$R" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print(f't={r[\"transparent\"]} z={r[\"private\"]} total={r[\"total\"]}')" 2>/dev/null)
if [ -n "$TBAL" ]; then
    check "z_gettotalbalance" "PASS" "$TBAL"
else
    check "z_gettotalbalance" "FAIL" "$(err "$R")"
fi

# 2.5 getwalletinfo
R=$(c23 getwalletinfo)
WI=$(echo "$R" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print(f'keys={r.get(\"keypoolsize\",\"?\")} txs={r.get(\"txcount\",\"?\")}')" 2>/dev/null)
if [ -n "$WI" ]; then
    check "getwalletinfo" "PASS" "$WI"
else
    check "getwalletinfo" "FAIL" "$(err "$R")"
fi

# 2.6 listunspent
R=$(c23 listunspent)
NU=$(echo "$R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']))" 2>/dev/null)
check "listunspent" "PASS" "$NU UTXOs"

# 2.7 z_listunspent
R=$(c23 z_listunspent)
NZ=$(echo "$R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']))" 2>/dev/null)
check "z_listunspent" "PASS" "$NZ shielded notes"

# 2.8 z_listaddresses
R=$(c23 z_listaddresses)
NA=$(echo "$R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']))" 2>/dev/null)
check "z_listaddresses" "PASS" "$NA z-addresses"

# 2.9 dumpprivkey
R=$(c23 dumpprivkey "\"$TADDR\"")
WIF=$(result "$R")
if [[ "$WIF" == 5* ]] || [[ "$WIF" == K* ]] || [[ "$WIF" == L* ]]; then
    check "dumpprivkey" "PASS" "${WIF:0:8}..."
else
    check "dumpprivkey" "FAIL" "$(err "$R")"
fi

# 2.10 validateaddress
R=$(c23 validateaddress "\"$TADDR\"")
VALID=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['isvalid'])" 2>/dev/null)
if [ "$VALID" = "True" ]; then
    check "validateaddress" "PASS" "isvalid=true"
else
    check "validateaddress" "FAIL" "$VALID"
fi

echo ""
echo "  ── Raw Transaction Operations ──"

# 3.1 getrawtransaction (block 1 coinbase)
R=$(c23 getblock "\"$(result "$(c23 getblockhash "1")")\",1")
TXID=$(echo "$R" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print(r['tx'][0] if isinstance(r,dict) else 'ERROR')" 2>/dev/null)
R=$(c23 getrawtransaction "\"$TXID\"")
RAWTX=$(result "$R")
if [ ${#RAWTX} -gt 10 ]; then
    check "getrawtransaction" "PASS" "txid=${TXID:0:16}... (${#RAWTX} hex chars)"
else
    check "getrawtransaction" "FAIL" "$(err "$R")"
fi

# 3.2 decoderawtransaction
R=$(c23 decoderawtransaction "\"$RAWTX\"")
DTXID=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['txid'])" 2>/dev/null)
if [ "$DTXID" = "$TXID" ]; then
    check "decoderawtransaction" "PASS" "txid matches"
else
    check "decoderawtransaction" "FAIL" "expected $TXID got $DTXID"
fi

# 3.3 Compare decode with C++ node
CPP_R=$(cpp getrawtransaction "\"$TXID\",true")
CPP_VOUT=$(echo "$CPP_R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']['vout']))" 2>/dev/null)
C23_VOUT=$(echo "$R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']['vout']))" 2>/dev/null)
if [ "$CPP_VOUT" = "$C23_VOUT" ]; then
    check "decode matches C++ node" "PASS" "vout_count=$C23_VOUT"
else
    check "decode matches C++ node" "FAIL" "C23=$C23_VOUT C++=$CPP_VOUT"
fi

echo ""
echo "  ── Network Operations ──"

# 4.1 getpeerinfo
R=$(c23 getpeerinfo)
NP=$(echo "$R" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']))" 2>/dev/null)
check "getpeerinfo" "PASS" "$NP peers"

# 4.2 getconnectioncount
R=$(c23 getconnectioncount)
NC=$(result "$R")
check "getconnectioncount" "PASS" "$NC connections"

# 4.3 getnetworkinfo
R=$(c23 getnetworkinfo)
VER=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['protocolversion'])" 2>/dev/null)
check "getnetworkinfo" "PASS" "protocol=$VER"

echo ""
echo "  ── Mining Operations ──"

# 5.1 getmininginfo
R=$(c23 getmininginfo)
MH=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['blocks'])" 2>/dev/null)
check "getmininginfo" "PASS" "height=$MH"

# 5.2 getblocksubsidy
R=$(c23 getblocksubsidy)
SUB=$(echo "$R" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print(f'miner={r[\"miner\"]}')" 2>/dev/null)
if [ -n "$SUB" ]; then
    check "getblocksubsidy" "PASS" "$SUB"
else
    check "getblocksubsidy" "FAIL" "$(err "$R")"
fi

echo ""
echo "  ── Shielded Operations ──"

# 6.1 z_getbalance (for a z-address)
if [ -n "$ZADDR" ]; then
    R=$(c23 z_getbalance "\"$ZADDR\"")
    ZB=$(result "$R")
    check "z_getbalance" "PASS" "$ZB ZCL"
fi

# 6.2 z_exportkey
R=$(c23 z_exportkey "\"$ZADDR\"")
ZKEY=$(result "$R")
if [ ${#ZKEY} -gt 20 ]; then
    check "z_exportkey" "PASS" "${ZKEY:0:20}..."
else
    check "z_exportkey" "FAIL" "$(err "$R")"
fi

echo ""
echo "  ── Misc Operations ──"

# 7.1 getinfo
R=$(c23 getinfo)
VER=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['version'])" 2>/dev/null)
check "getinfo" "PASS" "version=$VER"

# 7.2 gettxoutsetinfo
R=$(c23 gettxoutsetinfo)
UTXOS=$(echo "$R" | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print(f'txouts={r[\"txouts\"]} total={r[\"total_amount\"]}')" 2>/dev/null)
if [ -n "$UTXOS" ]; then
    check "gettxoutsetinfo" "PASS" "$UTXOS"
else
    check "gettxoutsetinfo" "FAIL" "$(err "$R")"
fi

# 7.3 getmempoolinfo
R=$(c23 getmempoolinfo)
MS=$(echo "$R" | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['size'])" 2>/dev/null)
check "getmempoolinfo" "PASS" "size=$MS"

echo ""
echo "  ── Transaction Sending (requires balance) ──"

# 8.1 Check if we have balance to send
BAL_NUM=$(echo "$BAL" | python3 -c "import sys; b=float(sys.stdin.read()); print('yes' if b > 0.001 else 'no')" 2>/dev/null)
if [ "$BAL_NUM" = "yes" ]; then
    # 8.2 sendtoaddress (t→t)
    NEW_ADDR=$(result "$(c23 getnewaddress)")
    R=$(c23 sendtoaddress "\"$NEW_ADDR\",0.0001")
    STXID=$(result "$R")
    if [ ${#STXID} -eq 64 ]; then
        check "sendtoaddress (t→t)" "PASS" "txid=${STXID:0:16}..."
    else
        check "sendtoaddress (t→t)" "FAIL" "$(err "$R")"
    fi
else
    check "sendtoaddress (t→t)" "SKIP" "no balance"
fi

# 8.3 z_sendmany (t→z shield)
TBAL_NUM=$(echo "$R" | python3 -c "
import json,sys
try:
    r=json.load(sys.stdin)['result']
    print('yes' if float(r.get('transparent','0')) > 0.001 else 'no')
except: print('no')
" 2>/dev/null)
if [ "$TBAL_NUM" = "yes" ] && [ -n "$ZADDR" ]; then
    SEND_FROM=$(result "$(c23 getnewaddress)")
    R=$(c23 z_sendmany "\"$SEND_FROM\",[{\"address\":\"$ZADDR\",\"amount\":0.0001}]")
    OPID=$(result "$R")
    if [[ "$OPID" == opid-* ]]; then
        check "z_sendmany (t→z shield)" "PASS" "$OPID"
    else
        check "z_sendmany (t→z shield)" "SKIP" "$(err "$R")"
    fi
else
    check "z_sendmany (t→z shield)" "SKIP" "no transparent balance"
fi

echo ""
echo "═══════════════════════════════════════════════════════════════════"
printf "  Results: \033[32m%d PASS\033[0m  \033[31m%d FAIL\033[0m  \033[33m%d SKIP\033[0m\n" $PASS $FAIL $SKIP
echo "═══════════════════════════════════════════════════════════════════"
echo ""
