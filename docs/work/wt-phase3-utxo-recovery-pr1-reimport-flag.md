# Worker Assignment — Phase 3 utxo_recovery PR-1: extract utxo_reimport_flag primitive

**Worktree:** wt2 OR wt3 (either) — or isolated sub-agent worktree
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** none — pure extraction, no cutover gate
**Status: READY** — claim by marking IN PROGRESS.

**Owns:**
- NEW `lib/storage/include/storage/utxo_reimport_flag.h` — tiny primitive API
- NEW `lib/storage/src/utxo_reimport_flag.c` — implementation lifted from
  `utxo_recovery_service.c:200-225` (the `check` + `prepare` functions, plus
  the file path computation)
- EDIT `app/services/include/services/utxo_recovery_service.h` — DELETE the
  two function declarations; ADD `#include "storage/utxo_reimport_flag.h"`
- EDIT `app/services/src/utxo_recovery_service.c` — DELETE the two function
  bodies; ADD the include
- UPDATE callers of `utxo_recovery_check_reimport_flag` /
  `utxo_recovery_prepare_reimport` — rename their imports + call sites to
  the new primitive (find via `grep -rn` — about 3-5 callers)
- EDIT `Makefile` — list the new .c in `LIB_STORAGE_SRCS`
- NEW `lib/test/src/test_utxo_reimport_flag.c` — 4 unit cases
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h` — register

**MUST NOT touch:**
- Any other utxo_recovery function (PR-2/3/4 territory).
- `node_db` schema or the `leveldb_utxo_migrated` flag (that's a different
  primitive; PR-3 absorbs it into the repair job).
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.

---

## Why this matters

`utxo_recovery_service.c` is 1,200+ LOC across recovery, import, scan
fallback, and the reimport-flag primitive. The dissolve plan splits these
into a Condition + a Job + a primitive. PR-1 is the smallest slice: extract
the durable reimport flag as its own primitive in `lib/storage/`.

After this PR:
- The flag primitive lives in the right layer (it's a storage concern, not
  a service concern).
- `utxo_recovery_service.c` shrinks by ~30 LOC + one section.
- PR-2 (extract scan fallback to tx_index reader) has the next-smallest
  slice ready.

**Risk: very low.** The flag is a single file write/read with `fopen` /
`fread` / `remove`. No locks, no concurrency, no state machine.

---

## API (clean)

```c
/* lib/storage/include/storage/utxo_reimport_flag.h
 *
 * The "needs_reimport" durable flag — written by process_block.c when
 * UTXO validation fails so the next boot retries the import path.
 *
 * File location: $datadir/needs_reimport
 * File content : single byte '1' (or absent = false)
 *
 * `check_and_clear` atomically reads + removes the flag. The "atomic" is
 * not strict — between the `fopen` and the `remove`, a concurrent writer
 * could re-trigger; that's fine because the contract is "set once, clear
 * once at boot." Production has one writer (process_block.c) and one
 * reader (boot path), both single-threaded.
 */

#ifndef ZCL_STORAGE_UTXO_REIMPORT_FLAG_H
#define ZCL_STORAGE_UTXO_REIMPORT_FLAG_H

#include <stdbool.h>

/* Returns true if the flag was set; clears it as a side effect. */
bool utxo_reimport_flag_check_and_clear(const char *datadir);

/* Set the flag. Called by process_block.c when validation fails. */
bool utxo_reimport_flag_set(const char *datadir);

#endif
```

Note: the spec changes the function name from
`utxo_recovery_check_reimport_flag` → `utxo_reimport_flag_check_and_clear`.
The new name is more honest (it DOES clear the flag — that's not optional).
Old name was a misleading verb; this PR pays the small renaming cost.

---

## Tasks (in order)

### Task 1: New primitive

NEW `lib/storage/include/storage/utxo_reimport_flag.h` per the API above.

NEW `lib/storage/src/utxo_reimport_flag.c`:
- `utxo_reimport_flag_check_and_clear(datadir)` — body lifted from
  `utxo_recovery_service.c:200-219`. Drop the `printf` (the caller can log
  if it wants); add `LOG_INFO` style logging via `LOG_OK("reimport flag
  was set; cleared")` if it fired (defensive_coding compliance).
- `utxo_reimport_flag_set(datadir)` — NEW (the existing caller in
  `process_block.c` does this inline; consolidate). Computes the path the
  same way, writes a single '1' byte, fsyncs.

Use `zcl_malloc` if any allocation is needed (none currently). Use
`clock_now_wall_ms` only if the function needs a timestamp (it doesn't).

**Acceptance:** compiles standalone; no unresolved symbols when built in
isolation.

### Task 2: Update callers

EDIT `app/services/include/services/utxo_recovery_service.h` — remove the
two declarations under "Auto-reimport flag"; add
`#include "storage/utxo_reimport_flag.h"` so existing callers compile.

EDIT `app/services/src/utxo_recovery_service.c` — DELETE the two function
bodies (lines 200-229 today).

Find every caller of the old function names:
```bash
grep -rn 'utxo_recovery_check_reimport_flag\|utxo_recovery_prepare_reimport' app/ lib/ config/ tools/
```

For `_check_reimport_flag`: rename to `utxo_reimport_flag_check_and_clear`.

For `_prepare_reimport`: this stays in utxo_recovery (it's a wipe-prep
helper that ALSO needs node_db; not a flag-primitive concern). Leave its
body in `utxo_recovery_service.c`.

**Acceptance:** clean rebuild PASS; behavior unchanged.

### Task 3: process_block.c consolidation

Find the inline file-write in `process_block.c` (or wherever) that today
creates the `needs_reimport` file. Replace with a call to
`utxo_reimport_flag_set(datadir)`. If the inline write doesn't exist (and
the flag is set elsewhere), skip this task.

```bash
grep -rn 'needs_reimport' app/ lib/ config/
```

**Acceptance:** all writes go through the new primitive; no remaining
direct `fopen` on the `needs_reimport` file.

### Task 4: Unit tests

NEW `lib/test/src/test_utxo_reimport_flag.c` — 4 cases:
1. Flag absent → `check_and_clear` returns false, no side effect.
2. Flag set with '1' → returns true + removes the file.
3. Flag set with non-'1' → returns false but still removes the file
   (current behavior — preserve it).
4. `set` followed by `check_and_clear` round-trip returns true.

Use a temp directory created with `mkdtemp` in each test; clean up on
completion.

**Acceptance:** all 4 pass via `./test_parallel --jobs=$(nproc)`.

### Task 5: Wire test

EDIT `lib/test/include/test/test_helpers.h` — declare
`int test_utxo_reimport_flag(void);`

EDIT `lib/test/src/test.c` — add `failures += test_utxo_reimport_flag();`

EDIT `lib/test/src/test_parallel.c` — add `"utxo_reimport_flag"` to the
TEST_LIST array (alphabetical insertion).

**Acceptance:** `./test_parallel --jobs=$(nproc)` reports
`utxo_reimport_flag: 4 passed, 0 failed`.

### Task 6: Verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Acceptance

- All existing tests still pass.
- New primitive test passes (4 cases).
- `grep -rn 'utxo_recovery_check_reimport_flag'` returns 0 hits in
  production code (only the deleted line in the dissolve plan + this
  spec doc may remain).
- `make lint` PASS.

---

## What this does NOT do

- Does NOT touch the LDB import path, the scan fallback, or the recovery
  state machine (PR-2/3 territory).
- Does NOT change the on-disk format of the flag file.
- Does NOT add a Job, Service, or Condition.

---

## Commit cadence

One commit. The entire PR is a primitive extraction + rename + tests.

---

## Status

**READY.** Any worker can claim. Smallest Phase 3 PR available — extraction
of a ~30 LOC primitive into its own file with isolated unit tests.
