# AGENT2 — Wave 23: Fix Block Download Stall (CRITICAL)

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

The node is STUCK at height 2,016,355. Headers arrive (tip 2025K+) but blocks are NEVER requested. The event log shows a 250ms tight loop:

```
boot.activate: tip=2016355 most_work=2016356
connecting->at_tip: at_tip
getheaders sent → 160 headers received
at_tip->connecting: p2p_trigger
(repeat)
```

Root cause analysis identified THREE compounding bugs. You fix Bug 1 and Bug 2 (the download pipeline). Agent3 fixes Bug 3 (activation controller).

---

## Bug 1 (ROOT CAUSE): syncsvc_collect_needed_blocks Walk Terminates at pprev==NULL

### File: `app/services/src/header_sync_service.c`, line ~515

```c
while (walk && walk->pprev && walk->nHeight > our_height &&
       result->count < max_collect && walk_steps < 2048) {
```

New headers have `pprev == NULL` because they're header-only entries not yet connected. The `walk->pprev` condition terminates the walk immediately, yielding `count == 0`. With count=0, `should_queue_needed_blocks = false` and no `getdata` is ever sent.

### Fix:
Remove the `walk->pprev` requirement from the walk condition. Instead, handle NULL pprev gracefully inside the loop — if `pprev` is NULL, try to look up the parent hash in the block map:

```c
while (walk && walk->nHeight > our_height &&
       result->count < max_collect && walk_steps < 2048) {
    // ... existing logic to check BLOCK_HAVE_DATA etc ...
    
    // Walk backwards — handle missing pprev
    if (walk->pprev) {
        walk = (struct block_index *)walk->pprev;
    } else {
        break;  // can't walk further — but we already collected what we could
    }
    walk_steps++;
}
```

The key insight: we should collect blocks ABOVE our_height that need data, walking DOWN from the candidate. Even if the walk stops early due to missing pprev, we should still have `count > 0` for the blocks we DID find. The current code breaks out with count=0 because `walk->pprev` is checked BEFORE the first iteration even runs.

**Test:** After this fix, `needed_blocks.count` should be > 0 when headers are ahead of our tip. Add a test case that creates header-only block_index entries (pprev=NULL, nChainTx=0, no BLOCK_HAVE_DATA) and verifies `syncsvc_collect_needed_blocks` returns count > 0.

---

## Bug 2: syncsvc_should_begin_blocks_download Requires SYNC_HEADERS_DOWNLOAD

### File: `app/services/src/header_sync_service.c`, line ~435

```c
bool syncsvc_should_begin_blocks_download(enum sync_state sync_state,
                                          const struct block_index *candidate,
                                          int our_height)
{
    return candidate && candidate->nHeight > our_height &&
           sync_state == SYNC_HEADERS_DOWNLOAD;
}
```

Once the state transitions to `SYNC_BLOCKS_DOWNLOAD`, this function returns `false` for all subsequent header batches. No more blocks are ever queued.

### Fix:
Allow block queuing in BOTH header and block download states:

```c
    return candidate && candidate->nHeight > our_height &&
           (sync_state == SYNC_HEADERS_DOWNLOAD ||
            sync_state == SYNC_BLOCKS_DOWNLOAD);
```

This is safe — the function's purpose is "should we queue blocks for download?" and the answer should be yes whenever we have a candidate above our height, regardless of which download phase we're in.

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 23 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/chain_activation_controller.c` (Agent3)
- `lib/validation/src/process_block.c` (Agent1 — coordinator)
