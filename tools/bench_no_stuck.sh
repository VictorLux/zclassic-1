#!/usr/bin/env bash
# bench_no_stuck.sh — prove the "never stuck" guarantee under kill -9.
#
# Loop ITERATIONS times:
#   1. record current tip
#   2. systemctl --user kill -s SIGKILL zclassic23 (simulates power loss)
#   3. systemctl --user start zclassic23
#   4. wait for RPC to come up (RPC_DEADLINE_SECS)
#   5. assert tip advances by at least 1 block within ADVANCE_DEADLINE_SECS
#
# If any iteration fails to advance, exit non-zero. Otherwise we've
# proven the boot-recovery path is healthy.
#
# Env:
#   ITERATIONS=10              how many kill-restart cycles to run
#   RPC_DEADLINE_SECS=60       max time for RPC to answer after restart
#   ADVANCE_DEADLINE_SECS=300  max time for tip to advance by ≥ 1 block
#                              (5 min default because a kill -9 mid-flush
#                              can rewind coins state by a few blocks and
#                              the chain must re-fetch them from peers;
#                              with thin peering this dominates)
#   ZCL_RPCPORT=18232
#
# Exit codes:
#   0  all iterations succeeded
#   1  pre-flight check failed (node not running, RPC not answering)
#   2  RPC did not come up after restart within the deadline
#   3  tip did not advance within the deadline (NODE GOT STUCK)

set -uo pipefail

ITERATIONS="${ITERATIONS:-10}"
RPC_DEADLINE_SECS="${RPC_DEADLINE_SECS:-60}"
ADVANCE_DEADLINE_SECS="${ADVANCE_DEADLINE_SECS:-300}"
RPCPORT="${ZCL_RPCPORT:-18232}"
RPC_TOOL="${RPC_TOOL:-./tools/zcl-rpc}"

if [[ ! -x "${RPC_TOOL}" ]]; then
    echo "[bench_no_stuck] RPC tool not executable: ${RPC_TOOL}" >&2
    exit 1
fi

get_height() {
    local result
    result=$("${RPC_TOOL}" getblockcount 2>/dev/null) || return 1
    # Parse {"result":N,...}
    echo "${result}" | sed -n 's/.*"result":\([0-9-]\+\).*/\1/p'
}

wait_for_rpc() {
    local deadline
    deadline=$(( $(date +%s) + RPC_DEADLINE_SECS ))
    while [[ $(date +%s) -lt ${deadline} ]]; do
        local h
        h=$(get_height 2>/dev/null) || true
        if [[ -n "${h:-}" && "${h}" =~ ^[0-9]+$ ]]; then
            return 0
        fi
        sleep 2
    done
    return 1
}

wait_for_advance() {
    local base="$1"
    local deadline
    deadline=$(( $(date +%s) + ADVANCE_DEADLINE_SECS ))
    while [[ $(date +%s) -lt ${deadline} ]]; do
        local h
        h=$(get_height 2>/dev/null) || true
        if [[ -n "${h:-}" && "${h}" =~ ^[0-9]+$ && "${h}" -gt "${base}" ]]; then
            return 0
        fi
        sleep 3
    done
    return 1
}

# Pre-flight
START_TIP=$(get_height) || { echo "[bench_no_stuck] node not responding"; exit 1; }
if [[ -z "${START_TIP}" || ! "${START_TIP}" =~ ^[0-9]+$ ]]; then
    echo "[bench_no_stuck] could not read starting tip" >&2
    exit 1
fi
echo "[bench_no_stuck] pre-flight OK: starting tip=${START_TIP}"

PASSED=0
for i in $(seq 1 "${ITERATIONS}"); do
    BEFORE=$(get_height) || { echo "[bench_no_stuck] iter ${i}: RPC dead before kill"; exit 1; }
    echo "[bench_no_stuck] iter ${i}/${ITERATIONS}: killing at tip=${BEFORE}"

    systemctl --user kill -s SIGKILL zclassic23 || true
    # Give systemd a moment to register the death before we ask it to start.
    sleep 3
    systemctl --user start zclassic23 || {
        echo "[bench_no_stuck] iter ${i}: systemd start failed"
        exit 2
    }

    t0=$(date +%s)
    if ! wait_for_rpc; then
        echo "[bench_no_stuck] iter ${i}: RPC did not come up within ${RPC_DEADLINE_SECS}s"
        exit 2
    fi
    t_rpc=$(( $(date +%s) - t0 ))

    AFTER_BOOT=$(get_height)
    echo "[bench_no_stuck] iter ${i}: RPC up in ${t_rpc}s, tip=${AFTER_BOOT}"

    if ! wait_for_advance "${AFTER_BOOT}"; then
        echo "[bench_no_stuck] iter ${i}: TIP DID NOT ADVANCE within ${ADVANCE_DEADLINE_SECS}s — STUCK"
        exit 3
    fi
    t_adv=$(( $(date +%s) - t0 ))
    FINAL=$(get_height)
    echo "[bench_no_stuck] iter ${i}: tip advanced ${AFTER_BOOT}->${FINAL} in ${t_adv}s (total)"
    PASSED=$((PASSED + 1))
done

echo "[bench_no_stuck] PASS: ${PASSED}/${ITERATIONS} iterations advanced cleanly"
exit 0
