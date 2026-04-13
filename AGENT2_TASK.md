# Agent 2 Task: Wave 13 — Thread Safety & Crash Fixes

## Previous work (DONE)
Wave 12: Header sync stall detection — per-peer tracking, stall recovery, inbound fallback. All merged.

## Problem

Two disabled features crash under concurrent access (CHECKLIST.md items):
1. **SIGSEGV in bg_hash_verify fread** — crashes at h=20000 when P2P is running. `g_cached_file` mutex doesn't cover all fread paths.
2. **Multi-threaded bg_validation** — crashes with >1 worker (same file I/O thread safety issue).

The root cause is that `disk_block_io.c` uses a shared file handle cache (`g_cached_file`) that isn't protected across all access paths. Multiple threads (bg_validation, P2P block processing, catchup) call `read_block_from_disk()` concurrently.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md` — mandatory coding rules
- `CHECKLIST.md` — items 2.10, remaining items at bottom
- `lib/storage/src/disk_block_io.c` — file handle cache, the crash site
- `app/services/src/bg_validation_service.c` — background validation workers
- `app/services/src/bg_hash_verification_service.c` — hash verify (DISABLED due to crash)
- `lib/net/src/msgprocessor.c` — P2P block reads in `process_block_msg()`

## Tasks

### 1. Migrate disk_block_io.c to pread()

Replace all `fopen/fseek/fread/fclose` in `disk_block_io.c` with `open/pread/close`. `pread()` is thread-safe — multiple threads can read the same file descriptor at different offsets without races.

- Replace the file handle cache (`g_cached_file`, `g_cached_file_num`) with a simple `open()` per read or a thread-local fd cache
- Remove the disk I/O mutex (`g_disk_io_mutex`) if pread makes it unnecessary
- Ensure all callers still work: `read_block_from_disk()`, `read_raw_block()`, `read_undo_data()`

### 2. Re-enable bg_hash_verification_service

In `config/src/boot_services.c`, the bg_hash_verify service is disabled. After fixing the thread safety:
- Re-enable it
- Run with P2P active to verify no crash at h=20000+
- If it still crashes, add ASAN build and investigate

### 3. Enable multi-threaded bg_validation

In `bg_validation_service.c`, change the worker count from 1 to `nproc/2` (capped at 4). Test under load.

### 4. Migrate bare malloc in disk_block_io.c

`disk_block_io.c` has bare `malloc` calls. Replace with `zcl_malloc()` per DEFENSIVE_CODING.md.

### 5. Tests

- Add thread safety test: spawn 4 threads reading different blocks concurrently, verify no crash
- Add test for pread correctness: read known block, verify hash matches
- Put tests in `lib/test/src/test_disk_block_io.c`
- Register in `lib/test/src/test.c`

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing — zero new failures
- Commit with descriptive messages
- Do NOT touch files unrelated to this task
