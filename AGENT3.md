# AGENT3 — Wave 24: Sapling Persistence + Multi-threaded Validation

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

UTXO wipe+replay is running. While that proceeds, work on the remaining reliability items from the checklist.

---

## Task 1 (HIGH): Sapling Tree Persistence After Crash

SIGKILL loses WAL → 5-minute Sapling tree rebuild on next boot. Fix this.

### Investigation
1. Find Sapling tree persistence code — look in `lib/sapling/src/incremental_merkle_tree.c`, `config/src/boot.c`, `app/services/src/sync_service.c`
2. How is the tree stored? SQLite table? What table schema?
3. When is it flushed? Only on clean shutdown? Or after each block?
4. Can we add a periodic checkpoint? After Sapling tree updates, call `sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL)` every 1000 blocks

### Fix
Add periodic Sapling tree persistence so SIGKILL only loses ~1000 blocks of tree state, not all of it. The rebuild from 1000 blocks takes seconds, not 5 minutes.

---

## Task 2 (MEDIUM): Investigate Multi-threaded bg_validation Crash

bg_validation crashes with >1 worker. The file I/O is safe (pread) and block_index access is now locked (cs_main). The remaining suspect is the script interpreter.

### Investigation
1. Read `app/services/src/bg_validation_service.c` — find `verify_scripts_parallel` or equivalent
2. Check `lib/script/src/interpreter.c` for global/static state
3. Check if `secp256k1_context` is shared or per-thread
4. Look for any `static` variables in the script verification path

Report findings. If the fix is clear, implement it.

---

## Task 3 (LOW): Improve Boot Timing Output

The boot timing from wave 22b adds `[boot]` lines. Check that all phases are covered:
1. SQLite open + schema migration
2. Block index load
3. UTXO set load/import
4. Sapling tree load/rebuild
5. Total boot time

If any phases are missing timing, add them.

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
- `config/src/boot.c` UTXO import section (Agent2)
- `lib/storage/src/coins_db.c` (Agent2)
