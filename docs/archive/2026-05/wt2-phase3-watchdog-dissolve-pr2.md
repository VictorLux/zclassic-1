# wt2 Assignment — Phase 3 PR-2: watchdog dissolve, 4 kick-only conditions

**Worktree:** `~/github/zclassic23-2`
**Branch:** `wt2/phase3-watchdog-dissolve-pr2`
**Phase:** 3 (Dissolve mega-modules) — second slice
**Depends on:** Phase 3 PR-1 (wt2-phase3-watchdog-dissolve-pr1, must be merged first)
**Plan reference:** [`docs/dissolve/sync_watchdog_service.md`](../dissolve/sync_watchdog_service.md)

**Owns:**
- NEW `app/conditions/src/header_stall_at_height.c`
- NEW `app/conditions/src/sync_state_stuck.c`
- NEW `app/conditions/src/download_queue_starved.c`
- NEW `app/conditions/src/local_header_refill_needed.c`
- EDIT `app/conditions/src/condition_registry.c` — register the 4 new conditions
- EDIT `app/services/src/sync_watchdog_service.c` — delete the 4 corresponding branches
- NEW `lib/test/src/test_watchdog_conditions_pr2.c` — one file covering all 4
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`

**MUST NOT touch:**
- `app/conditions/src/block_failed_mask_at_tip.c` (PR-1 owns)
- `app/conditions/src/utxo_activation_paused.c` (PR-1 owns)
- `app/conditions/src/chain_stalled_with_data.c`, `contradiction_frozen.c` (Phase 0)
- The remaining 2 watchdog branches: `WATCHDOG_PEER_FLOOR` and
  `WATCHDOG_SYNC_VIOLATION` — PR-3 owns those
- Anything outside `app/conditions/` and `app/services/sync_watchdog_service.c`
- `app/services/src/script_validate_stage.c` (wt3 owns Phase 2)
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

PR-2 extracts the 4 "kick-only" conditions — remedies that just poke
the right subsystem without destructive action. Safe to land together
since none of the remedies cascade. After this PR, `sync_watchdog_service.c`
is down to ~500 LOC (just peer-related logic for PR-3).

Read `docs/dissolve/sync_watchdog_service.md` §"PR-2" before starting.

---

## The 4 conditions

### 1. `header_stall_at_height` (CRITICAL)

- **detect** — `best_header_height` has not advanced for ≥300s while
  `peer_max_height > best_header_height` (peers know more than us).
  Witness: `{stuck_at_height, age_s, peer_max_height, gap}`.
- **remedy** — call `header_probe_kick("condition:header_stall")` (the
  existing fn inside header_probe_service — grep to find exact signature).
- **clear** — header height advances OR peer floor drops below 3
  (in which case `peer_floor_violated` will own recovery — PR-3).
- **max_attempts**: 3 over 15 minutes, then operator page.

### 2. `sync_state_stuck` (WARN)

- **detect** — any non-tip sync state unchanged for ≥600s AND no
  more-specific condition is currently active (check via the engine's
  active list — see `condition_engine_count_active()` or add one if
  needed; if adding, keep it small).
- **remedy** — call `sync_state_kick(state, reason)` — poke the current
  state's handler. If no such fn exists, create a thin wrapper that
  calls the appropriate per-state advance fn (request more headers if
  in HEADERS, kick body downloads if in BODIES, etc.). Mirror the
  switch in `sync_watchdog_check()`.
- **clear** — state changes.
- **max_attempts**: 3 over 30 minutes, then operator page.

### 3. `download_queue_starved` (WARN)

- **detect** — download in-flight slots < 10% capacity for ≥120s while
  sync state is BODIES. Read in-flight count via existing download_manager
  API (grep `download_manager_inflight_count` or similar).
- **remedy** — call `download_manager_refill_queue()`. Cheap, idempotent.
- **clear** — in-flight ≥ 50%.
- **max_attempts**: unbounded (refill is safe to retry).

### 4. `local_header_refill_needed` (WARN)

- **detect** — `active_chain_tip + 1` is absent in block index AND
  the corresponding header is not staged anywhere AND peer count > 0.
- **remedy** — call `header_probe_kick_for_height(active_chain_tip + 1)`
  with explicit peer rotation (re-use the local_recovery_reset path
  from sync_watchdog_service.c — extract it to a small helper if
  needed; if extracting, the helper goes in
  `app/services/include/services/header_probe_service.h` not in
  conditions/).
- **clear** — missing header appears in block index.
- **max_attempts**: 3 with peer rotation, then operator page with
  witness `{missing_height, attempted_peers, distinct_peers_tried}`.

---

## Tasks (in order)

### Task 1: Read reference files

- `docs/dissolve/sync_watchdog_service.md` §"PR-2"
- `app/conditions/src/block_failed_mask_at_tip.c` (PR-1 extended) — pattern
- `app/conditions/src/utxo_activation_paused.c` (PR-1 new) — pattern
- `app/services/src/sync_watchdog_service.c` — find these 4 branches:
  `WATCHDOG_HEADER_STALL`, `WATCHDOG_HEADER_LAG`, `WATCHDOG_STATE_STUCK`,
  `WATCHDOG_QUEUE_STARVED`, `WATCHDOG_LOCAL_HEADER_REFILL`. (HEADER_STALL
  and HEADER_LAG collapse into one Condition per the plan.)
- `app/services/src/header_probe_service.c` — find `header_probe_kick` and
  any height-specific variant.

Acceptance: you can quote what each of the 5 watchdog branches does.

### Task 2: Implement `header_stall_at_height`

Create `app/conditions/src/header_stall_at_height.c`. Pattern from
`block_failed_mask_at_tip.c`. Wire detect/remedy/clear/witness.

Register in `condition_registry.c`.

### Task 3: Implement `sync_state_stuck`

Same pattern. The remedy is the most complex of the 4 because it
switches on current sync state. Keep the switch small — delegate to
existing per-state advance fns. If no such fns exist, create a single
`sync_state_kick(state, reason)` helper in
`lib/sync/include/sync/sync_state.h` and call it.

### Task 4: Implement `download_queue_starved`

Trivial — single-call remedy. Mirror the structure of `header_stall`.

### Task 5: Implement `local_header_refill_needed`

Trickiest — needs peer rotation across attempts. Use the engine's
`attempt_count` to drive rotation (every nth attempt → different peer).
If the local_recovery_reset logic in sync_watchdog_service.c is
non-trivial, extract it to a helper in header_probe_service.h first.

### Task 6: Delete 5 branches from sync_watchdog

In `app/services/src/sync_watchdog_service.c`:
- Delete `WATCHDOG_HEADER_STALL` + `WATCHDOG_HEADER_LAG` branches.
  Comment: `/* moved to condition header_stall_at_height (PR-2) */`
- Delete `WATCHDOG_STATE_STUCK`. Comment:
  `/* moved to condition sync_state_stuck (PR-2) */`
- Delete `WATCHDOG_QUEUE_STARVED`. Comment:
  `/* moved to condition download_queue_starved (PR-2) */`
- Delete `WATCHDOG_LOCAL_HEADER_REFILL`. Comment:
  `/* moved to condition local_header_refill_needed (PR-2) */`
- Remove unused enum values from the header.
- Grep for removed enum references and clean them up.

After this PR, `sync_watchdog_check()` should only handle
`WATCHDOG_PEER_FLOOR` and `WATCHDOG_SYNC_VIOLATION` (PR-3 will delete those).

**Acceptance:** `make -j$(nproc)` clean; existing tests PASS.

### Task 7: Unit tests

`test_watchdog_conditions_pr2.c` — one test file covering all 4. For
each condition:
- detect fires under the right predicate
- detect does NOT fire when the predicate is false
- remedy is called and produces the right effect
- after max_attempts, operator-needed event emitted (for the 2 that
  page)

Register in `test.c`, `test_parallel.c`, `test_helpers.h`.

### Task 8: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt2/phase3-watchdog-dissolve-pr2
```

Append Completion section.

---

## Commit cadence

One commit per task (8 total). Push after tasks 2, 4, 6, 7.

---

## Status

**✅ DONE — pushed 2026-05-24** to main as commit `b40b555b3`.

<!-- Worker: append a Completion section below when done. -->

## Completion (wt2, 2026-05-24)

### Summary
Shipped watchdog dissolve PR-2 on top of current `main`: four
kick-only watchdog paths now live as Conditions
(`header_stall_at_height`, `sync_state_stuck`,
`download_queue_starved`, `local_header_refill_needed`), while
`sync_watchdog_service` keeps only the PR-3-owned peer-floor and
sync-violation paths.

### Commits
- `09fd89ba7` wt2: extract watchdog dissolve pr2 conditions
- `b40b555b3` stabilize tests after watchdog pr2 replay

### Files added/modified
- `app/conditions/include/conditions/watchdog_dissolve_pr2.h` (NEW)
- `app/conditions/src/header_stall_at_height.c` (NEW)
- `app/conditions/src/sync_state_stuck.c` (NEW)
- `app/conditions/src/download_queue_starved.c` (NEW)
- `app/conditions/src/local_header_refill_needed.c` (NEW)
- `app/conditions/src/condition_registry.c`
- `app/services/include/services/sync_watchdog_service.h`
- `app/services/src/sync_watchdog_service.c`
- `lib/test/src/test_watchdog_dissolve_pr2.c` (NEW)
- `lib/test/src/test_sync_watchdog.c`
- `lib/test/src/test_chain_advance_coordinator.c`
- `lib/test/src/test_event.c`
- `lib/test/src/test_projection_adoption.c`
- `lib/test/src/test_zclassicd_oracle.c`
- `lib/test/include/test/test_helpers.h`
- `lib/test/src/test.c`
- `lib/test/src/test_parallel.c`

### Acceptance verification
- [x] `make -j$(nproc)` — PASS
- [x] `make lint` — PASS
- [x] `ZCL_TEST_ONLY=watchdog_dissolve_pr2 ./test_zcl` — PASS
- [x] `ZCL_TEST_ONLY=sync_watchdog ./test_zcl` — PASS
- [x] `./test_parallel --jobs=$(nproc)` — PASS: `ALL TESTS PASSED — 0/187 groups failed (108.0s wall, 32 workers)`

### Surprises / follow-ups
The replay started from the older feature-branch workflow and had to be
cherry-picked onto current `main`. A pre-existing dirty test patch in
the wt2 worktree was preserved as a separate stabilizer commit; it fixes
condition cleanup between PR-2 cases plus unrelated test flake surfaces
around async event stop, projection WAL contention, and oracle evidence
setup.
