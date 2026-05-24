# Worker Assignment — Phase 3 chain_restore PR-1: extract restore_planner

**Worktree:** wt2 OR wt3 (either) — or isolated sub-agent worktree
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** none — this is a pure extraction with NO behavior change.
The dissolve plan (docs/dissolve/chain_restore_service.md) lists later PRs
as gated on C-9, but PR-1 has no such gate; the planning function is
already side-effect free.
**Status: READY** — claim by marking IN PROGRESS.

**Owns:**
- NEW `app/services/include/services/chain_restore_planner.h` — pure planning API
- NEW `app/services/src/chain_restore_planner.c` — implementation moved from
  `chain_restore_service.c` (`chain_restore_plan` + `chain_restore_record_plan_result`)
- EDIT `app/services/include/services/chain_restore_service.h` — forward the
  planning struct + functions to the new header (one-line include); keep the
  rest of the service unchanged
- EDIT `app/services/src/chain_restore_service.c` — DELETE the moved
  function bodies; replace with `#include "services/chain_restore_planner.h"`
- EDIT `Makefile` — list the new .c in `LIB_SERVICES_SRCS`
- NEW `lib/test/src/test_chain_restore_planner.c` — 5 unit cases for the
  pure planner (it's already pure, so testing in isolation is easy)
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h` — register the test

**MUST NOT touch:**
- The execution side of `chain_restore_service.c` (PR-2 territory).
- `chain_restore_record_plan_result`'s evidence-recording side effect (move
  it as-is — refactoring it out is PR-2 work).
- Any caller of `chain_restore_plan` outside `chain_restore_service.c` — the
  function signature stays identical.
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.

---

## Why this matters

`chain_restore_service.c` is 1,674 LOC across three distinct concerns
(planning, execution, integrity check). The dissolve plan (4 PRs) splits
them into one Service + two Jobs + one Condition. PR-1 is the cheapest
slice: extract the pure planner.

After this PR ships:
- The planning function lives in `chain_restore_planner.c` and can be
  unit-tested in isolation without spinning up the full chain_restore
  state machine.
- PR-2 (extract reorg_disconnect + reorg_connect Jobs) has a clean
  starting line — it now only deals with execution, not planning.
- The framework Service shape ([`docs/FRAMEWORK.md`](../FRAMEWORK.md) § 3.2)
  is one extraction closer to canonical.

**This is low-risk because the planner is already pure** — read the
function body in `chain_restore_service.c:30-87`. No globals, no I/O, no
state changes except writing the output struct. Moving the function to a
different `.c` is a mechanical edit.

---

## API (unchanged from today)

```c
/* app/services/include/services/chain_restore_planner.h */

struct chain_restore_plan;  /* defined in chain_restore_service.h */
struct chain_restore_input; /* defined in chain_restore_service.h */

void chain_restore_plan(struct chain_restore_plan *out,
                        const struct chain_restore_input *in);

void chain_restore_record_plan_result(const struct chain_restore_plan *p);
```

The two struct types stay in `chain_restore_service.h` (which the planner
header includes) so the existing 30+ callers don't need to change their
includes.

---

## Tasks (in order)

### Task 1: Create the new files

NEW `app/services/include/services/chain_restore_planner.h` — see API above.

NEW `app/services/src/chain_restore_planner.c` — MOVE (cut + paste) the
following from `chain_restore_service.c`:
- `void chain_restore_plan(...)` body
- `void chain_restore_record_plan_result(...)` body
- Any local statics they reference (probably `g_last_plan_result` — move it)
- Required includes (uint256, evidence, log_macros)

LEAVE in `chain_restore_service.c`:
- All execute / commit / disconnect / connect / anchor logic
- `chain_restore_state_name` (static helper) — stays unless the planner needs it
- `#include "services/chain_restore_planner.h"` so the rest of the file can
  still call the planner

**Acceptance:** `make -j$(nproc)` clean; no behavior changes; binary size
within ±0.1% of pre-PR.

### Task 2: Wire Makefile

EDIT `Makefile` — find the existing `LIB_SERVICES_SRCS` (or equivalent
variable) listing `chain_restore_service.c`; add
`app/services/src/chain_restore_planner.c` to it.

**Acceptance:** clean rebuild from `make clean && make -j$(nproc)` PASS.

### Task 3: Unit tests

NEW `lib/test/src/test_chain_restore_planner.c` — 5 cases that exercise the
4 paths in `chain_restore_plan`:

1. Null coins_best_hash → next_state == CHAIN_RESTORE_FAILED, should_skip_activate
2. Hash found in map + height > 0 → CHAIN_RESTORE_FOUND_IN_INDEX, anchor_height set
3. Hash NOT in map but utxo_max_height > 0 → CHAIN_RESTORE_ANCHOR_CREATED, anchor_height set
4. Hash NOT in map and utxo_max_height == 0 → CHAIN_RESTORE_FAILED, "awaiting P2P"
5. The `source` field correctly influences the reason string in path B

Each test constructs a `chain_restore_input` literal and asserts on the
output `chain_restore_plan`. No mocks, no globals (the planner has no
external dependencies other than the static `g_last_plan_result`, which
is fine — it's a memory write).

**Acceptance:** all 5 pass via `./test_parallel --jobs=$(nproc)`.

### Task 4: Register the test

EDIT `lib/test/include/test/test_helpers.h` — declare
`int test_chain_restore_planner(void);`

EDIT `lib/test/src/test.c` — add `failures += test_chain_restore_planner();`

EDIT `lib/test/src/test_parallel.c` — add `"chain_restore_planner"` to the
TEST_LIST array (alphabetical insertion).

**Acceptance:** `./test_parallel --jobs=$(nproc)` reports
`chain_restore_planner: 5 passed, 0 failed`.

### Task 5: Verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section to this file.

---

## Acceptance

- All existing chain_restore tests still pass.
- New planner test passes (5 cases).
- Binary size unchanged (within rounding).
- `make lint` PASS.

---

## What this does NOT do

- Does NOT change behavior.
- Does NOT touch the execute / disconnect / connect paths (those are PR-2).
- Does NOT delete anything from `chain_restore_service.c` except the moved
  function bodies.
- Does NOT add a Job, Service, or Condition (those are PR-2/3/4).

---

## Commit cadence

One commit. The entire PR is a mechanical move + tests, which is small
enough to be a single coherent commit.

---

## Status

**READY.** Any worker can claim. This is the cheapest Phase 3 PR available
right now — no cutover dependencies, no API changes, pure mechanical
refactor with isolation tests as the proof of correctness.
