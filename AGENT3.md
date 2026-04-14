# AGENT3 — Wave 23c: Fallback Sync Path + Reliability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Node stuck at 2,016,355 due to UTXO/chain tip mismatch. Agent2 is fixing the primary path (set chain tip to UTXO height after LDB import). Your job: build the fallback path and improve resilience.

---

## Task 1 (HIGH): Implement UTXO Wipe + Replay Fallback

If the coins_best_block can't be resolved in the block_index (hash not found), the node currently stalls. Add a fallback:

### File: `config/src/boot.c`

When `coins_best_block` hash is not found in the block_index:
1. Log: `[boot] WARNING: coins_best_block not in block index — wiping UTXOs and replaying from genesis`
2. Delete all UTXOs from SQLite: `DELETE FROM utxos`
3. Set `coins_best_block` to genesis hash
4. Set chain tip to genesis
5. The node will replay all blocks from genesis, rebuilding the UTXO set

This is the nuclear option but it's better than being stuck forever. The node has all block data on disk — replaying ~3M blocks takes a few hours but produces a correct UTXO set.

### Safety Guard
Before wiping, check UTXO count. If > 1,000,000, log a WARNING and require a flag file (`~/.zclassic-c23/force_utxo_wipe`) to proceed. This prevents accidental wipes.

---

## Task 2 (MEDIUM): Improve activate_best_chain Error Recovery

### File: `lib/validation/src/process_block.c`

When `connect_tip` fails with `bad-txns-inputs-missingorspent`:
1. Currently marks the block as BLOCK_FAILED after self-heal fails
2. After 3 failures, writes `needs_reimport` flag
3. But the reimport from the same stale LDB just reproduces the same mismatch

Add: after 5 consecutive failures at the same height, try disconnecting the tip (if possible) and retrying. If disconnect isn't possible (no undo data), log a clear message:
```
[recovery] UTXO mismatch at h=N: inputs missing. Chain tip and UTXO set are out of sync.
[recovery] Restart with -reimport-utxos or delete chainstate/ to force fresh import.
```

---

## Task 3 (LOW): Update CHECKLIST.md

Mark these as FIXED:
- Block download stalling after P2P catchup — FIXED wave 23 (HAVE_DATA in chain selection + state gate + AT_TIP check)
- SIGSEGV in bg_hash_verify — FIXED wave 22b
- SIGSEGV in address backfill — FIXED wave 22b

Add new remaining item:
- UTXO/chain tip mismatch after LDB import — coins at h=3M, chain at h=2M, connect fails

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
- `lib/storage/src/coins_db.c` (Agent2)
- `lib/coins/src/coins_view_sqlite.c` (Agent2)
- `app/services/src/header_sync_service.c` (Agent1)
