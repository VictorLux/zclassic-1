# AGENT2 — Wave 22: Thread Safety & Allocator Hardening

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Audit found 3 thread safety bugs and 5 files using raw allocators. These are the root causes of the known SIGSEGVs (bg_hash_verify, address backfill, multi-threaded bg_validation). Fix them.

Read `DEFENSIVE_CODING.md` first.

---

## Task 1 (CRITICAL): Fix block_pruning_service.c Lock Bug

### File: `app/services/src/block_pruning_service.c`

**Bug:** Lines 160-165 — lock acquired and immediately released BEFORE `unlink()`. A concurrent reader can enter `open_disk_file` with `g_cached_file` pointing to the file being deleted → SIGSEGV.

**Fix:**
1. Move `unlink()` INSIDE the lock
2. Call `disk_block_io_close_cache()` under the lock to invalidate `g_cached_file` before deleting
3. Then `unlink()` while still holding the lock
4. Release lock after unlink

```c
disk_block_io_lock();
disk_block_io_close_cache();  // invalidate cached handle
bool blk_ok = (unlink(blk_path) == 0 || errno == ENOENT);
disk_block_io_unlock();
```

---

## Task 2 (CRITICAL): Protect boot_index.c Scans from P2P Races

### File: `config/src/boot_index.c`

**Bug:** `scan_one_block_file()` (line 497) and `resolve_orphan_pprev_from_disk()` (line 680) open private FILE* handles on blk*.dat without any mutex. Called from P2P thread (`msg_headers.c:412`) and RPC thread (`repair_controller.c:644`) while `write_block_to_disk` may be writing to the same files.

**Fix:** Wrap both functions' file I/O with `disk_block_io_lock()` / `disk_block_io_unlock()`:

```c
// In scan_one_block_file:
disk_block_io_lock();
FILE *f = fopen(filepath, "rb");
// ... all fread calls ...
fclose(f);
disk_block_io_unlock();
```

Same for `resolve_orphan_pprev_from_disk`.

**Note:** These functions do their own `fopen`/`fclose` (don't use `g_cached_file`), but the lock prevents concurrent writes from corrupting the file mid-read.

---

## Task 3 (MEDIUM): Replace Raw Allocators in lib/validation/

### Files:
- `lib/validation/src/connect_block.c:592` — `realloc` → `zcl_realloc`
- `lib/validation/src/update_coins.c:51` — `realloc` → `zcl_realloc`
- `lib/validation/src/process_block.c:822,1100` — `malloc(4096)` → `zcl_malloc(4096, "process_block")`
- `lib/net/src/msg_headers.c:271` — `malloc` → `zcl_malloc`

For each:
1. Replace raw call with `zcl_*` equivalent
2. Replace bare `fprintf(stderr,...)` on failure with `LOG_ERR` or `LOG_NULL`
3. Include `util/safe_alloc.h` if not already included

---

## Task 4 (MEDIUM): Audit All fread/fwrite Paths for Lock Coverage

Search the entire codebase for `fread` and `fwrite` calls on block/undo files. For each, verify the call is inside a `disk_block_io_lock()` region. Document any unprotected paths.

```bash
grep -rn 'fread\|fwrite' lib/storage/src/ lib/validation/src/ app/services/src/ config/src/ --include='*.c' | grep -v test | grep -v vendor
```

Add `// disk-io-lock: held` comments to protected paths and `// disk-io-lock: private-fd` to paths using their own file descriptors (pread-based, safe).

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
- `lib/net/src/fast_sync.c` (Agent3)
- `app/models/src/mempool_entry.c` (Agent3)
- `app/models/src/tx_index.c` (Agent3)
- `app/models/src/wallet_tx.c` (Agent3)
- Any file in `app/services/src/` that Agent3 is modifying for fprintf→LOG_ERR
