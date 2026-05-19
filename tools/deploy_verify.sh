#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Post-restart RPC health check for `make deploy`.
#
# The previous `make deploy` printed "Deployed." whenever systemd reported
# the unit active for >2s. That includes binaries that segfault on first
# RPC call. This script replaces that false-positive with a real probe:
# poll RPC every 2s for up to 120s and only succeed when the node answers
# with an integer height and the public-node hardening diagnostics are
# registered by the running daemon.
#
# Exit codes:
#   0  — RPC live, block count observed, diagnostic contract present
#   1  — RPC/diagnostic contract did not come up within the deadline
#
# Usage: ./tools/deploy_verify.sh [rpc_tool] [timeout_seconds]
set -eu

RPC_TOOL="${1:-./zclassic-cli}"
TIMEOUT="${2:-120}"
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

json_has_key() {
    printf '%s\n' "$1" | grep -q "\"$2\"[[:space:]]*:"
}

json_key_is_true() {
    printf '%s\n' "$1" | grep -q "\"$2\"[[:space:]]*:[[:space:]]*true"
}

json_key_is_string() {
    printf '%s\n' "$1" |
        grep -q "\"$2\"[[:space:]]*:[[:space:]]*\"$3\""
}

extract_height() {
    height=$(printf '%s' "$1" |
        grep -oE '"result"[[:space:]]*:[[:space:]]*[0-9]+' |
        grep -oE '[0-9]+' | head -1)
    if [ -z "$height" ]; then
        plain=$(printf '%s' "$1" | tr -d '[:space:]')
        case "$plain" in
            [0-9]*) height="$plain" ;;
        esac
    fi
    printf '%s' "$height"
}

rpc_dumpstate() {
    component="$1"
    out=$("$RPC_TOOL" dumpstate "$component" 2>&1 || true)
    if json_has_key "$out" "$2"; then
        printf '%s\n' "$out"
        return 0
    fi

    # tools/zcl-rpc wraps remaining argv directly into a JSON params array,
    # so string arguments need quotes. zclassic-cli accepts the unquoted
    # form above, but this fallback keeps deploy verification portable.
    out=$("$RPC_TOOL" dumpstate "\"$component\"" 2>&1 || true)
    printf '%s\n' "$out"
}

verify_contract() {
    height="$1"

    ca=$(rpc_dumpstate chain_advance_coordinator initialized)
    json_key_is_true "$ca" initialized ||
        { last_err="chain_advance_coordinator not initialized: $ca"; return 1; }
    json_key_is_true "$ca" has_connman ||
        { last_err="chain_advance_coordinator missing connman: $ca"; return 1; }
    json_key_is_true "$ca" has_main_state ||
        { last_err="chain_advance_coordinator missing main_state: $ca"; return 1; }
    json_key_is_true "$ca" has_node_db ||
        { last_err="chain_advance_coordinator missing node_db: $ca"; return 1; }
    json_key_is_string "$ca" authority local_consensus_validation ||
        { last_err="chain_advance authority contract missing: $ca"; return 1; }
    json_has_key "$ca" sources ||
        { last_err="chain_advance sources missing: $ca"; return 1; }

    net=$("$RPC_TOOL" getnetworkinfo 2>&1 || true)
    for key in inbound_connections outbound_connections handshaked_connections \
               inbound_handshake_seen remote_handshake_seen magicbean_peers \
               zclassic_c23_peers peer_lifecycle; do
        json_has_key "$net" "$key" ||
            { last_err="getnetworkinfo missing $key: $net"; return 1; }
    done

    peer=$(rpc_dumpstate peer_lifecycle summary)
    json_has_key "$peer" summary ||
        { last_err="peer_lifecycle summary missing: $peer"; return 1; }
    json_has_key "$peer" sources ||
        { last_err="peer_lifecycle sources missing: $peer"; return 1; }

    mirror=$(rpc_dumpstate legacy_mirror consensus_authority)
    json_has_key "$mirror" consensus_authority ||
        { last_err="legacy_mirror authority missing: $mirror"; return 1; }
    json_has_key "$mirror" blockers_total ||
        { last_err="legacy_mirror blockers_total missing: $mirror"; return 1; }
    json_has_key "$mirror" stalls_total ||
        { last_err="legacy_mirror stalls_total missing: $mirror"; return 1; }
    json_has_key "$mirror" unsafe_overrides_total ||
        { last_err="legacy_mirror unsafe_overrides_total missing: $mirror"; return 1; }
    json_has_key "$mirror" last_override_safe ||
        { last_err="legacy_mirror last_override_safe missing: $mirror"; return 1; }
    json_has_key "$mirror" last_override_scope ||
        { last_err="legacy_mirror last_override_scope missing: $mirror"; return 1; }

    echo "Deployed + RPC live at block $height; canonical diagnostics ready."
    return 0
}

while [ "$(date +%s)" -lt "$deadline" ]; do
    attempt=$((attempt + 1))
    if out=$("$RPC_TOOL" getblockcount 2>&1); then
        # Accept either a plain integer (zclassic-cli) or a JSON
        # envelope with "result":<integer> (tools/zcl-rpc). Any other
        # output keeps the loop polling.
        height=$(extract_height "$out")
        if [ -n "$height" ] && verify_contract "$height"; then
            exit 0
        fi
    fi
    if [ -z "$last_err" ]; then
        last_err="$out"
    fi
    sleep "$INTERVAL"
done

echo "DEPLOY FAILED: RPC/diagnostic contract did not become ready within ${TIMEOUT}s (attempts=$attempt)"
if [ -n "$last_err" ]; then
    echo "last error: $last_err"
fi
exit 1
