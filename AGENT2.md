# AGENT2 — Wave 22b: Thread Safety Fixes

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Agent3 completed their wave 22 tasks (LOG_FAIL fix, before_save hooks, fprintf→LOG_ERR). Tests pass.

Two **critical thread safety bugs** remain — these are the root causes of the known SIGSEGV crashes. Fix them both.

Read `DEFENSIVE_CODING.md` first.

---

## Task 1 (CRITICAL): Fix block_pruning_service.c Lock Bug

### File: `app/services/src/block_pruning_service.c`

**Bug at lines 160-165:** Lock is acquired and immediately released BEFORE `unlink()`. A concurrent reader can enter `open_disk_file` with `g_cached_file` pointing to the file being deleted.

Current code:
```c
disk_block_io_lock();
/* Close and reopen to flush — we just need the lock to
 * prevent concurrent reads while we delete. */
disk_block_io_unlock();

bool blk_ok = (unlink(blk_path) == 0 || errno == ENOENT);  // RACE HERE
bool rev_ok = (unlink(rev_path) == 0 || errno == ENOENT);
```

**Fix:** Hold the lock across the unlink, and invalidate the cache first:
```c
disk_block_io_lock();
disk_block_io_close_cache();  // invalidate g_cached_file if it points to this file
bool blk_ok = (unlink(blk_path) == 0 || errno == ENOENT);
bool rev_ok = (unlink(rev_path) == 0 || errno == ENOENT);
disk_block_io_unlock();
```

Also replace the `fprintf(stderr, ...)` on line 169-170 with `LOG_ERR("prune", ...)`.

---

## Task 2 (CRITICAL): Protect boot_index.c Scans from P2P Races

### File: `config/src/boot_index.c`

**Bug:** `scan_one_block_file()` (line 491) opens its own `FILE *f = fopen(filepath, "rb")` and does `fread` calls without holding `g_file_cache_mutex`. This function is called from:
- Boot (single-threaded — safe)
- P2P thread via `msg_headers.c:412` — **RACE** with `write_block_to_disk` on the main thread

Same issue in `resolve_orphan_pprev_from_disk()` (line ~680).

**Fix:** Wrap the file I/O sections with the disk lock:

For `scan_one_block_file`:
```c
disk_block_io_lock();
FILE *f = fopen(filepath, "rb");
// ... all the fread/fseek work ...
fclose(f);
disk_block_io_unlock();
```

For `resolve_orphan_pprev_from_disk`:
```c
disk_block_io_lock();
FILE *f = fopen(path, "rb");
// ... fread ...
fclose(f);
disk_block_io_unlock();
```

You need to `#include "storage/disk_block_io.h"` in boot_index.c if not already present.

**Note:** The lock prevents concurrent writes — the private FILE* doesn't touch `g_cached_file` but the lock ensures `write_block_to_disk` can't modify the file mid-read.

---

## Task 3 (MEDIUM): Audit fread/fwrite Paths and Document Lock Coverage

Search the whole codebase for `fread`/`fwrite` calls on block/undo files:

```bash
grep -rn 'fread\|fwrite' lib/storage/src/ lib/validation/src/ app/services/src/ config/src/ app/controllers/src/ --include='*.c' | grep -v test | grep -v vendor
```

For each site:
- If inside `disk_block_io_lock()` region: add comment `// disk-io-lock: held`
- If using own fd via `open()`+`pread()`: add comment `// disk-io-lock: private-fd (pread)`
- If unprotected: fix it or flag it

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 22 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `lib/net/src/fast_sync.c` (Agent3 — done)
- `app/models/src/mempool_entry.c` (Agent3 — done)
- `app/models/src/tx_index.c` (Agent3 — done)
- `app/models/src/wallet_tx.c` (Agent3 — done)
