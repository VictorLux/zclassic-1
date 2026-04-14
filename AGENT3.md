# AGENT3 — Wave 23b: Sapling Persistence + Multi-threaded Validation

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Wave 23 progress:
- Block download pipeline fixed (Agent1 + Agent3)
- Node at 2,016,355 with headers at 3,078,027 — UTXO reimport in progress
- Two remaining reliability items from the checklist need investigation

---

## Task 1 (HIGH): Sapling Tree Persistence After Crash

### Problem
SIGKILL loses WAL → forces 5-minute Sapling tree rebuild on next boot. The tree has 1M Pedersen hash commitments that must be recomputed by mmap-scanning all blocks.

### Investigation
1. Find the Sapling tree save code — likely in `lib/sapling/src/incremental_merkle_tree.c` or `config/src/boot.c`
2. How is the tree persisted? SQLite table? Flat file?
3. When is it flushed? Only on clean shutdown? Or periodically?
4. Can we add a periodic WAL checkpoint after Sapling tree updates? Similar to the "WAL checkpoint after bulk catchup" pattern in `sync_controller.c:1909`

### Fix
If the tree is only saved on clean shutdown, add periodic persistence (every 1000 blocks or every 60 seconds). Use `sqlite3_wal_checkpoint_v2` if it's in SQLite.

---

## Task 2 (MEDIUM): Investigate Multi-threaded bg_validation Crash

### Problem
bg_validation crashes with >1 worker thread. Wave 22b fixed the file I/O (pread is safe), but the crash persists.

### Investigation (from wave 22b audit)
The likely cause is in `verify_scripts_parallel` — worker_ctx structs hold raw `const struct transaction *tx` pointers into `blk.vtx[]`, which is allocated on the `bg_validation_thread` stack. If multiple workers access the same transaction's script interpreter state simultaneously, that's the race.

1. Read `app/services/src/bg_validation_service.c` — find `verify_scripts_parallel`
2. Check if the script interpreter (`lib/script/src/interpreter.c`) has any global/static state
3. Check if `secp256k1_context` is shared across threads
4. Does each worker get its own secp256k1 context or share one?

Report findings. If the fix is clear (e.g., per-thread secp256k1 context), implement it. Otherwise document what you found.

---

## Task 3 (LOW): PHGR13 Sprout VK Format Investigation

### Problem
PHGR13 proof verification is wired but the verification key (VK) parsing fails. Code is in place but VK format needs investigation.

1. Find the PHGR13 VK loading code — likely in `lib/sapling/src/sprout.c`
2. What format does it expect? What format does the VK file actually have?
3. Is this blocking anything? PHGR13 is only for Sprout proofs at heights < 581876 — those are old and bg_validation skips them

If this is non-blocking (old Sprout proofs, no new blocks affected), document it as low-priority and move on.

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 23b task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `lib/validation/src/process_block.c` (Agent1/Agent2)
- `app/services/src/header_sync_service.c` (Agent1)
- `config/src/boot_index.c` scan functions (Agent2)
