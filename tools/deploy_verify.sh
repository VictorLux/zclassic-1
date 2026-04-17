#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Post-restart RPC health check for `make deploy`.
#
# The previous `make deploy` printed "Deployed." whenever systemd reported
# the unit active for >2s. That includes binaries that segfault on first
# RPC call. This script replaces that false-positive with a real probe:
# poll `zcl-rpc getblockcount` every 2s for up to 30s and only succeed
# when the node answers with an integer.
#
# Exit codes:
#   0  — RPC live, block count observed
#   1  — RPC did not come up within the deadline
#
# Usage: ./tools/deploy_verify.sh [rpc_tool] [timeout_seconds]
set -eu

RPC_TOOL="${1:-./zclassic-cli}"
TIMEOUT="${2:-30}"
INTERVAL=2

if [ ! -x "$RPC_TOOL" ]; then
    alt="./tools/zcl-rpc"
    if [ -x "$alt" ]; then
        RPC_TOOL="$alt"
    fi
fi

deadline=$(( $(date +%s) + TIMEOUT ))
attempt=0
last_err=""

while [ "$(date +%s)" -lt "$deadline" ]; do
    attempt=$((attempt + 1))
    if out=$("$RPC_TOOL" getblockcount 2>&1); then
        # Accept either a plain integer (zclassic-cli) or a JSON
        # envelope with "result":<integer> (tools/zcl-rpc). Any other
        # output keeps the loop polling.
        height=$(printf '%s' "$out" | grep -oE '"result"[[:space:]]*:[[:space:]]*[0-9]+' | grep -oE '[0-9]+' | head -1)
        if [ -z "$height" ]; then
            plain=$(printf '%s' "$out" | tr -d '[:space:]')
            case "$plain" in
                [0-9]*) height="$plain" ;;
            esac
        fi
        if [ -n "$height" ]; then
            echo "Deployed + RPC live at block $height."
            exit 0
        fi
    fi
    last_err="$out"
    sleep "$INTERVAL"
done

echo "DEPLOY FAILED: RPC did not come up within ${TIMEOUT}s (attempts=$attempt)"
if [ -n "$last_err" ]; then
    echo "last error: $last_err"
fi
exit 1
