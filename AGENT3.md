# AGENT3 — Wave 20: Fix False At-Tip & Sync Observability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

The node is stuck at height 2,016,354 while the network tip is ~3,078,009 (1M blocks behind). Root cause: the node falsely enters `SYNC_AT_TIP` state because `getblockchaininfo` reports hardcoded `verificationprogress=1.0`, headers count reports block height (not actual best header), and the `headers_caught_up` check compares headers against local blocks instead of peer heights. Once at tip, getheaders requests drop to 120s intervals and the watchdog doesn't monitor `SYNC_AT_TIP` for being behind peers.

Agent2 owns the sync services. Agent3 fixes the RPC/observability side and adds test coverage for the false-tip scenario.

---

## Task 1: Fix getblockchaininfo to Report Real Sync State

The RPC lies about sync status. Fix it.

### File: `app/controllers/src/blockchain_controller.c`

**Fix 1: headers field** (line ~291)
Currently both `blocks` and `headers` use `active_chain_tip()->nHeight`. The `headers` field must report `pindex_best_header->nHeight`:
```c
// BEFORE (wrong):
json_push_kv_int(result, "headers", tip ? tip->nHeight : 0);

// AFTER (correct):
struct block_index *best_hdr = ctx->main_state->pindex_best_header;
int header_height = best_hdr ? best_hdr->nHeight : (tip ? tip->nHeight : 0);
json_push_kv_int(result, "headers", header_height);
```

**Fix 2: verificationprogress** (line ~300)
Currently hardcoded to `1.0`. Calculate it relative to the max peer starting_height:
```c
// BEFORE (wrong):
json_push_kv_real(result, "verificationprogress", 1.0);

// AFTER (correct):
int max_peer_h = connman_max_peer_height(ctx->connman);
int our_h = tip ? tip->nHeight : 0;
double progress = 1.0;
if (max_peer_h > 0 && our_h < max_peer_h)
    progress = (double)our_h / (double)max_peer_h;
json_push_kv_real(result, "verificationprogress", progress);
```

You'll need access to `connman` through the controller context. Check how other controllers (e.g. `network_controller.c`) access `connman` and follow the same pattern. If `connman_max_peer_height()` doesn't exist yet, Agent1 is adding it — pull first.

**Fix 3: Add `best_header_height` field**
Add a new field so operators can see the real header tip:
```c
json_push_kv_int(result, "best_header_height", header_height);
```

### Test:
- Verify `make test` still passes (the test for getblockchaininfo, if any, may need updating)
- The fix is mostly about correctness of reported values — no behavioral change

---

## Task 2: Fix MCP zcl_status to Expose Header vs Block Gap

### File: `tools/mcp/controllers/ops_controller.c` (or wherever `zcl_status` lives)

The MCP status endpoint should clearly show when headers are behind peers:
1. Find where `zcl_status` is implemented
2. Add fields:
   - `header_height`: from `pindex_best_header->nHeight`
   - `max_peer_height`: highest `startingheight` among connected peers
   - `header_gap`: `max_peer_height - header_height` (0 when caught up)
   - `sync_behind`: true if header_gap > 144

This makes it trivial for AI agents or monitoring to detect the stall.

---

## Task 3: Add Tests for False At-Tip Detection

### File: `lib/test/src/test_sync_service.c` (or create if needed)

Test the `syncsvc_evaluate_block_acceptance()` function from `block_sync_service.c`. This is where the false at-tip bug lives.

**Test cases:**
1. `headers_caught_up should be false when best_header == block_height but peer is 1M ahead`
   - Call with `new_tip_height=2016354, best_header_height=2016354, node.starting_height=3078009`
   - Assert `should_set_sync_state` is false (should NOT transition to AT_TIP)
   - **NOTE: This test will FAIL with current code.** That's expected — it documents the bug. Add a comment: `/* BUG: headers_caught_up only checks local state, not peer height — see wave 20 task for Agent2 */`

2. `headers_caught_up should be true when genuinely at tip`
   - Call with `new_tip_height=3078000, best_header_height=3078001, node.starting_height=3078009`
   - Assert `should_set_sync_state` is true and `next_sync_state == SYNC_AT_TIP`

3. `tip_is_recent bypasses headers_caught_up correctly`
   - Call with recent `new_tip_time` (now - 60), low `best_header_height`
   - Assert AT_TIP transition happens (the recent-tip path is correct)

**Important:** You can test `syncsvc_evaluate_block_acceptance()` directly since it's a pure function. Read the header file for the signature.

---

## Task 4: Add getheaders Diagnostic Logging

### File: `app/services/src/header_sync_service.c`

Add logging to make header sync behavior visible:

1. In `syncsvc_plan_periodic_getheaders()`: when a getheaders is planned, log:
   ```c
   printf("[headers] getheaders planned: peer=%d our_h=%d peer_start_h=%d interval=%llds\n",
          node->id, our_height, node->starting_height, interval);
   ```

2. In `syncsvc_getheaders_interval()`: when interval is > 60s, log (once per change):
   ```c
   printf("[headers] interval=%llds for peer %d (stale_count=%d)\n",
          base, node->id, stale);
   ```

This helps diagnose WHY headers stop: is the interval too long? Are getheaders not being sent?

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
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `app/services/src/block_index_integrity.c` (Agent2)
- `lib/net/src/download.c` (Agent2)
- `lib/net/src/connman.c` (Agent1)
- `lib/net/src/msgprocessor.c` (Agent1)
