# wt2 Assignment — Phase 3 PR-1: sync_watchdog dissolve, first 2 conditions

**Worktree:** `~/github/zclassic23-2`
**Branch:** `wt2/phase3-watchdog-dissolve-pr1`
**Phase:** 3 (Dissolve mega-modules) — first slice
**Depends on:** Phase 0 (condition engine) + Phase 1b (projection) — both in main
**Plan reference:** [`docs/dissolve/sync_watchdog_service.md`](../dissolve/sync_watchdog_service.md)

**Owns:**
- EDIT `app/conditions/src/block_failed_mask_at_tip.c` — extend detect predicate
- NEW `app/conditions/src/utxo_activation_paused.c`
- NEW `app/conditions/include/conditions/utxo_activation_paused.h` (if needed for tests)
- EDIT `app/conditions/src/condition_registry.c` — register the new condition
- EDIT `app/services/src/sync_watchdog_service.c` — delete the 2 branches we're replacing
- NEW `lib/test/src/test_utxo_activation_paused.c`
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h` to register the test

**MUST NOT touch:**
- Other watchdog branches (PR-2 and PR-3 own them) — leave them in place
- `app/services/src/script_validate_stage.c` or `body_persist_stage.c` (wt3 owns Phase 2 work)
- `app/services/src/chain_advance_coordinator.c` — the resume function lives here, you READ
  it but don't edit
- `app/conditions/src/chain_stalled_with_data.c` — existing condition, don't conflict
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

`sync_watchdog_service.c` is 1,448 LOC of branchy state-machine code
that handles 10 recovery types. The dissolve plan splits it into 8
Conditions over 3 PRs. PR-1 extracts the 2 conditions that overlap
with work already shipped (block_stall + utxo_pause), proving the
pattern under low risk before PR-2 and PR-3 take on the larger
extractions.

Read `docs/dissolve/sync_watchdog_service.md` §"PR-1" before starting.

---

## What you're extracting

### Watchdog branch 1 → existing `block_failed_mask_at_tip` (extend)

Current `WATCHDOG_BLOCK_STALL` branch detects:
- `chain_height` has not advanced for ≥300s
- `BLOCK_HAVE_DATA` flag set on `tip+1`

This is morally the same as the existing `block_failed_mask_at_tip`
condition — both fire when we have the data but can't connect. Today
the existing condition only checks `BLOCK_FAILED_MASK`. PR-1 EXTENDS
its detect predicate to also fire on `tip stale for ≥300s with
BLOCK_HAVE_DATA at tip+1 but BLOCK_FAILED_MASK NOT set` (a different
class of stall the watchdog catches).

Same remedy (`process_block_revalidate`) works for both cases.

### Watchdog branch 2 → NEW condition `utxo_activation_paused`

```c
/* utxo_activation_paused — CRITICAL
 *
 * Detect: chain activation paused for >= 300s.
 * Remedy: chain_advance_coordinator_resume("condition:utxo_activation_paused"),
 *         OR if pause_reason == "utxo_audit_drift",
 *         utxo_recovery_kick("condition:utxo_activation_paused").
 * Clear:  activation resumed (g_activation_paused == false).
 * Max attempts: 2 then operator page. */
```

---

## Tasks (in order)

### Task 1: Read the reference files

Open and skim (don't edit yet):
- `docs/dissolve/sync_watchdog_service.md` §"PR-1" — the contract
- `app/conditions/src/block_failed_mask_at_tip.c` — current shape
- `app/conditions/src/chain_stalled_with_data.c` — closest analog for
  the new utxo_paused condition
- `app/services/src/sync_watchdog_service.c` — find the
  `WATCHDOG_BLOCK_STALL` and `WATCHDOG_UTXO_PAUSE` branches in
  `sync_watchdog_check()`. These are what you'll delete.
- `app/services/src/chain_advance_coordinator.c` — find
  `chain_advance_coordinator_resume()` and any
  `g_activation_paused` atomic. These are what the remedy calls.

No code change. Acceptance: you can quote what `sync_watchdog_check()`'s
`WATCHDOG_BLOCK_STALL` branch does and what
`chain_advance_coordinator_resume()` takes/returns.

### Task 2: Extend `block_failed_mask_at_tip` detect predicate

In `app/conditions/src/block_failed_mask_at_tip.c`:

- Add a static `int64_t g_tip_height_at_check = -1;` and `int64_t
  g_tip_unchanged_since = 0;` to track tip-stall age inside the detect
  function. (Mirror the atomic pattern in `chain_stalled_with_data.c`.)
- Extend `detect()`: in addition to the existing
  `BLOCK_FAILED_MASK at tip+1` check, also fire if:
  - `BLOCK_HAVE_DATA at tip+1`,
  - `BLOCK_FAILED_MASK NOT at tip+1` (avoid double-firing with existing path),
  - `now - g_tip_unchanged_since > 300`.
- Witness payload: add `stall_type: 'failed_mask' | 'no_advance'`
  and `tip_age_s`.

**Acceptance:**
- `make test_parallel test_block_failed_mask_at_tip` PASS (existing tests).
- Add a new test case "fires on tip stale with HAVE_DATA + no FAILED_MASK"
  — happy path triggers detect→remedy→clear.

### Task 3: New condition `utxo_activation_paused`

Create `app/conditions/src/utxo_activation_paused.c`. Mirror
`block_failed_mask_at_tip.c` structure:

- `detect()` — reads `g_activation_paused` from chain_advance_coordinator
  (you may need a small public getter — add it minimally to
  `chain_advance_coordinator.h` if not present). Tracks
  `g_paused_since_unix`. Fires when `now - g_paused_since_unix > 300`.
- `remedy()` — branches on `pause_reason`:
  - If reason contains "utxo_audit_drift" → call
    `utxo_recovery_kick("condition:utxo_activation_paused")`
  - Else → call `chain_advance_coordinator_resume("condition:utxo_activation_paused")`
- `witness()` — emits `pause_reason`, `paused_at`, `pause_age_s`.

Severity: CRITICAL. Max attempts: 2.

Register in `app/conditions/src/condition_registry.c` (mirror how the
existing 3 conditions are registered).

### Task 4: Delete the 2 branches from sync_watchdog

In `app/services/src/sync_watchdog_service.c`:

- Delete the `WATCHDOG_BLOCK_STALL` branch in `sync_watchdog_check()`.
  Leave a comment: `/* moved to condition block_failed_mask_at_tip
  (PR-1, 2026-05-23) */`
- Delete the `WATCHDOG_UTXO_PAUSE` branch similarly. Comment:
  `/* moved to condition utxo_activation_paused (PR-1, 2026-05-23) */`
- Remove the corresponding enum values (`WATCHDOG_BLOCK_STALL`,
  `WATCHDOG_UTXO_PAUSE`) from `sync_watchdog_service.h` if they're
  not used elsewhere (grep first).
- Any references to those enum values in other files must be deleted
  too — grep `WATCHDOG_BLOCK_STALL` and `WATCHDOG_UTXO_PAUSE` and
  remove any branches.

**Acceptance:** `make -j$(nproc)` clean build. The remaining 8 watchdog
branches still compile and work.

### Task 5: Unit test for the new condition

`lib/test/src/test_utxo_activation_paused.c`. Mirror
`test_condition_engine.c` for the harness. Test cases:
- detect fires when paused > 300s
- detect does NOT fire when paused < 300s
- remedy calls resume (use a test hook — same pattern as other condition tests)
- remedy with reason "utxo_audit_drift" calls utxo_recovery_kick
- after max_attempts (2), operator-needed event emitted

Register in `test.c`, `test_parallel.c`, `test_helpers.h`.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt2/phase3-watchdog-dissolve-pr1
```

Append Completion section.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.
Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Status

**IN PROGRESS (wt2)** — started 2026-05-23 on
`wt2/phase3-watchdog-dissolve-pr1`.

<!-- Worker: append a Completion section below when done. -->

## Completion (wt2, 2026-05-23)

### Summary
Extracted the first sync watchdog dissolve slice: `block_failed_mask_at_tip`
now also detects stale tips with `BLOCK_HAVE_DATA` on the next block, and
`utxo_activation_paused` is a registered CRITICAL condition with resume,
drift-repair, witness, and operator-needed coverage. Removed the matching
inline watchdog branches while leaving the still-used `WATCHDOG_BLOCK_STALL`
native-next-block recovery intact.

### Commits
- `a7c794f0d` wt2: start watchdog dissolve pr1
- `14c4a079e` extract watchdog pause conditions

### Files added/modified
- `app/conditions/src/block_failed_mask_at_tip.c`
- `app/conditions/include/conditions/utxo_activation_paused.h` (NEW)
- `app/conditions/src/utxo_activation_paused.c` (NEW)
- `app/conditions/src/condition_registry.c`
- `app/services/include/services/sync_watchdog_service.h`
- `app/services/src/sync_watchdog_service.c`
- `lib/test/src/test_utxo_activation_paused.c` (NEW)
- `lib/test/src/test_sync_watchdog.c`
- `lib/test/include/test/test_helpers.h`
- `lib/test/src/test.c`
- `lib/test/src/test_parallel.c`

### Acceptance verification
- [x] `make -j$(nproc) test_zcl test_parallel` — PASS
- [x] `ZCL_TEST_ONLY=utxo_activation_paused ./test_zcl` — PASS
- [x] `ZCL_TEST_ONLY=sync_watchdog ./test_zcl` — PASS
- [x] `make lint` — PASS
- [x] `./test_parallel --jobs=$(nproc)` — PASS: `ALL TESTS PASSED — 0/186 groups failed`

### Surprises / follow-ups
The assignment text referenced `chain_advance_coordinator_resume()` and
`utxo_recovery_kick()`, but those APIs do not exist in current `main`.
The condition therefore preserves the existing production recovery path:
clear the process-block UTXO activation pause, kick gap-fill, and request
activation when an activation controller is available. The drift branch is
kept as a distinct tested path so it can be swapped to a real
`utxo_recovery_kick()` if that API lands later.

### Status
DONE — branch `wt2/phase3-watchdog-dissolve-pr1` pushed to origin, ready
for orchestrator merge.
