# AGENT2 — Wave 20: Sync Robustness & Watchdog Hardening

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context: What Agent1 Already Fixed

Agent1 shipped two critical fixes:

1. **Chain activation was unreachable** (msg_headers.c): The activation code was inside `if (should_queue_needed_blocks)` which requires `count > 0`, but activation fires when count == 0 (all blocks already have data). Moved activation outside the guard. DEPLOYED.

2. **OOM on deep connect** (process_block.c): `activate_best_chain()` tried to connect 1M+ blocks at once, pushing RAM to 9GB. Added 500-block batching with flush between batches. DEPLOYED.

The node is now activating and connecting blocks in batches. Your tasks focus on making the remaining sync pipeline more robust.

---

## Task 1: Watchdog Should Detect Stale AT_TIP State

Once the node reaches what it thinks is tip, the watchdog stops monitoring. But if the node falsely enters AT_TIP (e.g. headers_caught_up is true but we're 1M blocks behind peers), it stays stuck forever.

### File: `app/services/src/sync_watchdog_service.c`

### Current code (line 502):
```c
if (state != SYNC_AT_TIP && duration > state_stuck_timeout(state)) {
```
The `state != SYNC_AT_TIP` guard means AT_TIP is never checked.

### Fix:
Add a new check: if we're at AT_TIP but peers report heights much higher than ours, transition back to HEADERS_DOWNLOAD.

```c
/* e. STALE_TIP: at_tip but peers are far ahead */
if (state == SYNC_AT_TIP && duration > 120) {
    int our_height = -1;
    if (ms)
        our_height = active_chain_height(&ms->chain_active);
    int max_peer = connman_max_peer_height(cm);
    if (max_peer > 0 && our_height >= 0 && max_peer > our_height + 144) {
        printf("[watchdog] STALE_TIP: at_tip h=%d but peers at %d "
               "(gap %d), reverting to HEADERS_DOWNLOAD\n",
               our_height, max_peer, max_peer - our_height);
        event_emitf(EV_SYNC_STATE_CHANGE, 0,
                    "watchdog STALE_TIP: h=%d peers=%d gap=%d",
                    our_height, max_peer, max_peer - our_height);
        if (!sync_set_state(SYNC_HEADERS_DOWNLOAD,
                            "watchdog STALE_TIP recovery")) {
            sync_set_state(SYNC_IDLE, "watchdog STALE_TIP via idle");
            sync_set_state(SYNC_HEADERS_DOWNLOAD,
                           "watchdog STALE_TIP recovery");
        }
        record_recovery(now, WATCHDOG_STATE_STUCK);
        return WATCHDOG_STATE_STUCK;
    }
}
```

Add this BEFORE the existing STATE_STUCK check (line 502). The 144-block threshold is ~1 day of blocks — enough to distinguish "slightly behind" from "truly stale".

---

## Task 2: BLOCK_STALL Force-Populates Queue from Block Index

When BLOCK_STALL fires (5 minutes with no height progress), the current recovery only re-queues timed-out in-flight blocks. But if the queue was NEVER populated, this is a no-op.

### File: `app/services/src/sync_watchdog_service.c`

### Fix:
After the existing BLOCK_STALL recovery (line ~494), add:

```c
// After re-queuing timed-out blocks, check if queue is still empty
uint64_t post_queued = 0, post_inflight = 0;
dl_get_stats(dm, NULL, NULL, NULL, &post_inflight, &post_queued);
if (post_queued == 0 && post_inflight == 0 && ms) {
    int chain_h = active_chain_height(&ms->chain_active);
    struct uint256 scan_hashes[256];
    int32_t scan_heights[256];
    size_t scan_count = 0;
    size_t iter = 0;
    struct block_index *bi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi || scan_count >= 256) break;
        if (bi->nHeight <= chain_h || bi->nHeight > chain_h + 2048) continue;
        if (bi->nStatus & BLOCK_HAVE_DATA) continue;
        if (bi->nStatus & BLOCK_FAILED_MASK) continue;
        if (!bi->phashBlock) continue;
        scan_hashes[scan_count] = *bi->phashBlock;
        scan_heights[scan_count] = bi->nHeight;
        scan_count++;
    }
    if (scan_count > 0) {
        dl_queue_blocks(dm, scan_hashes, scan_heights, scan_count);
        printf("[watchdog] BLOCK_STALL: force-queued %zu blocks\n", scan_count);
    } else {
        printf("[watchdog] BLOCK_STALL: no downloadable blocks, "
               "reverting to HEADERS_DOWNLOAD\n");
        sync_set_state(SYNC_HEADERS_DOWNLOAD,
                       "watchdog BLOCK_STALL: no blocks available");
    }
}
```

---

## Task 3: Add `zcl_syncdiag` MCP Tool (if not done)

Check if `zcl_syncdiag` is already wired and working. If the agent2 previous work added it to `ops_controller.c`, verify it compiles and returns useful data. If not, add it.

Key fields: `sync_state`, `chain_height`, `best_header_height`, `peer_max_height`, `download_queue_size`, `download_in_flight`, `watchdog_escalation`, `blocks_per_sec`.

---

## Task 4: Fix `syncsvc_note_valid_block` False AT_TIP Detection

### File: `app/services/src/block_sync_service.c`

The `reached_peer` check on line 104 uses `node->starting_height` which is set at handshake time. If the peer advanced while we synced, we never "reach" the stale starting_height and the tip_is_recent check (line 101-102) becomes the only path to AT_TIP.

### Fix:
Add peer height staleness detection. If `starting_height` is more than 144 blocks below `connman_max_peer_height()`, don't use it for at-tip detection:

```c
bool reached_peer = (node->starting_height > 0 &&
                     new_tip_height >= node->starting_height);
// Guard against stale starting_height
int max_peer_h = connman_max_peer_height(/* need connman access */);
if (reached_peer && max_peer_h > 0 &&
    max_peer_h > node->starting_height + 144)
    reached_peer = false;  // starting_height is stale
```

Note: you'll need to pass connman into this function or use a global accessor. Check how the watchdog accesses it.

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 20 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `lib/net/src/msg_headers.c` (Agent1 — activation fix)
- `lib/validation/src/process_block.c` (Agent1 — batch connect)
- `app/models/src/*` (Agent3)
- `config/src/boot.c` (Agent3)
