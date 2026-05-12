#!/bin/bash
# Backfill sprout_value and sapling_value in zclassic23 SQLite
# by querying zclassicd for block data with JoinSplits/Sapling activity.
#
# Usage: ./tools/backfill_shielded.sh
# Requires: zclassicd running on port 8232, zclassic23 running on port 18232

set -e

ZCD_AUTH="zclrhett:zclrhettpass2026"
ZCD_URL="http://127.0.0.1:8232"
ZCL_COOKIE=$(cat ~/.zclassic-c23/.cookie 2>/dev/null)
ZCL_URL="http://127.0.0.1:18232"

rpc_zcd() {
    curl -s -u "$ZCD_AUTH" --data-binary "$1" "$ZCD_URL"
}

rpc_zcl() {
    curl -s -u "$ZCL_COOKIE" --data-binary "$1" "$ZCL_URL"
}

TIP=$(rpc_zcd '{"jsonrpc":"1.0","method":"getblockcount","params":[],"id":1}' | python3 -c "import json,sys; print(json.load(sys.stdin)['result'])")
echo "Chain tip: $TIP"
echo "Scanning for Sprout/Sapling activity..."

# Scan blocks in batches
BATCH=1000
FOUND=0

for ((start=1; start<=TIP; start+=BATCH)); do
    end=$((start + BATCH - 1))
    if [ $end -gt $TIP ]; then end=$TIP; fi

    # For each block in range, check if it has joinsplits or sapling activity
    for ((h=start; h<=end; h++)); do
        HASH=$(rpc_zcd "{\"jsonrpc\":\"1.0\",\"method\":\"getblockhash\",\"params\":[$h],\"id\":1}" | python3 -c "import json,sys; print(json.load(sys.stdin)['result'])" 2>/dev/null)
        [ -z "$HASH" ] && continue

        # Get block with tx details
        DATA=$(rpc_zcd "{\"jsonrpc\":\"1.0\",\"method\":\"getblock\",\"params\":[\"$HASH\",2],\"id\":1}" 2>/dev/null)

        VALUES=$(echo "$DATA" | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)['result']
    sprout=0; sapling=0
    for tx in d['tx']:
        for js in tx.get('vjoinsplit',[]):
            sprout += int(round(js['vpub_old']*1e8))
            sprout -= int(round(js['vpub_new']*1e8))
        vb = tx.get('valueBalance', 0)
        if vb: sapling += int(round(vb*1e8))
    if sprout or sapling:
        print(f'{d[\"height\"]} {sprout} {sapling}')
except: pass
" 2>/dev/null)

        if [ -n "$VALUES" ]; then
            HEIGHT=$(echo "$VALUES" | awk '{print $1}')
            SPROUT=$(echo "$VALUES" | awk '{print $2}')
            SAPLING=$(echo "$VALUES" | awk '{print $3}')
            echo "h=$HEIGHT sprout=$SPROUT sapling=$SAPLING"
            FOUND=$((FOUND + 1))
        fi
    done

    echo "  ... scanned to height $end ($FOUND blocks with activity)"
done

echo "Done. Found $FOUND blocks with shielded activity."
