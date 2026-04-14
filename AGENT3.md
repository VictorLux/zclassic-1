# AGENT3 — Wave 23: Fix Activation False AT_TIP + Enable Disabled Features

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

The node is STUCK at height 2,016,355. Headers arrive but blocks are never downloaded. Agent2 fixes the download pipeline (Bug 1 + Bug 2). You fix Bug 3 (false AT_TIP) and re-enable the features that were fixed in wave 22b.

---

## Task 1 (CRITICAL): Fix False AT_TIP in Activation Controller

### File: `app/services/src/chain_activation_controller.c`, line ~297

```c
if (!ok) {
    activation_set_state(ctl, ACTIVATION_READY, "activation_failed");
} else {
    activation_set_state(ctl, ACTIVATION_AT_TIP, "at_tip");  // ← unconditional
}
```

`activate_best_chain` returns `true` after connecting one block (2016356), because `find_most_work_chain` only sees blocks with `nChainTx > 0`. The activation controller then declares AT_TIP even though the node is 1M blocks behind.

### Fix:
After `activate_best_chain` returns true, check if we're actually near the network tip before declaring AT_TIP. Compare the chain tip height against known peer heights:

```c
if (!ok) {
    activation_set_state(ctl, ACTIVATION_READY, "activation_failed");
} else {
    /* Don't declare at_tip if we're far behind peers — blocks
     * may not be downloaded yet (nChainTx==0 hides them from
     * find_most_work_chain). Stay in CONNECTING to keep the
     * download pipeline active. */
    int tip_h = active_chain_height(&ms->chain_active);
    int peer_h = connman_max_peer_height(ms->connman);
    if (peer_h > 0 && tip_h + 100 < peer_h) {
        activation_set_state(ctl, ACTIVATION_READY, "behind_peers");
    } else {
        activation_set_state(ctl, ACTIVATION_AT_TIP, "at_tip");
    }
}
```

Check that `connman_max_peer_height` exists and works — it was added in wave 20. Use a margin of 100 blocks to avoid flapping when nearly synced.

---

## Task 2: Update CHECKLIST.md — Mark Fixed Items

The following items in CHECKLIST.md section 6 "Remaining" are now FIXED. Move them to "Completed":

- **SIGSEGV in bg_hash_verify fread** — FIXED in wave 22b task 3 (cs_main lock added, snapshot block_index fields before pread)
- **SIGSEGV in address backfill** — FIXED in wave 22b task 4 (disabled mmap for bg thread, PRAGMA mmap_size=0)

Update the checklist to reflect current reality.

---

## Task 3: Re-enable bg_hash_verify

### File: Check where bg_hash_verify is disabled

The SIGSEGV was fixed in wave 22b (cs_main lock + field snapshotting). Find where the feature is disabled and re-enable it. It should start automatically after sync completes.

Search for:
```bash
grep -rn 'bg_hash_verify\|bg_hash_verification\|nobghash' app/ config/ lib/ --include='*.c' --include='*.h'
```

If it's disabled by a flag or a commented-out call, re-enable it. If it's gated by `-nobgvalidation`, that's fine — leave it as user-controlled.

---

## Task 4: Re-enable Address Backfill

### File: Check where address backfill is disabled

The SIGSEGV was fixed in wave 22b (mmap_size=0). Find where the backfill thread is disabled and re-enable it.

Search for:
```bash
grep -rn 'backfill_address\|address_backfill\|nobackfill' app/ config/ lib/ --include='*.c' --include='*.h'
```

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
- `app/services/src/header_sync_service.c` (Agent2)
- `lib/net/src/msg_headers.c` (Agent2)
- `lib/net/src/download.c` (Agent2)
