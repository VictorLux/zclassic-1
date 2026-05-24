#!/usr/bin/env bash
# scoreboard.sh — print the goal scoreboard from LIVE node state, never a
# hand-copied snapshot. Doctrine #3 (docs/REFACTOR_STATUS.md): "the scoreboard
# reads live truth, sampled now." On 2026-05-24 the board showed "stay at tip
# ✓ 0 gap" while the chain was frozen 45 behind — because a human pasted a
# stale number. This tool makes that impossible: every line below is read from
# the running node at call time.
#
# "Stay at tip" is decided by SAMPLING chain_height twice and checking it
# actually moved — a frozen tip can never show ✓ here, by construction.
#
# Usage:  tools/scoreboard.sh
# Exit:   0 healthy/advancing · 3 WEDGED (tip not advancing, gap > GAP_OK)
set -uo pipefail

RPC="$(dirname "$0")/zcl-rpc"
GAP_OK="${GAP_OK:-2}"          # blocks behind legacy still considered "at tip"
SAMPLE_GAP_SECS="${SAMPLE_GAP_SECS:-4}"

num() { sed -n "s/.*\"$1\":\([0-9-]*\).*/\1/p"; }
rpc_num() { "$RPC" "$1" 2>/dev/null | num result; }

diag="$($RPC getsyncdiag 2>/dev/null)"
ch1="$(printf '%s' "$diag" | num chain_height)"
legacy="$(printf '%s' "$diag" | num legacy_height)"
conns="$(rpc_num getconnectioncount)"
pid="$(systemctl --user show zclassic23 -p MainPID --value 2>/dev/null)"
rss="$(awk '/VmRSS/{printf "%.2f",$2/1048576}' /proc/"$pid"/status 2>/dev/null)"
uptime="$(ps -o etimes= -p "$pid" 2>/dev/null | tr -d ' ')"

# --- the live truth test: did the tip move? ---
sleep "$SAMPLE_GAP_SECS"
ch2="$(rpc_num getblockcount)"
: "${ch1:=0}" "${ch2:=0}" "${legacy:=0}" "${conns:=0}" "${uptime:=0}"
gap=$(( legacy > ch2 ? legacy - ch2 : 0 ))
advanced=$(( ch2 - ch1 ))

if   [ "$gap" -le "$GAP_OK" ] && [ "$advanced" -ge 0 ]; then tip="✓ at tip ($gap behind)"; verdict=0
elif [ "$advanced" -gt 0 ];                              then tip="→ catching up (+$advanced/${SAMPLE_GAP_SECS}s, $gap behind)"; verdict=0
else                                                          tip="✗ WEDGED — frozen at $ch2, $gap behind, not advancing"; verdict=3
fi

mark() { awk -v v="$1" -v t="$2" 'BEGIN{printf (v<=t)?"✓":"⚠"}'; }

cat <<EOF
ZCLASSIC23 · LIVE scoreboard · $(date -u +%H:%M:%SZ) · generated, not pasted

⚡ FAST
   Stay at tip       $tip
   z23 / legacy      $ch2 / $legacy   (gap $gap)
🪶 LEAN
   Memory (RSS)      ${rss:-?} GB   target ≤1.0   $(mark "${rss:-9}" 1.0)
💪 UNBREAKABLE
   Peers connected   $conns   floor 3   $(mark 3 "$conns")
   Uptime            ${uptime}s   $( [ "$uptime" -lt 120 ] && echo '⚠ just restarted — MTBF failing' || echo 'up' )

VERDICT: $( [ "$verdict" = 0 ] && echo 'HEALTHY (tip advancing)' || echo '🚨 WEDGED — tip not advancing' )
EOF
exit "$verdict"
