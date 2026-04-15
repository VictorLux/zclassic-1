# AGENT2 — Wave 24: Boot Resilience + Feature Re-enable

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

The UTXO/chain mismatch is being resolved via force_utxo_wipe + replay from genesis. The wipe+replay fallback from your wave 23c code is running now.

Your focus: make the boot sequence more resilient so this class of bug doesn't recur, and re-enable features fixed in wave 22b.

---

## Task 1 (HIGH): Fix coins_best_block Resolution After LDB Import

### Problem
After importing UTXOs from zclassicd LevelDB, the `coins_best_block` hash from the `'B'` key often can't be found in our block_index. This is because zclassicd's tip block hasn't been received as a header yet. The current code falls through silently and leaves chain tip wrong.

### Fix in `config/src/boot.c`
After LDB import sets `coins_best_block`, if the hash can't be found in block_map:

1. Query the LDB chainstate for the best block height (read the height from the block header stored alongside the `'B'` key, or look up by iterating)
2. If we can't get the height from LDB either, find the highest block in our block_index that has `BLOCK_HAVE_DATA` — that's our best guess for where the UTXO set is valid
3. Set chain tip to that block
4. Log clearly: `[boot] coins_best_block hash not in index — setting tip to highest HAVE_DATA block at h=N`

The key insight: even if we can't find the exact hash, the UTXO set is valid for some height range. The highest HAVE_DATA block is a safe conservative choice because all blocks up to that point have been written to disk (from zclassicd).

---

## Task 2 (MEDIUM): Re-enable bg_hash_verify

The SIGSEGV was fixed in wave 22b (cs_main lock + field snapshotting).

Search: `grep -rn 'bg_hash_verify\|bg_hash_verification\|nobghash' app/ config/ lib/ --include='*.c' --include='*.h'`

Find where it's disabled. If it's gated by a hardcoded bool or commented-out call, re-enable it. If gated by `-nobgvalidation`, leave it user-controlled.

---

## Task 3 (MEDIUM): Re-enable Address Backfill

The SIGSEGV was fixed in wave 22b (mmap_size=0).

Search: `grep -rn 'backfill_address\|address_backfill\|nobackfill' app/ config/ lib/ --include='*.c' --include='*.h'`

Find and re-enable.

---

## Build & Test

```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 24 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/header_sync_service.c` (Agent1)
- `lib/validation/src/process_block.c` (Agent1)
- `app/services/src/chain_activation_controller.c` (Agent3)
