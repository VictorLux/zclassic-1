# Agent 3 Task: Built-in Sync Watchdog & Self-Healing

## Problem

The node has no built-in self-healing for sync stalls. The external `zcl-watchdog.c` only logs alerts but takes no corrective action. When the node gets stuck (currently stuck at height 2,015,124 while peers are at 3,077,062 in `headers_download` state), it stays stuck forever until manually restarted.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md` — mandatory coding rules
- `app/services/src/node_health_service.c` — existing health check pattern
- `app/services/src/block_sync_service.c` — stall recovery (block download only)
- `lib/net/src/msgprocessor.c` lines 4194-4290 (current stall detection + tip stale watchdog)
- `config/src/boot_services.c` — how services are wired up
- `lib/net/include/net/net.h` — sync states, p2p_node struct

## Tasks

### 1. Create sync watchdog service

Create `app/services/src/sync_watchdog_service.c` and `app/services/include/services/sync_watchdog_service.h`.

The watchdog runs from the message processing loop (same place as current stall detection, ~every 30s). It checks:

**a. HEADER_STALL**: If `SYNC_HEADERS_DOWNLOAD` for >300s with `pindex_best_header` not advancing:
- Disconnect ALL outbound peers (force fresh connections)
- Reset sync state to `SYNC_FINDING_PEERS`
- Log: `"[watchdog] HEADER_STALL recovery: disconnected %d peers, resetting sync"`

**b. BLOCK_STALL**: If `SYNC_BLOCKS_DOWNLOAD` for >300s with chain height not advancing:
- Call `dl_reset()` on download manager (you may need to add this — clear all in-flight, re-queue)
- Re-request headers from all outbound peers
- Log: `"[watchdog] BLOCK_STALL recovery: reset download manager, re-queued blocks"`

**c. STATE_STUCK**: If ANY sync state unchanged for >600s (except `SYNC_AT_TIP`):
- Force transition to `SYNC_HEADERS_DOWNLOAD`
- Log: `"[watchdog] STATE_STUCK: %s for %llds, forcing header re-sync"`

**d. REPEATED_RESTART**: If the watchdog has triggered recovery >3 times in 30 minutes:
- Stop trying automatic recovery
- Log: `"[watchdog] REPEATED failures — manual intervention needed"`

### 2. Sync state timestamps

Add to the sync state tracking (wherever `sync_set_state()` is defined):
- `int64_t sync_state_entered_time` — when current state began
- `int sync_state_entry_height` — chain height when state was entered
- Expose via: `int64_t sync_get_state_duration(void)` and `int sync_get_state_entry_height(void)`

### 3. RPC command: getsyncwatchdog

Add RPC method `getsyncwatchdog` returning JSON:
```json
{
  "enabled": true,
  "checks_run": 142,
  "recoveries_triggered": 2,
  "last_recovery_time": 1776117000,
  "last_recovery_type": "HEADER_STALL",
  "current_state": "headers_download",
  "current_state_duration_secs": 450,
  "current_state_entry_height": 2015124
}
```

Wire it into the RPC dispatch table following existing patterns (look at how `getsyncdetail` is registered).

### 4. Tests

Write tests in `lib/test/src/test_sync_watchdog.c`:
- Test header stall detection triggers after 300s
- Test block stall detection triggers after 300s
- Test state stuck detection triggers after 600s
- Test repeated restart circuit breaker (>3 in 30min)
- Test that SYNC_AT_TIP is exempt from stuck detection
- Register tests in `lib/test/src/test.c`

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing — all tests must pass
- Commit with descriptive messages
- Do NOT touch files unrelated to this task
