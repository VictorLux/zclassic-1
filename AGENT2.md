# AGENT2 — Wave 20: Fix Sync Stall Root Cause

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

**CRITICAL BUG:** The node is stuck at height 2,016,354 while the network tip is ~3,078,009 (over 1M blocks behind). The node falsely enters `SYNC_AT_TIP` and stops aggressively requesting headers.

### Root Cause Chain:
1. `syncsvc_evaluate_block_acceptance()` in `block_sync_service.c` declares `headers_caught_up = true` when `best_header_height <= new_tip_height + 1` — comparing headers against LOCAL blocks, not peer heights
2. Once `headers_caught_up`, the node enters `SYNC_AT_TIP`
3. At tip, `syncsvc_getheaders_interval()` returns 120s (slow polling)
4. The watchdog only checks for stalls in `SYNC_HEADERS_DOWNLOAD` and `SYNC_BLOCKS_DOWNLOAD` — it has NO check for `SYNC_AT_TIP` being wrong
5. `getblockchaininfo` reports `verificationprogress=1.0` (hardcoded) and `headers=blocks` (both use chain tip), masking the problem completely

---

## Task 1: Fix False SYNC_AT_TIP Transition

### File: `app/services/src/block_sync_service.c`

The `headers_caught_up` check at line ~109 is wrong. It only compares headers to local blocks:
```c
// CURRENT (wrong):
headers_caught_up =
    (best_header_height >= 0 && best_header_height <= new_tip_height + 1);
```

Fix: also require that we're close to the peer's height:
```c
// FIXED:
bool headers_near_blocks =
    (best_header_height >= 0 && best_header_height <= new_tip_height + 1);
bool near_peer_tip =
    (node->starting_height <= 0 || new_tip_height >= node->starting_height - 10);
headers_caught_up = headers_near_blocks && near_peer_tip;
```

This ensures `SYNC_AT_TIP` only fires when:
- Headers are caught up to blocks (existing check), AND
- Blocks are within 10 of the peer's advertised height (new check)

The `-10` margin handles the edge where the peer's `starting_height` is slightly stale.

### Test: Add to existing tests for `syncsvc_evaluate_block_acceptance`:
```c
// False at-tip: headers==blocks but far from peer
syncsvc_evaluate_block_acceptance(&result, &node_3m, SYNC_BLOCKS_DOWNLOAD,
    2016354, 2016354, old_time);
assert(!result.should_set_sync_state); // Must NOT transition to AT_TIP

// True at-tip: headers==blocks AND near peer
syncsvc_evaluate_block_acceptance(&result, &node_3m, SYNC_BLOCKS_DOWNLOAD,
    3078000, 3078001, 0);
assert(result.next_sync_state == SYNC_AT_TIP);
```

---

## Task 2: Watchdog — Detect Stale SYNC_AT_TIP

### File: `app/services/src/sync_watchdog_service.c`

Add a new check **after** the existing HEADER_STALL and HEADER_LAG checks (around line ~432):

```c
/* NEW: FALSE_TIP: in SYNC_AT_TIP but far behind peers */
if (state == SYNC_AT_TIP && duration > 60) {
    int max_peer_height = connman_max_peer_height(cm);
    int our_height = -1;
    if (ms) {
        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (tip) our_height = tip->nHeight;
    }
    
    if (max_peer_height > 0 && our_height >= 0 &&
        max_peer_height - our_height > 144) {
        printf("[watchdog] FALSE_TIP: at_tip but %d blocks behind peers "
               "(our=%d, peer_max=%d)\n",
               max_peer_height - our_height, our_height, max_peer_height);
        
        /* Force back to SYNC_HEADERS_DOWNLOAD */
        sync_set_state(SYNC_HEADERS_DOWNLOAD,
                       "watchdog FALSE_TIP: behind peers");
        record_recovery(now, WATCHDOG_STATE_STUCK);
        return WATCHDOG_STATE_STUCK;
    }
}
```

This catches the scenario where the node is falsely at tip. Even after Task 1 fixes the root cause, this watchdog check prevents future regressions.

### Test: Add to `test_sync_watchdog.c`:
```c
TEST("watchdog detects false SYNC_AT_TIP when far behind peers") {
    sync_set_state(SYNC_AT_TIP, "test");
    // Set peer starting_height to 3M, our height to 2M
    // Assert watchdog fires WATCHDOG_STATE_STUCK
}
```

---

## Task 3: Force Aggressive Headers When Behind

### File: `app/services/src/header_sync_service.c`

The interval logic in `syncsvc_getheaders_interval()` (line ~249) uses `our_height` (which is block height) to decide the interval. Even with Task 1's fix, we need the header sync to be aggressive when behind.

**Fix:** Add a check for `best_header_height` vs `peer_starting_height`:
```c
static int64_t syncsvc_getheaders_interval(const struct p2p_node *node,
                                           int our_height)
{
    int64_t base;
    if (syncsvc_is_initial_block_download(node, our_height))
        base = 10;
    else if (node->starting_height > 0 && our_height < node->starting_height)
        base = 30;
    else
        base = 120;
    // ... existing backoff
```

The problem is `our_height` is block height, so once blocks catch up to headers (but both are 1M behind peers), `our_height < node->starting_height` is true and we get 30s. That's acceptable.

BUT: there's still the case where `syncsvc_is_initial_block_download()` returns false because `our_height` (2M blocks) is within 144 of headers (2M headers), even though peer is at 3M. Fix `syncsvc_is_initial_block_download`:

```c
bool syncsvc_is_initial_block_download(const struct p2p_node *node,
                                       int our_height)
{
    if (!node) return false;
    return (node->starting_height > 0 &&
            our_height < node->starting_height - 144);
}
```

This already compares against `node->starting_height`, so if `our_height=2M` and `starting_height=3M`, IBD=true, interval=10s. **Verify this is actually being called with block height, not header height.** Check the call site in `msgprocessor.c` line ~2376 where `our_height = msg_get_height(mp)` — this uses `active_chain_height()` which is block height. So the IBD check should already work when blocks are behind. 

The real issue might be that when the node is at `SYNC_AT_TIP`, the periodic getheaders path at line ~2488 in `msgprocessor.c` is not reached. Trace the code flow:

1. Check if the `should_sync` flag is set for AT_TIP peers
2. If not, that's why no getheaders are being sent

Look at `syncsvc_begin_peer_sync()` — does it return false when we're AT_TIP? If so, the periodic getheaders never fires.

**The fix may be:** In msgprocessor.c's send loop, always call `syncsvc_plan_periodic_getheaders()` regardless of sync state, or ensure `should_sync` is true when we have outbound peers.

---

## Task 4: Reset Stale Counts on State Transition Out of AT_TIP

### File: `app/services/src/header_sync_service.c`

When the watchdog kicks the node out of `SYNC_AT_TIP` back to `SYNC_HEADERS_DOWNLOAD`, the `getheaders_stale_count` on peers may be high from the AT_TIP phase (where empty header responses are expected). This causes exponential backoff on the very peers we need.

Add a function to reset stale counts:
```c
void syncsvc_reset_peer_stale_counts(struct connman *cm);
```

Call it from the watchdog when transitioning from AT_TIP to HEADERS_DOWNLOAD.

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
- `app/controllers/src/blockchain_controller.c` (Agent3)
- `tools/mcp/controllers/ops_controller.c` (Agent3)
- `app/services/src/block_index_integrity.c` (Agent1/Agent3)
- `config/src/boot.c` (Agent3)
- `Makefile` (Agent3)
