#!/usr/bin/env bash
# Read-only live node scoreboard for operator gates.
#
# Exit codes:
#   0  live node is healthy (and cutover-ready when --cutover is used)
#   2  RPC failed or returned unparseable JSON
#   3  live node is not healthy enough for operator action
#   4  --cutover requested and cutoverpreflight.ready is false
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RPC="${ZCL_RPC:-$ROOT/tools/zcl-rpc}"
MODE="live"

case "${1:-}" in
    ""|--live) MODE="live" ;;
    --cutover) MODE="cutover" ;;
    -h|--help)
        echo "Usage: tools/scoreboard.sh [--live|--cutover]"
        echo "  --live     require healthy live tip only (default)"
        echo "  --cutover  also require cutoverpreflight.ready=true"
        exit 0
        ;;
    *)
        echo "Usage: tools/scoreboard.sh [--live|--cutover]" >&2
        exit 2
        ;;
esac

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/zcl-scoreboard.XXXXXX")" || exit 2
trap 'rm -rf "$tmpdir"' EXIT

health_json="$tmpdir/health.json"
preflight_json="$tmpdir/preflight.json"

if ! "$RPC" healthcheck >"$health_json" 2>"$tmpdir/health.err"; then
    echo "scoreboard read-only $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "RPC_ERROR=healthcheck failed"
    cat "$tmpdir/health.err" >&2
    exit 2
fi

if ! "$RPC" cutoverpreflight -1 -1 >"$preflight_json" 2>"$tmpdir/preflight.err"; then
    echo "scoreboard read-only $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "RPC_ERROR=cutoverpreflight failed"
    cat "$tmpdir/preflight.err" >&2
    exit 2
fi

python3 - "$health_json" "$preflight_json" "$MODE" <<'PY'
import json
import sys

health_path, preflight_path, mode = sys.argv[1:4]

def load_rpc(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            blob = json.load(f)
    except Exception as exc:
        print(f"JSON_ERROR={path}: {exc}")
        raise SystemExit(2)
    if isinstance(blob, dict) and "result" in blob:
        result = blob.get("result")
        return result if isinstance(result, dict) else {}
    return blob if isinstance(blob, dict) else {}

health = load_rpc(health_path)
preflight = load_rpc(preflight_path)
checks = health.get("checks") if isinstance(health.get("checks"), dict) else {}
live = preflight.get("live") if isinstance(preflight.get("live"), dict) else {}
state = preflight.get("cutover_state")
if not isinstance(state, dict):
    state = {}
ca = checks.get("chain_advance") if isinstance(checks.get("chain_advance"), dict) else {}
blockers = preflight.get("blockers")
if not isinstance(blockers, list):
    blockers = []

healthy = bool(health.get("healthy"))
tip_lag = int(checks.get("tip_lag", live.get("tip_lag", 999999)) or 0)
tip_age = int(checks.get("tip_advance_age_seconds",
                         live.get("tip_advance_age_seconds", 999999)) or 0)
peer_count = int(checks.get("peer_count", live.get("peer_count", 0)) or 0)
live_ready = healthy and peer_count > 0 and tip_lag <= 2 and 0 <= tip_age <= 180
cutover_ready = bool(preflight.get("ready"))

print("scoreboard read-only")
print(f"build_commit={health.get('build_commit', 'unknown')}")
print(f"sync_state={health.get('sync_state', 'unknown')}")
print(
    "live="
    f"healthy={str(healthy).lower()} "
    f"peer_count={peer_count} "
    f"tip_lag={tip_lag} "
    f"tip_advance_age_seconds={tip_age}"
)
print(
    "chain_advance="
    f"decision={ca.get('decision', 'unknown')} "
    f"selected_source={ca.get('selected_source', 'unknown')} "
    f"blocker={ca.get('blocker', '')}"
)
print(
    "cutover="
    f"ready={str(cutover_ready).lower()} "
    f"canary_target_height={live.get('canary_target_height', 0)} "
    f"blockers={','.join(str(b) for b in blockers) if blockers else 'none'}"
)
print(
    "cutover_state="
    f"has_change={str(bool(state.get('has_change'))).lower()} "
    f"authoritative_active={str(bool(state.get('authoritative_active'))).lower()} "
    f"canary_status={state.get('canary_status', 'unknown')} "
    f"change_height={state.get('change_height', 0)} "
    f"canary_target_height={state.get('canary_target_height', 0)} "
    f"current_tip_height={state.get('current_tip_height', 0)} "
    f"canary_passed={str(bool(state.get('canary_passed'))).lower()} "
    f"canary_failed={str(bool(state.get('canary_failed'))).lower()} "
    f"elapsed_seconds={state.get('canary_elapsed_seconds', -1)}"
)

if not live_ready:
    print("VERDICT=LIVE_NOT_READY")
    raise SystemExit(3)
if mode == "cutover" and not cutover_ready:
    print("VERDICT=CUTOVER_NOT_READY")
    raise SystemExit(4)

print("VERDICT=CUTOVER_READY" if mode == "cutover" else "VERDICT=LIVE_READY")
PY
