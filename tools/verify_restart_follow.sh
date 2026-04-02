#!/bin/bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# verify_restart_follow.sh
# Restart/verify helper for the real operator question:
# can zclassic23 restart, catch up to legacy zclassicd tip, and stay there?

set -u

LEGACY_RPC_PORT="${LEGACY_RPC_PORT:-8232}"
LEGACY_RPC_USER="${LEGACY_RPC_USER:-}"
LEGACY_RPC_PASS="${LEGACY_RPC_PASS:-}"
LEGACY_CONF="${LEGACY_CONF:-$HOME/.zclassic/zclassic.conf}"
LEGACY_COOKIE="${LEGACY_COOKIE:-$HOME/.zclassic/.cookie}"

C23_RPC_PORT="${C23_RPC_PORT:-18232}"
C23_RPC_USER="${C23_RPC_USER:-}"
C23_RPC_PASS="${C23_RPC_PASS:-}"
C23_CONF="${C23_CONF:-$HOME/.zclassic-c23/zclassic.conf}"
C23_COOKIE="${C23_COOKIE:-$HOME/.zclassic-c23/.cookie}"

RESTART_MODE=0
RESTART_CMD="${RESTART_CMD:-systemctl --user restart zclassic23}"
TIMEOUT_SECS="${TIMEOUT_SECS:-900}"
STABLE_SAMPLES="${STABLE_SAMPLES:-3}"
POLL_SECS="${POLL_SECS:-10}"

usage() {
    cat <<EOF
Usage: ./tools/verify_restart_follow.sh [--restart] [--timeout N] [--poll N]

Environment:
  LEGACY_RPC_PORT   default: 8232
  LEGACY_RPC_USER   default: read from \$LEGACY_COOKIE or \$LEGACY_CONF
  LEGACY_RPC_PASS   default: read from \$LEGACY_COOKIE or \$LEGACY_CONF
  LEGACY_CONF       default: ~/.zclassic/zclassic.conf
  LEGACY_COOKIE     default: ~/.zclassic/.cookie
  C23_RPC_PORT      default: 18232
  C23_RPC_USER      default: read from \$C23_COOKIE or \$C23_CONF
  C23_RPC_PASS      default: read from \$C23_COOKIE or \$C23_CONF
  C23_CONF          default: ~/.zclassic-c23/zclassic.conf
  C23_COOKIE        default: ~/.zclassic-c23/.cookie
  RESTART_CMD       default: systemctl --user restart zclassic23
  TIMEOUT_SECS      default: 900
  STABLE_SAMPLES    default: 3
  POLL_SECS         default: 10

Examples:
  ./tools/verify_restart_follow.sh
  ./tools/verify_restart_follow.sh --restart
  RESTART_CMD="./zclassic23 -daemon" ./tools/verify_restart_follow.sh --restart
EOF
}

load_rpc_creds() {
    local conf="$1"
    local cookie_file="$2"
    local user_var="$3"
    local pass_var="$4"
    local user_val pass_val cookie_val

    eval "user_val=\"\${$user_var}\""
    eval "pass_val=\"\${$pass_var}\""

    if [ -z "$user_val" ] && [ -z "$pass_val" ] && [ -f "$cookie_file" ]; then
        cookie_val="$(cat "$cookie_file" 2>/dev/null)"
        if echo "$cookie_val" | grep -q ':'; then
            user_val="${cookie_val%%:*}"
            pass_val="${cookie_val#*:}"
        fi
    fi

    if [ -z "$user_val" ] && [ -f "$conf" ]; then
        user_val="$(sed -n 's/^rpcuser=//p' "$conf" | tail -n 1)"
    fi
    if [ -z "$pass_val" ] && [ -f "$conf" ]; then
        pass_val="$(sed -n 's/^rpcpassword=//p' "$conf" | tail -n 1)"
    fi

    eval "$user_var=\"\$user_val\""
    eval "$pass_var=\"\$pass_val\""
}

rpc_call() {
    local port="$1"
    local user="$2"
    local pass="$3"
    local method="$4"
    local params="${5:-[]}"

    curl -sf --connect-timeout 3 --max-time 15 \
        -u "$user:$pass" \
        -H 'content-type: text/plain;' \
        --data "{\"method\":\"$method\",\"params\":$params,\"id\":1}" \
        "http://127.0.0.1:${port}/" 2>/dev/null
}

json_result() {
    python3 -c '
import json,sys
try:
    d=json.load(sys.stdin)
    v=d.get("result")
    if isinstance(v, (dict,list)):
        print(json.dumps(v))
    elif v is None:
        print("")
    else:
        print(v)
except Exception:
    print("")
' 2>/dev/null
}

json_field() {
    local key="$1"
    python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    v=d['result']
    for k in '$key'.split('.'):
        if not k:
            continue
        if isinstance(v, list):
            v=v[int(k)]
        else:
            v=v[k]
    if isinstance(v, bool):
        print('true' if v else 'false')
    else:
        print(v)
except Exception:
    print('')
" 2>/dev/null
}

require_int() {
    echo "$1" | grep -qE '^-?[0-9]+$'
}

check_rpc_ready() {
    local port="$1"
    local user="$2"
    local pass="$3"
    local method="${4:-getinfo}"
    local tries="${5:-30}"
    local i out

    for i in $(seq 1 "$tries"); do
        out="$(rpc_call "$port" "$user" "$pass" "$method")"
        if [ -n "$out" ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --restart) RESTART_MODE=1 ;;
        --timeout) shift; TIMEOUT_SECS="${1:-$TIMEOUT_SECS}" ;;
        --poll) shift; POLL_SECS="${1:-$POLL_SECS}" ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; usage; exit 1 ;;
    esac
    shift
done

load_rpc_creds "$LEGACY_CONF" "$LEGACY_COOKIE" LEGACY_RPC_USER LEGACY_RPC_PASS
load_rpc_creds "$C23_CONF" "$C23_COOKIE" C23_RPC_USER C23_RPC_PASS

if [ -z "$LEGACY_RPC_USER" ] || [ -z "$LEGACY_RPC_PASS" ]; then
    echo "legacy RPC credentials missing; set LEGACY_RPC_USER/LEGACY_RPC_PASS or LEGACY_CONF" >&2
    exit 1
fi
if [ -z "$C23_RPC_USER" ] || [ -z "$C23_RPC_PASS" ]; then
    echo "zclassic23 RPC credentials missing; set C23_RPC_USER/C23_RPC_PASS or C23_CONF" >&2
    exit 1
fi

echo ""
echo "================================================================"
echo "  ZClassic23 Restart Follow Verification"
echo "================================================================"
echo ""

LEGACY_INFO="$(rpc_call "$LEGACY_RPC_PORT" "$LEGACY_RPC_USER" "$LEGACY_RPC_PASS" getinfo)"
LEGACY_TIP="$(echo "$LEGACY_INFO" | json_field blocks)"
if ! require_int "$LEGACY_TIP"; then
    echo "Could not read legacy zclassicd tip from port $LEGACY_RPC_PORT" >&2
    exit 1
fi
echo "Legacy zclassicd tip: $LEGACY_TIP"

if [ "$RESTART_MODE" -eq 1 ]; then
    echo "Restarting zclassic23 with: $RESTART_CMD"
    bash -lc "$RESTART_CMD"
fi

echo "Waiting for zclassic23 RPC on port $C23_RPC_PORT..."
if ! check_rpc_ready "$C23_RPC_PORT" "$C23_RPC_USER" "$C23_RPC_PASS" getinfo 60; then
    echo "zclassic23 RPC did not become ready" >&2
    exit 1
fi

deadline=$(( $(date +%s) + TIMEOUT_SECS ))
stable=0

while [ "$(date +%s)" -lt "$deadline" ]; do
    C23_INFO="$(rpc_call "$C23_RPC_PORT" "$C23_RPC_USER" "$C23_RPC_PASS" getblockchaininfo)"
    C23_HEALTH="$(rpc_call "$C23_RPC_PORT" "$C23_RPC_USER" "$C23_RPC_PASS" healthcheck)"
    C23_SYNC="$(rpc_call "$C23_RPC_PORT" "$C23_RPC_USER" "$C23_RPC_PASS" syncstate)"

    chain_h="$(echo "$C23_INFO" | json_field blocks)"
    headers_h="$(echo "$C23_INFO" | json_field headers)"
    sync_state="$(echo "$C23_SYNC" | json_field state)"
    healthy="$(echo "$C23_HEALTH" | json_field healthy)"
    degraded="$(echo "$C23_HEALTH" | json_field degraded_reason)"

    if ! require_int "$chain_h"; then chain_h="-1"; fi
    if ! require_int "$headers_h"; then headers_h="-1"; fi
    lag=$(( LEGACY_TIP - chain_h ))
    if [ "$lag" -lt 0 ]; then lag=0; fi

    printf "%s chain=%s headers=%s legacy=%s lag=%s state=%s healthy=%s degraded=%s\n" \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        "$chain_h" "$headers_h" "$LEGACY_TIP" "$lag" \
        "${sync_state:-unknown}" "${healthy:-unknown}" "${degraded:-none}"

    if [ "$chain_h" -ge "$LEGACY_TIP" ] &&
       [ "$headers_h" -ge "$LEGACY_TIP" ] &&
       [ "${sync_state:-}" = "at_tip" ] &&
       [ "${healthy:-}" = "true" ]; then
        stable=$((stable + 1))
        if [ "$stable" -ge "$STABLE_SAMPLES" ]; then
            echo ""
            echo "PASS: zclassic23 reached legacy tip and stayed healthy for $stable samples."
            exit 0
        fi
    else
        stable=0
    fi

    sleep "$POLL_SECS"
done

echo ""
echo "FAIL: zclassic23 did not prove catch-up to legacy tip within ${TIMEOUT_SECS}s." >&2
exit 1
