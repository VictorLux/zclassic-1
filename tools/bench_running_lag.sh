#!/usr/bin/env bash
# bench_running_lag.sh — sample chain_height and peer_max_height every 30 s
# for 10 min against the live `zclassic23` MCP and fail if the gap does not
# monotonically decrease.
#
# This is the CI guard for the failure mode the round-1 + round-2 sync work
# was designed to prevent: a node that "stays in sync" only on paper — peers
# up, watchdog ticking — yet chain_height does not advance toward
# peer_max_height. The earlier 9 h block_download stall would have tripped
# this in the first two samples.
#
# Acceptance:
#   * SAMPLES (default 20) reads of zcl_syncdiag spaced INTERVAL_SECS apart
#   * peer_max_height must be > 0 by the second sample (peers handshake)
#   * (peer_max_height - chain_height) must decrease net of mining; we
#     allow up to MINING_TOLERANCE blocks of "wash" each interval to absorb
#     a new mined block on the upstream tip
#
# Exit codes:
#   0  OK
#   1  no MCP / can't read syncdiag
#   2  peer_max_height never populated (handshake broken)
#   3  gap grew when it should have shrunk

set -uo pipefail

SAMPLES="${SAMPLES:-20}"
INTERVAL_SECS="${INTERVAL_SECS:-30}"
MINING_TOLERANCE="${MINING_TOLERANCE:-3}"

DATADIR="${ZCL_DATADIR:-$HOME/.zclassic-c23}"
RPCPORT="${ZCL_RPCPORT:-18232}"
RPCURL="${RPCURL:-http://127.0.0.1:${RPCPORT}}"
COOKIE_FILE="${COOKIE_FILE:-${DATADIR}/.cookie}"

if [[ ! -r "${COOKIE_FILE}" ]]; then
    echo "[bench_running_lag] cookie not readable: ${COOKIE_FILE}" >&2
    exit 1
fi
COOKIE="$(cat "${COOKIE_FILE}")"

call_syncdiag() {
    curl -s --max-time 5 --user "${COOKIE}" \
        -H 'content-type: text/plain;' \
        --data-binary '{"jsonrpc":"1.0","id":"bench","method":"getsyncdiag","params":[]}' \
        "${RPCURL}"
}

parse_field() {
    # $1 = key name, reads JSON blob from stdin
    python3 -c '
import json, sys
key = sys.argv[1]
blob = sys.stdin.read()
try:
    b = json.loads(blob)
except Exception:
    print(0); sys.exit(0)
r = b.get("result") if isinstance(b, dict) else None
if not isinstance(r, dict):
    r = b if isinstance(b, dict) else {}
print(r.get(key, 0))
' "$1"
}

prev_gap=""
peer_max_seen=0

for i in $(seq 1 "$SAMPLES"); do
    blob="$(call_syncdiag)" || {
        echo "[bench_running_lag] curl failed at sample $i" >&2
        exit 1
    }
    if ! printf '%s' "$blob" | python3 -c 'import sys,json; json.loads(sys.stdin.read())' 2>/dev/null; then
        echo "[bench_running_lag] invalid JSON at sample $i: $blob" >&2
        exit 1
    fi

    ch="$(printf '%s' "$blob" | parse_field chain_height)"
    pmh="$(printf '%s' "$blob" | parse_field peer_max_height)"

    if [[ "$pmh" -gt 0 ]]; then
        peer_max_seen=1
    fi

    if [[ "$pmh" -le 0 ]]; then
        echo "[bench_running_lag] sample $i: ch=$ch pmh=0 (waiting on handshake)"
        sleep "$INTERVAL_SECS"
        continue
    fi

    gap=$(( pmh - ch ))
    echo "[bench_running_lag] sample $i: ch=$ch pmh=$pmh gap=$gap"

    if [[ -n "$prev_gap" ]]; then
        # The gap is allowed to drift up by MINING_TOLERANCE blocks per
        # interval because upstream mining adds ~1 block/min. Anything
        # beyond that means we're not catching up.
        max_allowed=$(( prev_gap + MINING_TOLERANCE ))
        if [[ "$gap" -gt "$max_allowed" ]]; then
            echo "[bench_running_lag] gap grew: prev=$prev_gap now=$gap > allowed=$max_allowed" >&2
            exit 3
        fi
    fi
    prev_gap=$gap

    if [[ "$i" -lt "$SAMPLES" ]]; then
        sleep "$INTERVAL_SECS"
    fi
done

if [[ "$peer_max_seen" -eq 0 ]]; then
    echo "[bench_running_lag] peer_max_height never > 0 — peers never handshaked" >&2
    exit 2
fi

echo "[bench_running_lag] OK ($SAMPLES samples; final gap=$prev_gap)"
