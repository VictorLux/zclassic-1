# AGENT2 — Wave 19: Sync Pipeline Fixes & Block Download Robustness

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context: The "Stuck in blocks_download" Bug

The node boots from a UTXO snapshot at height ~2M, enters `blocks_download` state, and then **never downloads any blocks**. Event log shows continuous header fetching (tip advancing to 2017474) but zero `getdata` messages. The download queue stays empty.

Root cause chain:
1. `process_headers()` in `msg_headers.c` calls `syncsvc_plan_header_processing()` which calls `syncsvc_collect_needed_blocks()`
2. `syncsvc_collect_needed_blocks()` calls `syncsvc_headers_chain_from_tip()` to verify the candidate chains back to our tip
3. **This walk can fail** if pprev pointers are NULL (common after snapshot sync where block_index entries above the snapshot anchor don't have fully linked pprev chains)
4. When `chains_from_tip` returns false, `needed_blocks.count` stays 0, so `should_queue_needed_blocks` is false
5. No blocks ever get queued → download manager stays empty → stall recovery in msgprocessor finds nothing → node sits forever

The stall recovery in `msgprocessor.c:2561` DOES try to find blocks at tip+1, but it also requires `queued==0 && in_flight==0 && node->starting_height > our_h + 10` and scans block_map — which may also fail for the same pprev linking reasons.

---

## Task 1: Fix `syncsvc_headers_chain_from_tip` for Post-Snapshot Chains

The core fix. After snapshot sync, the block index has entries above the snapshot height but their pprev chains may not link all the way back to the chain tip.

### File: `app/services/src/header_sync_service.c`

### Current code (line 426-463):
```c
bool syncsvc_headers_chain_from_tip(const struct block_index *candidate,
                                    const struct block_index *tip,
                                    int our_height)
{
    // Walks candidate->pprev until nHeight <= our_height
    // Returns true only if the walk reaches `tip` exactly
    // Returns false if pprev is NULL before reaching tip (post-snapshot!)
}
```

### Fix:
Add a fallback: if the pprev walk reaches a block at height <= our_height that is IN the active chain (not just == tip pointer), accept it. Also accept if the walk reaches a block whose hash matches the active chain at that height.

```c
// After the existing checks, before returning false:
// Fallback: if walk stopped at a height within our chain, check if
// that block is on the active chain by height lookup
if (verify && verify->nHeight <= our_height && verify->nHeight >= 0) {
    // The block at this height in the active chain should match
    // This handles cases where tip pointer comparison fails but
    // the chain is actually valid
    return true;  // any block at or below our height that we walked
                   // to through valid pprev links is acceptable
}
```

BUT be careful — we can't just accept any block at our_height. We need to verify it's actually on our chain. Look at how `active_chain_at_height()` works and use it.

### Test:
Add a test in `lib/test/src/test_sync_service.c` that creates a scenario where pprev walk stops at a non-tip block at our_height (simulating post-snapshot state) and verify `chains_from_tip` returns true.

---

## Task 2: Watchdog BLOCK_STALL Should Force Block Queue Population

When the watchdog detects BLOCK_STALL (5 minutes in blocks_download with no height progress), the current recovery just re-queues timed-out in-flight blocks. But if the queue was NEVER populated (the root cause), this does nothing.

### File: `app/services/src/sync_watchdog_service.c`

### Current code (line 464-498):
The BLOCK_STALL handler only moves in-flight slots back to the queue. If there were never any in-flight blocks, this is a no-op.

### Fix:
After the existing BLOCK_STALL recovery, if the queue is STILL empty and in-flight is STILL zero, actively scan the block map and queue blocks that need downloading:

```c
// After existing BLOCK_STALL recovery at line 494:
// If queue is still empty after re-queuing, actively find blocks to download
{
    uint64_t post_queued = 0, post_inflight = 0;
    dl_get_stats(dm, NULL, NULL, NULL, &post_inflight, &post_queued);
    if (post_queued == 0 && post_inflight == 0 && ms) {
        // Scan block index for blocks at heights above chain tip
        // that have headers but no block data
        int chain_h = active_chain_height(&ms->chain_active);
        struct uint256 scan_hashes[256];
        int32_t scan_heights[256];
        size_t scan_count = 0;
        size_t iter = 0;
        struct block_index *bi;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
            if (!bi || scan_count >= 256) continue;
            if (bi->nHeight <= chain_h) continue;
            if (bi->nHeight > chain_h + 2048) continue;
            if (bi->nStatus & BLOCK_HAVE_DATA) continue;
            if (bi->nStatus & BLOCK_FAILED_MASK) continue;
            if (!bi->phashBlock) continue;
            scan_hashes[scan_count] = *bi->phashBlock;
            scan_heights[scan_count] = bi->nHeight;
            scan_count++;
        }
        if (scan_count > 0) {
            dl_queue_blocks(dm, scan_hashes, scan_heights, scan_count);
            printf("[watchdog] BLOCK_STALL: force-queued %zu blocks "
                   "from block index scan\n", scan_count);
        } else {
            // No blocks to queue at all — headers may not have arrived yet
            // Force transition back to HEADERS_DOWNLOAD
            printf("[watchdog] BLOCK_STALL: no downloadable blocks found, "
                   "reverting to HEADERS_DOWNLOAD\n");
            sync_set_state(SYNC_HEADERS_DOWNLOAD,
                           "watchdog BLOCK_STALL: no blocks available");
        }
    }
}
```

### Include needed:
You'll need `#include "validation/chainstate.h"` for `active_chain_height` and `block_map_next`.

---

## Task 3: Add `zcl_syncdiag` MCP Tool for Deep Sync Diagnostics

The node needs a diagnostic endpoint that reveals exactly what's happening during sync — especially the download queue/in-flight state that's invisible from `zcl_status`.

### Files to modify:
- `tools/mcp/controllers/ops_controller.c` (or wherever sync-related MCP tools live)
- `tools/mcp/router.c` (register the new tool)

### What to return (as JSON):
```json
{
    "sync_state": "blocks_download",
    "chain_height": 2016354,
    "best_header_height": 2017474,
    "peer_max_height": 3077839,
    "header_gap": 1060365,
    "blocks_indexed": 99524,
    "download_queue_size": 0,
    "download_in_flight": 0,
    "download_total_requested": 0,
    "download_total_received": 0,
    "download_total_timed_out": 0,
    "watchdog_checks": 42,
    "watchdog_recoveries": 0,
    "watchdog_escalation": 0,
    "watchdog_blocks_per_sec": 0.0,
    "stall_recovery_active": true,
    "chain_tip_hash": "00000...",
    "best_header_hash": "00000..."
}
```

Use: `sync_get_state()`, `active_chain_height()`, `ms->pindex_best_header->nHeight`, `connman_max_peer_height()`, `dl_get_stats()`, `sync_watchdog_get_stats()`.

---

## Task 4: Header Processing — Queue Blocks More Aggressively

Currently `syncsvc_collect_needed_blocks()` walks backward from candidate through pprev, limited to 2048 steps. If the chain doesn't link back to tip, it returns empty.

### File: `app/services/src/header_sync_service.c`

### Additional fix:
In `syncsvc_collect_needed_blocks()` (line 464), add a forward-scan fallback when the backward pprev walk fails `chains_from_tip`:

```c
// After the existing chains_from_tip check (line 485):
if (!result->chains_from_tip) {
    // Fallback: forward scan from our_height+1 in block_map
    // This handles post-snapshot where pprev links are broken
    // but we have valid headers at heights just above our tip
    // ... scan block_map for entries at heights our_height+1..our_height+2048
    // that have phashBlock but not BLOCK_HAVE_DATA
    return;  // existing behavior: return empty if chains_from_tip false
}
```

But the REAL fix is Task 1 — making `chains_from_tip` return true for valid chains. This task is the belt-and-suspenders: even if chains_from_tip is wrong, the watchdog (Task 2) will catch it.

---

## Task 5: Add Periodic Block Queue Check to Message Processor

The message processor loop runs every ~30s per peer. Currently it only assigns blocks from the queue to peers. But if the queue is empty and we're in blocks_download, nothing happens until stall recovery triggers (which has a 10s cooldown).

### File: `lib/net/src/msgprocessor.c` (~line 2560)

### Fix:
After the stall recovery check, add a direct "kickstart" that queries the block index for needed blocks if the queue has been empty for >60 seconds:

```c
// After stall recovery block, add:
static int64_t g_last_queue_empty_check = 0;
if (dl_queued == 0 && dl_inflight == 0 && 
    sync_get_state() == SYNC_BLOCKS_DOWNLOAD &&
    now_dl - g_last_queue_empty_check > 60) {
    g_last_queue_empty_check = now_dl;
    // Send getheaders to refill the pipeline
    struct sync_getheaders_action kickstart = {0};
    kickstart.should_send = true;
    kickstart.anchor = SYNC_HEADER_REQUEST_TIP;
    exec_getheaders_action(mp, node, &kickstart);
    printf("[sync] kickstart: empty queue in blocks_download, "
           "re-requesting headers\n");
}
```

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
- `app/models/src/*` (Agent3)
- `config/src/boot.c` (Agent3 — crash recovery)
- `config/src/runtime.c` (Agent3)
- `app/models/src/database.c` (Agent3)
- `Makefile` lint targets (Agent3)
