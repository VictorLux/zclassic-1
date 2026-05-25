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
source_commit="$(git -C "$ROOT" rev-parse --short=9 HEAD 2>/dev/null || true)"
if [ -z "$source_commit" ]; then
    source_commit="unknown"
fi
source_dirty="false"
if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]; then
    source_dirty="true"
fi

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

python3 - "$health_json" "$preflight_json" "$MODE" "$source_commit" "$source_dirty" <<'PY'
import json
import sys
import time

health_path, preflight_path, mode, source_commit, source_dirty_s = sys.argv[1:6]
source_dirty = source_dirty_s == "true"

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

def fmt_bool(obj, key):
    if not isinstance(obj, dict) or key not in obj:
        return "unknown"
    return str(bool(obj.get(key))).lower()

def fmt_age_from_unix(obj, key):
    try:
        ts = int(obj.get(key, 0) or 0)
    except Exception:
        return "unknown"
    if ts <= 0:
        return "unknown"
    age = int(time.time()) - ts
    return str(age) if age >= 0 else "unknown"

def int_or(obj, key, fallback=None):
    if not isinstance(obj, dict):
        return fallback
    try:
        return int(obj.get(key, fallback))
    except Exception:
        return fallback

def str_or(obj, key, fallback=""):
    if not isinstance(obj, dict):
        return fallback
    value = obj.get(key, fallback)
    return fallback if value is None else str(value)

def chain_advance_target_gap(ca_gate):
    local_height = int_or(ca_gate, "local_height")
    target_height = int_or(ca_gate, "target_height")
    if local_height is None or target_height is None:
        return "unknown"
    return str(target_height - local_height) if target_height > local_height else "0"

def chain_advance_not_ready_reason(ca_gate):
    existing = str_or(ca_gate, "not_ready_reason", "")
    if existing and existing != "unknown":
        return existing
    if fmt_bool(ca_gate, "ready") == "true":
        return ""
    if str_or(ca_gate, "decision", "unknown") != "use_source":
        return "decision_not_use_source"
    if str_or(ca_gate, "selected_source", "none") in ("none", "unknown"):
        return "selected_source_invalid"
    if str_or(ca_gate, "blocker", ""):
        return "blocker_present"
    local_height = int_or(ca_gate, "local_height")
    target_height = int_or(ca_gate, "target_height")
    if local_height is None or target_height is None:
        return "invalid_heights"
    if local_height + 1 < target_height:
        return "target_height_gap"
    projection_lag = int_or(ca_gate, "projection_lag")
    if projection_lag is None:
        return "projection_lag_unknown"
    if projection_lag < 0 or projection_lag > 1:
        return "projection_lag"
    if fmt_bool(ca_gate, "source_ready") != "true":
        return "source_not_ready"
    return "unknown"

health = load_rpc(health_path)
preflight = load_rpc(preflight_path)
checks = health.get("checks") if isinstance(health.get("checks"), dict) else {}
live = preflight.get("live") if isinstance(preflight.get("live"), dict) else {}
state = preflight.get("cutover_state")
if not isinstance(state, dict):
    state = {}
ca = checks.get("chain_advance") if isinstance(checks.get("chain_advance"), dict) else {}
ca_gate = preflight.get("chain_advance") if isinstance(preflight.get("chain_advance"), dict) else {}
guard = preflight.get("guard") if isinstance(preflight.get("guard"), dict) else {}
diff = preflight.get("header_admit_diff") if isinstance(preflight.get("header_admit_diff"), dict) else {}
vh = preflight.get("validate_headers") if isinstance(preflight.get("validate_headers"), dict) else {}
blockers = preflight.get("blockers")
if not isinstance(blockers, list):
    blockers = []

healthy = bool(health.get("healthy"))
tip_lag = int(checks.get("tip_lag", live.get("tip_lag", 999999)) or 0)
tip_age = int(checks.get("tip_advance_age_seconds",
                         live.get("tip_advance_age_seconds", 999999)) or 0)
peer_count = int(checks.get("peer_count", live.get("peer_count", 0)) or 0)
live_ready = healthy and peer_count > 0 and tip_lag <= 2 and 0 <= tip_age <= 180
build_commit = str(health.get("build_commit", "unknown") or "unknown")
build_matches_source = (
    source_commit != "unknown" and
    build_commit != "unknown" and
    not source_dirty and
    build_commit.startswith(source_commit)
)
cutover_ready = bool(preflight.get("ready")) and build_matches_source
display_blockers = list(blockers)
if source_dirty:
    display_blockers.append("source_tree_dirty")
if not build_matches_source:
    display_blockers.append("live_build_not_current")
ca_not_ready_reason = chain_advance_not_ready_reason(ca_gate)
ca_target_gap = str_or(ca_gate, "target_gap", "unknown")
if ca_target_gap == "unknown":
    ca_target_gap = chain_advance_target_gap(ca_gate)

print("scoreboard read-only")
print(f"build_commit={build_commit}")
print(
    "source_gate="
    f"source_commit={source_commit} "
    f"source_dirty={str(source_dirty).lower()} "
    f"build_matches_source={str(build_matches_source).lower()}"
)
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
    f"blockers={','.join(str(b) for b in display_blockers) if display_blockers else 'none'}"
)
print(
    "cutover_live_gate="
    f"ready={fmt_bool(live, 'cutover_ready')} "
    f"tip_recent={fmt_bool(live, 'tip_recent')} "
    f"headers_in_range={fmt_bool(live, 'headers_in_range')} "
    f"operator_needed_blocks={fmt_bool(live, 'operator_needed_blocks_cutover')} "
    f"degraded_reason={live.get('degraded_reason', '')}"
)
print(
    "cutover_chain_advance_gate="
    f"ready={fmt_bool(ca_gate, 'ready')} "
    f"source_ready={fmt_bool(ca_gate, 'source_ready')} "
    f"selected_source={ca_gate.get('selected_source', 'unknown')} "
    f"not_ready_reason={ca_not_ready_reason} "
    f"target_gap={ca_target_gap} "
    f"local_height={ca_gate.get('local_height', 'unknown')} "
    f"target_height={ca_gate.get('target_height', 'unknown')} "
    f"projection_lag={ca_gate.get('projection_lag', 'unknown')} "
    f"selected_source_blocker={ca_gate.get('selected_source_blocker', '')} "
    f"blocker={ca_gate.get('blocker', '')}"
)
print(
    "cutover_guard_gate="
    f"ready={fmt_bool(guard, 'ready')} "
    f"registered={fmt_bool(guard, 'registered')} "
    f"config_ready={fmt_bool(guard, 'config_ready')} "
    f"state_ready={fmt_bool(guard, 'state_ready')} "
    f"active={fmt_bool(guard, 'currently_active')} "
    f"last_outcome={guard.get('last_outcome', 'unknown')}"
)
print(
    "cutover_header_admit_gate="
    f"status={diff.get('status', 'unknown')} "
    f"cursor_lag={diff.get('cursor_lag', 0)} "
    f"log_tip_lag={diff.get('log_tip_lag', 0)} "
    f"mismatch_count={diff.get('mismatch_count', 0)} "
    f"first_divergent_height={diff.get('first_divergent_height', -1)}"
)
print(
    "cutover_validate_headers_gate="
    f"cursor_lag={vh.get('cursor_lag', 0)} "
    f"window_available={fmt_bool(vh, 'window_available')} "
    f"window_complete={fmt_bool(vh, 'window_complete')} "
    f"window_failed_count={vh.get('window_failed_count', 0)} "
    f"window_first_failed_height={vh.get('window_first_failed_height', -1)} "
    f"no_failures={fmt_bool(vh, 'no_failures')} "
    f"failed_total={vh.get('failed_total', 'unknown')} "
    f"failure_log_count={vh.get('failure_log_count', 'unknown')} "
    f"first_failed_height={vh.get('first_failed_height', -1)} "
    f"first_fail_reason={vh.get('first_fail_reason', '')} "
    f"last_failed_height={vh.get('last_failed_height', -1)} "
    f"last_fail_reason={vh.get('last_fail_reason', '')} "
    f"error_count={vh.get('error_count', 'unknown')} "
    f"last_blocked_age_seconds={fmt_age_from_unix(vh, 'last_blocked_unix')}"
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
