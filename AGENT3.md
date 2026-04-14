# AGENT3 — Wave 19: Testing, P2P Hardening & Observability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

The node has a pattern of getting "stuck" during sync — sitting in `blocks_download` state without downloading any blocks. Wave 19 Agent2 is fixing the root cause (broken pprev chain walks after snapshot sync, empty download queue). Agent3 focuses on testing, hardening, and observability to catch these issues faster.

---

## Task 1: Test Coverage for Sync Stall Scenarios

The sync pipeline has multiple paths that can silently fail, leaving the node stuck. Add tests that reproduce these scenarios.

### Files to create/modify:
- `lib/test/src/test_sync_service.c` — Add stall scenario tests

### Test scenarios to add:

**a) Empty download queue in blocks_download state:**
Set up a scenario where `sync_state == SYNC_BLOCKS_DOWNLOAD`, the download queue is empty, in-flight is zero, but `chain_height < peer_starting_height`. Verify that `syncsvc_build_stall_recovery()` returns `should_recover=true` and populates `alt_hashes`.

**b) `chains_from_tip` with broken pprev chain:**
Create block_index entries where the pprev chain stops at a non-tip block (simulating post-snapshot state). Verify `syncsvc_headers_chain_from_tip()` handles this correctly — currently it returns false, which is the bug. Your test should document the expected behavior.

**c) Watchdog BLOCK_STALL with zero queue:**
Set sync state to `SYNC_BLOCKS_DOWNLOAD`, wait past `BLOCK_STALL_SECS`, verify the watchdog fires `WATCHDOG_BLOCK_STALL`. Then verify that after recovery, blocks are actually queued (once Agent2's fix lands).

**d) Tip detection with stale peer starting_height:**
Test `syncsvc_note_valid_block()` where `node->starting_height` is stale (peer advanced while we synced) but `tip_is_recent` is true. Verify `should_set_sync_state` transitions to `SYNC_AT_TIP`.

### Target: 4 new test functions.

---

## Task 2: P2P Message Fuzzing Hardening

Malformed P2P messages shouldn't crash the node. Harden the message handlers.

### Files to investigate:
- `lib/net/src/msg_headers.c` — process_headers()
- `lib/net/src/msg_blocks.c` — process_block()
- `lib/net/src/msg_version.c` — process_version()
- `lib/net/src/msg_tx.c` — process_tx()

### What to check and fix for each handler:
1. Does it validate stream length before reading? (prevent buffer over-read)
2. Does it handle truncated messages gracefully? (return false, don't crash)
3. Does it disconnect on malformed input rather than crashing?
4. Are all error paths logged?

### Test:
Add fuzz-style tests in `lib/test/src/test_msg_handlers.c` (create if needed) that feed truncated/garbage data to each handler and verify no crash + clean error return.

---

## Task 3: MCP Health Endpoint — Add Sync Pipeline Visibility

The `zcl_health` and `zcl_status` MCP tools need to surface the download pipeline state so operators (and AI agents) can see WHY the node is stuck.

### Files to modify:
- `app/services/src/node_health_service.c` — Add download stats to health
- `tools/mcp/controllers/ops_controller.c` or wherever `zcl_status` is implemented

### What to add to `zcl_status` output:
```json
{
    "download": {
        "queue_size": 0,
        "in_flight": 0,
        "total_requested": 0,
        "total_received": 0,
        "total_timed_out": 0,
        "throughput_mbps": 0.0
    },
    "memory_rss_mb": 2048,
    "uptime_secs": 1260
}
```

### Implementation:
1. Add `get_rss_kb()` using `/proc/self/status` VmRSS
2. Track boot time with a static `time(NULL)` at init
3. Call `dl_get_stats()` and `dl_get_throughput()` from the status handler
4. Expose in the JSON output

### Note: Agent2 is adding a separate `zcl_syncdiag` tool. Your additions go into the existing `zcl_status` tool so the one-call diagnostic is complete.

---

## Task 4: Rate-Limit Peer Misbehavior Scoring

### Files to investigate:
- `lib/net/src/peer_scoring.c` — Current scoring implementation
- `lib/net/include/net/peer_scoring.h` — Score types and thresholds

### What to check and improve:
1. Read the current scoring system
2. Verify peers reaching ban threshold are actually disconnected
3. Add rate limiting: if a peer sends >100 rejected messages in 60 seconds, disconnect immediately
4. Add logging: `[scoring] peer %s banned: score %d, offences: %s`

---

## Task 5: Add Structured Boot Timing Log

### File: `config/src/boot.c`

### What to implement:
```c
struct timespec boot_start;
clock_gettime(CLOCK_MONOTONIC, &boot_start);

// After each phase:
int64_t phase_ms = elapsed_ms_since(&boot_start);
printf("[boot] %-30s %lldms\n", "load_block_index", phase_ms);
```

Phases to time:
1. SQLite open + schema migration
2. Block index load from cache
3. Block index height repair
4. UTXO set load / verification
5. Wallet load
6. P2P network start
7. Total boot time

Emit: `event_emitf(EV_BOOT_COMPLETE, 0, "total_ms=%lld", total_ms);`

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 19: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `app/services/src/header_sync_service.c` (Agent2)
- `lib/net/src/msg_headers.c` (Agent2)
- `lib/net/src/download.c` (Agent2)
- `lib/net/src/connman.c` (Agent2)
- `lib/net/src/compact_blocks.c` (Agent2)
- `lib/net/src/msgprocessor.c` (Agent2)
