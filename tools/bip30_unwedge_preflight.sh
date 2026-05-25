#!/usr/bin/env bash
# bip30_unwedge_preflight.sh — read-only evidence for the stale-coinbase
# BIP30 unwedge. This does not deploy, restart, mutate SQLite, or touch the
# service; it only reports whether the live node still has the exact
# tip+1 stale UTXO shape and whether the running process is the expected
# deployed binary.
#
# Exit codes:
#   0  no stale tip+1 row and live tip appears healthy
#   2  live RPC / DB could not be read
#   3  stale tip+1 row still present; deploy/restart unwedge still needed
#   4  stale row absent, but live tip is still unhealthy
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATADIR="${ZCL_DATADIR:-$HOME/.zclassic-c23}"
SERVICE="${ZCL_SERVICE:-zclassic23}"
DEPLOYED_BIN="${ZCL_DEPLOYED_BIN:-$HOME/zclassic23/zclassic23}"
WORKTREE_BIN="${ZCL_WORKTREE_BIN:-$ROOT/zclassic23}"
RPC="$ROOT/tools/zcl-rpc"
NODE_DB="$DATADIR/node.db"
NODE_LOG="$DATADIR/node.log"

sha_line() {
    local path="$1"
    if [[ ! -e "$path" ]]; then
        printf 'missing'
        return
    fi
    sha256sum "$path" 2>/dev/null | awk '{print $1}'
}

mtime_line() {
    local path="$1"
    if [[ ! -e "$path" ]]; then
        printf 'missing'
        return
    fi
    stat -Lc '%s bytes %y' "$path" 2>/dev/null || printf 'unreadable'
}

json_num() {
    local key="$1"
    python3 -c '
import json, sys
key = sys.argv[1]
try:
    blob = json.loads(sys.stdin.read())
except Exception:
    print(0)
    raise SystemExit
r = blob.get("result") if isinstance(blob, dict) else None
if not isinstance(r, dict):
    r = blob if isinstance(blob, dict) else {}
print(r.get(key, 0))
' "$key"
}

rpc_num_result() {
    local method="$1"
    "$RPC" "$method" 2>/dev/null | python3 -c '
import json, sys
try:
    blob = json.loads(sys.stdin.read())
except Exception:
    print(-1)
    raise SystemExit
print(blob.get("result", -1) if isinstance(blob, dict) else -1)
'
}

read_db_facts() {
    python3 - "$NODE_DB" <<'PY'
import sqlite3
import sys

path = sys.argv[1]
try:
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=2.0)
    cur = conn.cursor()
    cur.execute("SELECT COALESCE(MAX(height),-1) FROM blocks")
    tip = int(cur.fetchone()[0])
    cur.execute(
        "SELECT COUNT(*), COALESCE(MIN(height),-1), COALESCE(MAX(height),-1) "
        "FROM utxos WHERE height > ?",
        (tip,),
    )
    above_count, above_min, above_max = cur.fetchone()
    cur.execute("SELECT COUNT(*) FROM utxos WHERE height = ?", (tip + 1,))
    tip1_count = int(cur.fetchone()[0])
    cur.execute("SELECT COUNT(*) FROM transactions WHERE block_height > ?", (tip,))
    tx_above = int(cur.fetchone()[0])
    cur.execute("SELECT COUNT(*) FROM node_state WHERE key='utxo_commitment'")
    commitment_rows = int(cur.fetchone()[0])
except Exception as exc:
    print(f"DB_ERROR={exc}")
    raise SystemExit(2)
finally:
    try:
        conn.close()
    except Exception:
        pass

print(f"DB_TIP={tip}")
print(f"UTXO_ABOVE_COUNT={int(above_count)}")
print(f"UTXO_ABOVE_MIN={int(above_min)}")
print(f"UTXO_ABOVE_MAX={int(above_max)}")
print(f"UTXO_TIP_PLUS_ONE_COUNT={tip1_count}")
print(f"TX_ABOVE_TIP_COUNT={tx_above}")
print(f"UTXO_COMMITMENT_ROWS={commitment_rows}")
PY
}

echo "bip30_unwedge_preflight · read-only · $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo

pid="$(systemctl --user show "$SERVICE" -p MainPID --value 2>/dev/null || true)"
if [[ -z "$pid" || "$pid" = "0" ]]; then
    echo "service: $SERVICE not running"
    running_sha="missing"
else
    echo "service: $SERVICE pid=$pid"
    running_sha="$(sha_line "/proc/$pid/exe")"
    echo "running_exe_sha: $running_sha"
    echo "running_exe_stat: $(mtime_line "/proc/$pid/exe")"
fi
echo "deployed_bin: $DEPLOYED_BIN"
echo "deployed_sha: $(sha_line "$DEPLOYED_BIN")"
echo "deployed_stat: $(mtime_line "$DEPLOYED_BIN")"
echo "worktree_bin: $WORKTREE_BIN"
echo "worktree_sha: $(sha_line "$WORKTREE_BIN")"
echo "worktree_stat: $(mtime_line "$WORKTREE_BIN")"
echo

if [[ ! -r "$NODE_DB" ]]; then
    echo "DB_ERROR=node.db not readable: $NODE_DB" >&2
    exit 2
fi

db_facts="$(read_db_facts)" || exit 2
printf '%s\n' "$db_facts"
eval "$db_facts"
echo

syncdiag="$("$RPC" getsyncdiag 2>/dev/null || true)"
if [[ -z "$syncdiag" ]]; then
    echo "RPC_ERROR=getsyncdiag failed" >&2
    exit 2
fi
chain_height="$(printf '%s' "$syncdiag" | json_num chain_height)"
legacy_height="$(printf '%s' "$syncdiag" | json_num legacy_height)"
peer_max_height="$(printf '%s' "$syncdiag" | json_num peer_max_height)"
rpc_height="$(rpc_num_result getblockcount)"
gap=$(( legacy_height > rpc_height ? legacy_height - rpc_height : 0 ))
echo "RPC_CHAIN_HEIGHT=$chain_height"
echo "RPC_GETBLOCKCOUNT=$rpc_height"
echo "RPC_LEGACY_HEIGHT=$legacy_height"
echo "RPC_PEER_MAX_HEIGHT=$peer_max_height"
echo "RPC_GAP=$gap"
echo

if [[ -r "$NODE_LOG" ]]; then
    echo "recent_wedge_log_lines:"
    tail -300 "$NODE_LOG" \
        | grep -E "bad-txns-BIP30|STALL: h=${DB_TIP}|Boot: removed|refusing boot UTXO rewind|auto-rewind" \
        | tail -12 \
        || true
    echo
fi

stale_tip1=0
if [[ "$UTXO_ABOVE_COUNT" -eq 1 &&
      "$UTXO_ABOVE_MIN" -eq $((DB_TIP + 1)) &&
      "$UTXO_ABOVE_MAX" -eq $((DB_TIP + 1)) &&
      "$UTXO_TIP_PLUS_ONE_COUNT" -ge 1 &&
      "$TX_ABOVE_TIP_COUNT" -eq 0 ]]; then
    stale_tip1=1
fi

if [[ "$stale_tip1" -eq 1 ]]; then
    echo "VERDICT=STALE_TIP_PLUS_ONE_PRESENT"
    echo "next=operator-approved deploy/restart should run the boot rewind, then rerun this tool plus scoreboard"
    exit 3
fi

if [[ "$gap" -le 2 && "$rpc_height" -ge "$DB_TIP" ]]; then
    echo "VERDICT=NO_STALE_ROW_AND_LIVE_HEALTHY"
    exit 0
fi

echo "VERDICT=NO_STALE_ROW_BUT_LIVE_UNHEALTHY"
exit 4
