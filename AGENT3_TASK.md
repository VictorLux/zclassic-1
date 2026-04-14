# Agent 3 Task: Wave 16 — Robustness Hardening

## Status
- All tests pass (0 failures!)
- make lint wired
- Node is syncing (slowly)

## Tasks

### 1. Suppress remaining log spam

The bg_validation undo warning still floods the log:
```
[bg_validation] read_block_undo: undo pos is 0 for file N
```
Fix `bg_validation_service.c:192` to NOT log when undo pos is 0 — this is normal for blocks without undo data. Either remove the LOG_FAIL or change it to only log once per file.

### 2. Migrate bare malloc in remaining files

8 files still have bare `malloc` instead of `zcl_malloc`. Fix these:
```
app/services/src/snapshot_sync_service.c
app/services/src/block_index_integrity.c
app/controllers/src/explorer_controller.c
lib/validation/src/process_block.c
lib/storage/src/disk_block_io.c
config/src/boot.c
config/src/boot_index.c
config/src/boot_services.c
```
Replace `malloc(` with `zcl_malloc(size, "label")`, `calloc(` with `zcl_calloc(n, size, "label")`, `realloc(` with `zcl_realloc(ptr, size, "label")`.

### 3. Migrate bare return -1 in critical paths

Target the most important files:
- `lib/validation/src/process_block.c` — replace bare `return false` with `LOG_FAIL()`
- `config/src/boot.c` — boot failures must be visible
- Count before and after

### 4. Add Sapling tree persistence to avoid 5-minute rebuild

From CHECKLIST.md: "Sapling tree persistence after crash — SIGKILL loses WAL, forces 5-min rebuild on next boot"

The Sapling tree rebuild takes ~5 minutes on every restart. Investigate:
- Where is the Sapling tree stored? (likely in SQLite node.db)
- Why does it need rebuilding every time? (WAL not checkpointed?)
- Add a WAL checkpoint after Sapling tree operations complete
- Or save the tree root hash + commitment count so the rebuild can be skipped when they match

Look at `config/src/boot.c` around the Sapling tree load section and `app/controllers/src/sync_controller.c` for `sapling_tree_rebuild`.

### 5. Run make test + make lint — verify clean

Both must pass with 0 failures/violations.

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` — must stay at 0 failures
- Commit with descriptive messages
- Do NOT touch sync/header/activation code
