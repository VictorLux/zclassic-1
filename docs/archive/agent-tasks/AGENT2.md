# AGENT2 — Wave 26: Enable Everything + ZSLP On-Chain Send

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Node at tip (3,079,015), healthy, synced. Your triple-wipe fix and memory diagnosis were excellent. Now: enable the disabled features and implement a missing feature.

---

## Task 1 (DO FIRST — OVERDUE): Re-enable bg_hash_verify + Address Backfill

These have been assigned for 4 waves. Do them NOW before anything else.

**bg_hash_verify:** The SIGSEGV was fixed in wave 22b (cs_main lock in `bg_hash_verification_service.c`). Find where the feature is launched during boot and make sure it runs.

```bash
grep -rn 'bg_hash_verif' config/src/boot.c config/src/boot_services.c app/services/src/ --include='*.c'
```

Look for: a function like `bg_hash_verify_start()` or `bg_hash_verification_service_start()`. If it's called but gated by a flag, check the flag. If the call is missing entirely, add it to the boot sequence after sync completes.

**Address backfill:** The SIGSEGV was fixed in wave 22b (mmap_size=0). Find the launch point:

```bash
grep -rn 'backfill_address\|start_backfill\|backfill_thread' config/src/ app/services/src/ --include='*.c'
```

For each: verify the function exists, is called during boot, and isn't gated by a hardcoded `false`. If it crashes, the fix from wave 22b should prevent that. Test by checking if the feature actually runs after boot.

**Deliverable:** Both features running. Show evidence in commit message (e.g., "bg_hash_verify started, verified to h=1000 after 60s").

---

## Task 2: Implement ZSLP On-Chain SEND

### File: `app/controllers/src/zslp_controller.c`, line 248

There's a TODO: "build and broadcast ZSLP SEND transaction on-chain"

The ZSLP token protocol works for validation/indexing but can't originate SEND transactions from this node. Implement it:

1. Read the ZSLP protocol spec in `lib/zslp/src/slp.c` — understand the OP_RETURN format for SEND
2. Build the OP_RETURN: `lokad_id(4) + tx_type(1) + token_id(32) + amounts(8 each)`
3. Create a transaction with the OP_RETURN output + change output
4. Sign and broadcast via the existing wallet/tx infrastructure

Look at how `zcl_name_register` builds its OP_RETURN transaction — same pattern.

---

## Task 3: Fix nSolution Memory Leak

`process_block.c:317` allocates 1,344 bytes per block for `nSolution` but never frees it. For blocks received via P2P (not loaded from disk where it's NULL), this leaks ~1.3KB per block forever.

Fix: after a block is validated and connected, free `nSolution` if it's no longer needed:
```c
if (pindex->nSolution) {
    free(pindex->nSolution);
    pindex->nSolution = NULL;
}
```

Only free AFTER the block is fully validated (equihash check uses nSolution). The best place is after `connect_block` succeeds in `connect_tip`.

---

## Build & Test

```bash
git pull origin master
make -j$(nproc) && make test
git add <files> && git commit -m "wave 26 task N: description"
git push origin master
```
