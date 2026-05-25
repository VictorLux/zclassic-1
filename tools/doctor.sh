#!/usr/bin/env bash
# doctor.sh — ONE command that always answers the three questions we kept
# hand-investigating: (1) is the running node a STALE binary? (2) is the tip
# advancing? (3) if not, WHAT is blocking it right now. No more spelunking.
#
#   tools/doctor.sh
#   exit 0 healthy/advancing · 2 stale binary · 3 wedged
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RPC="$ROOT/tools/zcl-rpc"
DATADIR="${ZCL_DATADIR:-$HOME/.zclassic-c23}"
LOG="$DATADIR/node.log"
BIN="$ROOT/zclassic23"
GAP_OK="${GAP_OK:-2}"
num(){ sed -n "s/.*\"$1\":\([0-9-]*\).*/\1/p"; }

verdict=0

# ── BINARY PROVENANCE ───────────────────────────────────────────────
pid=$(systemctl --user show zclassic23 -p MainPID --value 2>/dev/null)
[ "${pid:-0}" -gt 0 ] 2>/dev/null || pid=""
exe_sha=$(sha256sum "/proc/${pid:-x}/exe" 2>/dev/null | cut -c1-12)
bin_sha=$(sha256sum "$BIN" 2>/dev/null | cut -c1-12)
bin_mtime=$(stat -c %Y "$BIN" 2>/dev/null || echo 0)
proc_start=$(ps -o lstart= -p "${pid:-x}" 2>/dev/null | sed 's/^ *//')
newest_src=$(find "$ROOT/app" "$ROOT/lib" "$ROOT/config" "$ROOT/tools" -name '*.c' -o -name '*.h' 2>/dev/null \
             | xargs stat -c %Y 2>/dev/null | sort -rn | head -1)
local_head=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)
origin_head=$(git -C "$ROOT" rev-parse --short origin/main 2>/dev/null)

proc_stale="no — process IS the on-disk binary"
if [ -n "$pid" ] && [ -n "$exe_sha" ] && [ "$exe_sha" != "$bin_sha" ]; then
    proc_stale="⚠ YES — on-disk binary changed since start → RESTART to load it"; verdict=2
fi
bin_stale="no — binary newer than all source"
if [ "${newest_src:-0}" -gt "${bin_mtime:-0}" ]; then
    bin_stale="⚠ YES — source newer than binary → REBUILD (make -j)"; verdict=2
fi
head_drift="in sync"
[ -n "$origin_head" ] && [ "$local_head" != "$origin_head" ] && head_drift="⚠ local $local_head ≠ origin $origin_head (pull/rebuild)"

# ── CHAIN: sample tip twice to prove movement ───────────────────────
ch1=$("$RPC" getblockcount 2>/dev/null | num result)
legacy=$("$RPC" getsyncdiag 2>/dev/null | num legacy_height)
sleep "${SAMPLE_GAP_SECS:-5}"
ch2=$("$RPC" getblockcount 2>/dev/null | num result)
: "${ch1:=0}" "${ch2:=0}" "${legacy:=0}"
gap=$(( legacy > ch2 ? legacy - ch2 : 0 ))
adv=$(( ch2 - ch1 ))

# ── BLOCKER: read the live truth from the log ───────────────────────
blocker="none"
if [ -z "$pid" ]; then
    blocker="NODE DOWN (systemd MainPID=0)"; verdict=3
elif [ "$adv" -le 0 ] && [ "$gap" -gt "$GAP_OK" ]; then
    verdict=3
    last=$(grep -aE "evidence controller rejected|csr-tip-commit-rejected|connect_block FAILED|bad-txns-[a-z0-9-]+|STALL: h=" "$LOG" 2>/dev/null | tail -1)
    case "$last" in
        *"(frozen)"*|*csr-tip-commit-rejected*) blocker="evidence controller FROZEN — refusing tip commit (stale flag; no thaw path)";;
        *bad-txns-BIP30*)                       blocker="bad-txns-BIP30 — coinbase self-write at tip+1";;
        *"connect_block FAILED"*)               blocker="connect_block failed: ${last##*FAILED }";;
        *"STALL: h="*)                          blocker="STALL: ${last##*STALL: }";;
        *)                                      blocker="stuck — last log: ${last:-<none>}";;
    esac
fi

cat <<EOF
ZCLASSIC23 · doctor · $(date -u +%H:%M:%SZ)

BINARY
  running process    pid ${pid:-DOWN}  exe ${exe_sha:-?}  started ${proc_start:-?}
  on-disk binary     sha ${bin_sha:-?}  built $(date -d @"${bin_mtime:-0}" -u +%H:%M:%SZ 2>/dev/null)
  process stale?     $proc_stale
  binary vs source?  $bin_stale
  commit             local $local_head · origin $origin_head · $head_drift

CHAIN
  tip / legacy       $ch2 / $legacy   (gap $gap)
  advancing?         $( [ "$adv" -gt 0 ] && echo "✓ +$adv in ${SAMPLE_GAP_SECS:-5}s" || echo "✗ flat in ${SAMPLE_GAP_SECS:-5}s" )
  blocking now       $blocker

VERDICT: $( case $verdict in 0) echo "HEALTHY — at tip, fresh binary";; 2) echo "⚠ STALE BINARY — rebuild/restart before trusting live behavior";; *) echo "🚨 WEDGED — $blocker";; esac )
EOF
exit "$verdict"
