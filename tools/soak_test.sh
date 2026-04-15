#!/bin/bash
# Soak test: monitor node health every 5 minutes for 72 hours
# Checks: height advancing, RSS stable, peers connected, no crashes
# Logs to: soak_test.log
#
# Usage: ./tools/soak_test.sh [-d HOURS] [-i SECONDS] [-p PORT]

DURATION_HOURS=72
INTERVAL_SECS=300
RPC_PORT=18232
LOG_FILE="soak_test.log"
SERVICE_NAME="zclassic23"
MAX_RSS_MB=4096
STALL_TIMEOUT_SECS=1800  # 30 minutes

# Parse flags
while getopts "d:i:p:" opt; do
    case $opt in
        d) DURATION_HOURS="$OPTARG" ;;
        i) INTERVAL_SECS="$OPTARG" ;;
        p) RPC_PORT="$OPTARG" ;;
        *) echo "Usage: $0 [-d hours] [-i seconds] [-p rpc_port]"; exit 1 ;;
    esac
done

RPC="./tools/zcl-rpc"
if [ ! -x "$RPC" ]; then
    RPC="$(dirname "$0")/zcl-rpc"
fi

TOTAL_CHECKS=$(( DURATION_HOURS * 3600 / INTERVAL_SECS ))
START_TIME=$(date +%s)
END_TIME=$(( START_TIME + DURATION_HOURS * 3600 ))

# Tracking
RESTARTS=0
MAX_RSS=0
LAST_HEIGHT=-1
LAST_HEIGHT_CHANGE=$(date +%s)
BLOCKS_START=-1
ALERTS=0

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $1" | tee -a "$LOG_FILE"
}

alert() {
    log "ALERT: $1"
    ALERTS=$((ALERTS + 1))
}

get_height() {
    $RPC getblockcount 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print(d.get('result', -1))
except: print(-1)" 2>/dev/null
}

get_rss_mb() {
    local pid
    pid=$(pidof zclassic23 | awk '{print $NF}')
    if [ -n "$pid" ]; then
        awk '/VmRSS/ {printf "%.0f", $2/1024}' "/proc/$pid/status" 2>/dev/null
    else
        echo 0
    fi
}

get_peer_count() {
    $RPC getpeerinfo 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    r = d.get('result', [])
    print(len(r) if isinstance(r, list) else 0)
except: print(0)" 2>/dev/null
}

get_sync_state() {
    $RPC syncstate 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    r = d.get('result', {})
    print(r.get('state', 'unknown'))
except: print('unknown')" 2>/dev/null
}

restart_node() {
    log "Restarting $SERVICE_NAME via systemctl..."
    systemctl --user start "$SERVICE_NAME" 2>/dev/null
    RESTARTS=$((RESTARTS + 1))
    sleep 10
}

log "=== Soak test started ==="
log "Duration: ${DURATION_HOURS}h, Interval: ${INTERVAL_SECS}s, Port: ${RPC_PORT}"
log "Max RSS threshold: ${MAX_RSS_MB}MB, Stall timeout: ${STALL_TIMEOUT_SECS}s"

CHECK_NUM=0
while [ "$(date +%s)" -lt "$END_TIME" ]; do
    CHECK_NUM=$((CHECK_NUM + 1))
    NOW=$(date +%s)
    ELAPSED=$(( (NOW - START_TIME) / 60 ))

    # Check if process is alive
    if ! pidof zclassic23 >/dev/null 2>&1; then
        alert "Node process not running! Restarting..."
        restart_node
        sleep 30
        continue
    fi

    HEIGHT=$(get_height)
    RSS=$(get_rss_mb)
    PEERS=$(get_peer_count)
    SYNC=$(get_sync_state)

    if [ "$BLOCKS_START" -eq -1 ] && [ "$HEIGHT" -gt 0 ]; then
        BLOCKS_START=$HEIGHT
    fi

    # Track max RSS
    if [ "$RSS" -gt "$MAX_RSS" ]; then
        MAX_RSS=$RSS
    fi

    # Log status
    log "check=$CHECK_NUM/${TOTAL_CHECKS} elapsed=${ELAPSED}m height=$HEIGHT rss=${RSS}MB peers=$PEERS sync=$SYNC"

    # Alert: height stalled
    if [ "$HEIGHT" -gt 0 ]; then
        if [ "$HEIGHT" -ne "$LAST_HEIGHT" ]; then
            LAST_HEIGHT=$HEIGHT
            LAST_HEIGHT_CHANGE=$NOW
        elif [ $((NOW - LAST_HEIGHT_CHANGE)) -gt "$STALL_TIMEOUT_SECS" ]; then
            STALL_MIN=$(( (NOW - LAST_HEIGHT_CHANGE) / 60 ))
            alert "Height stalled at $HEIGHT for ${STALL_MIN} minutes"
        fi
    fi

    # Alert: RSS too high
    if [ "$RSS" -gt "$MAX_RSS_MB" ]; then
        alert "RSS ${RSS}MB exceeds ${MAX_RSS_MB}MB threshold"
    fi

    # Alert: no peers
    if [ "$PEERS" -eq 0 ]; then
        alert "No peers connected"
    fi

    sleep "$INTERVAL_SECS"
done

# Summary
END=$(date +%s)
TOTAL_SECS=$((END - START_TIME))
TOTAL_HOURS=$((TOTAL_SECS / 3600))
TOTAL_MINS=$(( (TOTAL_SECS % 3600) / 60 ))
FINAL_HEIGHT=$(get_height)
BLOCKS_PROCESSED=0
if [ "$BLOCKS_START" -gt 0 ] && [ "$FINAL_HEIGHT" -gt 0 ]; then
    BLOCKS_PROCESSED=$((FINAL_HEIGHT - BLOCKS_START))
fi

log ""
log "=== Soak test summary ==="
log "Uptime: ${TOTAL_HOURS}h ${TOTAL_MINS}m"
log "Blocks processed: $BLOCKS_PROCESSED ($BLOCKS_START → $FINAL_HEIGHT)"
log "Max RSS: ${MAX_RSS}MB"
log "Restarts: $RESTARTS"
log "Alerts: $ALERTS"
log "Checks completed: $CHECK_NUM"
log "========================="
