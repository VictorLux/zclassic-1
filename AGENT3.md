# AGENT3 — Wave 22: LOG_FAIL Spam Fix + Before-Save Hooks + Logging Cleanup

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

The `make test` output is broken — 1M lines of LOG_FAIL spam from the fast_sync PoW solve loop mask real test failures. Additionally, 3 models are missing before_save hooks, and ~10 service error paths use bare `fprintf` instead of the structured `LOG_ERR` macro.

Read `DEFENSIVE_CODING.md` first.

---

## Task 1 (HIGH — DO FIRST): Fix LOG_FAIL Spam in fast_sync_verify_pow

### File: `lib/net/src/fast_sync.c`

**Bug:** `fast_sync_verify_pow()` (line ~392) calls `LOG_FAIL()` on every failed nonce attempt. The solve loop in `fast_sync_solve_pow()` tries up to 2^20 nonces, producing ~1M stderr lines. This masks the real test failure in `make test`.

**Fix:** The verify function should NOT log on normal verification failure — it's called in a tight loop. Remove or downgrade the LOG_FAIL to a simple `return false`:

```c
// In fast_sync_verify_pow():
// BEFORE:
LOG_FAIL("sync", "verify_pow: leading zero check failed at byte %d", i);

// AFTER: just return false — this is expected during solve loop
return false;
```

Keep LOG_FAIL only for truly unexpected errors (NULL pointer, etc.), not for the normal "this nonce didn't work" path.

**After this fix:** Re-run `make test` and identify which test suite is actually contributing the 1 failure.

---

## Task 2 (MEDIUM): Wire before_save Hooks on 3 Missing Models

These models go through `AR_BEGIN_SAVE` lifecycle but have no `before_save` guard registered:

### 2a. `app/models/src/mempool_entry.c`
Add `mempool_before_save`:
- Validate txid is 64 hex chars (not blank)
- Validate fee >= 0
- Register with `ar_register_before_save`

### 2b. `app/models/src/tx_index.c`
Add `tx_index_before_save`:
- Validate txid non-null
- Validate height >= 0
- Validate file_number >= 0
- Register with `ar_register_before_save`

### 2c. Look at `wallet_tx` save path
Check if `wallet_tx` actually needs a before_save hook or if its validation is handled by `db_wallet_tx_validate`. If validate covers it, document why no hook is needed. If not, add one.

Follow the pattern in `utxo.c` — look at how `utxo_before_save` is structured and registered.

---

## Task 3 (LOW): Migrate fprintf(stderr,...) to LOG_ERR in Services

Find service files that use bare `fprintf(stderr,...)` for error reporting instead of structured `LOG_ERR`. Convert them.

### Priority targets (from audit):
1. `app/services/src/bg_validation_service.c:504` — `!ndb->open` check with no log
2. `lib/validation/src/connect_block.c` — `realloc` failure path (Agent2 may get here first with allocator fix — coordinate)
3. `app/services/src/chain_state_repository.c:47,100` — SQLite query helpers
4. `app/services/src/snapshot_sync_service.c:880` — preceded by fprintf
5. `app/services/src/utxo_recovery_service.c:876` — preceded by fprintf

### Pattern:
```c
// BEFORE:
fprintf(stderr, "UTXO import: failed to restore normal mode\n");
return -1;

// AFTER:
LOG_ERR("sync", "UTXO import: failed to restore normal mode");
return -1;
```

Make sure `#include "util/log_macros.h"` is present in each file.

---

## Task 4: Find the Real Test Failure

After Task 1 removes the LOG_FAIL spam, re-run `make test`. The output should now be clean enough to see which of the 95 test suites is returning 1. Find it and fix it (or report back what it is).

Look at `lib/test/src/test.c` — it sums return values from all `test_*()` calls. One of them is returning 1.

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
- `lib/storage/src/disk_block_io.c` (Agent2)
- `app/services/src/block_pruning_service.c` (Agent2)
- `config/src/boot_index.c` (Agent2)
- `lib/validation/src/process_block.c` (Agent2)
- `lib/validation/src/connect_block.c` (Agent2 — allocator fix)
- `lib/validation/src/update_coins.c` (Agent2)
