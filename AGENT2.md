# AGENT2 — Wave 23c: Fix UTXO/Chain Tip Mismatch (CRITICAL BLOCKER)

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

**THE NODE IS STUCK.** Block download pipeline is now fixed (getdata sends, blocks arrive), but `connect_block` fails at h=2016356 with `bad-txns-inputs-missingorspent`.

**Root cause:** The UTXO set was imported from zclassicd's LevelDB chainstate, which is at height ~3,078,241. But our chain tip is 2,016,355. Block 2016356 tries to spend UTXOs that existed at height 2016355 — but those UTXOs were SPENT long ago in the zclassicd chain (between heights 2016356 and 3M). They no longer exist in the UTXO set.

**The mismatch:** UTXO set = height ~3M, chain tip = height ~2M. We can't connect block 2016356 because its inputs are missing from a UTXO set that's 1M blocks ahead.

---

## Task 1 (CRITICAL): Set Chain Tip to Match UTXO Set Height

### The Fix

After LDB UTXO import, the `coins_best_block` hash from LevelDB corresponds to the zclassicd tip (~3M). Our boot code should:

1. Read `coins_best_block` from the LDB chainstate (the `'B'` key in LevelDB)
2. Find that hash in our block_index
3. Set our `chain_active` tip to that block_index entry
4. The node then only needs to connect blocks from ~3M to current tip (~3,078K) — a few thousand blocks

### Investigation Steps

1. Read `config/src/boot.c` — find where `coins_best_block` is handled after LDB import
2. Read `lib/storage/src/coins_db.c` — find `coins_db_read_best_block` or similar
3. Check: after LDB import, what is `coins_best_block` set to? Is it the LDB's best block hash, or is it being overridden to our chain tip?
4. Find where `active_chain_set_tip` is called during boot — is it using the coins_best_block height or the block_index tip?

### Key Files
- `config/src/boot.c` — boot sequence, UTXO import, chain tip selection
- `lib/storage/src/coins_db.c` — LevelDB UTXO reader
- `lib/coins/src/coins_view_sqlite.c` — SQLite coins storage
- `config/src/boot_index.c` — block index loading

### What Success Looks Like
After your fix: node boots, imports UTXOs from LDB at height ~3M, sets chain tip to ~3M, connects ~50 blocks to reach network tip. No `bad-txns-inputs-missingorspent`.

---

## Task 2: Add Diagnostic Logging for UTXO/Chain Mismatch

After the LDB import, add a log line:
```c
printf("[boot] UTXO import: coins_best_block at h=%d, chain tip at h=%d%s\n",
       coins_h, tip_h, coins_h != tip_h ? " (MISMATCH — adjusting tip)" : "");
```

Also add logging when `coins_best_block` is resolved to a block_index entry vs. when it can't be found.

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 23c task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/header_sync_service.c` (Agent1)
- `lib/validation/src/process_block.c` (Agent1)
- `app/services/src/chain_activation_controller.c` (Agent3)
