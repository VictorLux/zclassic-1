# wt2 Assignment — Phase 3 PR-3: watchdog dissolve, FINAL — DELETE the module

**Worktree:** `~/github/zclassic23-2`
**Branch:** `wt2/phase3-watchdog-dissolve-pr3`
**Phase:** 3 (Dissolve mega-modules) — final slice
**Depends on:** PR-1 + PR-2 merged
**Plan reference:** [`docs/dissolve/sync_watchdog_service.md`](../dissolve/sync_watchdog_service.md)

**Owns:**
- NEW `app/conditions/src/peer_floor_violated.c`
- NEW `app/conditions/src/sync_violation_lag.c`
- EDIT `app/conditions/src/condition_registry.c`
- **DELETE** `app/services/src/sync_watchdog_service.c` (1,448 LOC → gone)
- **DELETE** `app/services/include/services/sync_watchdog_service.h`
- EDIT all call sites that included `<services/sync_watchdog_service.h>`
  — grep and replace with calls to the supervisor + condition engine
- EDIT `tools/mcp/controllers/` — migrate any `zcl_state subsystem=watchdog`
  fields into the supervisor's dump_state or the condition engine's dump_state
- NEW `lib/test/src/test_watchdog_conditions_pr3.c` — last 2 conditions
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`

**MUST NOT touch:**
- The 6 conditions already extracted by PR-1 + PR-2
- `app/services/src/script_validate_stage.c`, `proof_validate_stage.c`,
  `body_persist_stage.c` (wt3 owns Phase 2)
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

This is the final dissolve PR for `sync_watchdog_service.c`. After this
lands, the 1,448-LOC mega-module is **deleted entirely**. The 10
recovery-type branches become 8 supervised Conditions, each with
explicit max_attempts and operator paging.

The two remaining branches handle DESTRUCTIVE remedies (disconnecting
peers), so this PR lands LAST after we've proven the engine works on
the safer extractions.

Read `docs/dissolve/sync_watchdog_service.md` §"PR-3" before starting.

---

## The 2 conditions

### 1. `peer_floor_violated` (WARN)

- **detect** — outbound healthy peer count < 3 for ≥60s. Witness:
  `{outbound_count, inbound_count, first_violation_unix}`.
- **remedy** — call `connman_request_outbound_floor_kick(N)` — tells
  connman to attempt N more outbound connections. If addrman has no
  fresh candidates, trigger a DNS seed re-query (existing fn — grep
  `dns_seed_query` or similar in `lib/net/`).
- **clear** — outbound count ≥ 3, OR `getenv("ZCL_PEERLESS_OK") == "1"`
  (test fixtures intentionally run with no peers).
- **max_attempts**: UNBOUNDED. Don't operator-page on a real network
  outage — surface via the status dashboard.

### 2. `sync_violation_lag` (CRITICAL, destructive remedy)

- **detect** — `peer_max_height - local_tip > 100` for ≥600s.
  Witness: `{local_tip, peer_max_height, gap, age_s}`.
- **remedy** — call `disconnect_outbound_peers(connman)` (existing fn in
  the watchdog being deleted — extract it to `lib/net/include/net/connman.h`
  as a public `connman_force_outbound_rotation(reason)` first).
- **clear** — gap drops to ≤ 100.
- **max_attempts**: 1 per 1-hour window. After that, operator page.
  Peer rotation alone isn't fixing it — something deeper is wrong.

---

## Tasks (in order)

### Task 1: Read references

- `docs/dissolve/sync_watchdog_service.md` §"PR-3"
- The 6 already-extracted conditions in `app/conditions/src/` — pattern
- `app/services/src/sync_watchdog_service.c` — find the
  `WATCHDOG_PEER_FLOOR` and `WATCHDOG_SYNC_VIOLATION` branches. Find
  the `disconnect_outbound_peers` static function.

### Task 2: Extract `connman_force_outbound_rotation` to public API

Move `disconnect_outbound_peers` from `sync_watchdog_service.c` to
`lib/net/src/connman.c` (and declare in `connman.h`). Rename to
`connman_force_outbound_rotation(struct connman *cm, const char *reason)`.

Single-call public API. No new behavior. Acceptance: build clean,
existing watchdog test (still calling it via the new name) PASS.

### Task 3: Implement `peer_floor_violated` condition

Standard pattern. Register in `condition_registry.c`.

### Task 4: Implement `sync_violation_lag` condition

Standard pattern but with the 1-attempt-per-hour rate limit. The
condition engine's `max_attempts` is "absolute"; for "max_attempts per
window" we need either:
- Extend the engine with a `cooldown_seconds_per_attempt` field, OR
- Implement the cooldown inside the condition's `detect()` (return
  false if `now - last_remedy_attempt_unix < 3600`).

The second approach is simpler — start there. If many future conditions
need cooldowns, promote it to an engine field in a follow-up.

### Task 5: Delete sync_watchdog_service.{c,h}

```bash
git rm app/services/src/sync_watchdog_service.c
git rm app/services/include/services/sync_watchdog_service.h
```

Grep for every `#include "services/sync_watchdog_service.h"` and
replace with appropriate calls:
- `sync_watchdog_init()` → no-op (conditions are auto-registered via
  `condition_registry.c`)
- `sync_watchdog_check(...)` → no-op (the condition engine ticks via
  the supervisor; the explicit check loop is gone)
- `sync_watchdog_get_stats(...)` → migrate to
  `condition_engine_get_aggregate_stats(...)` (add this if not present)
- `sync_watchdog_dump_state_json(...)` → migrate to
  `condition_engine_dump_state_json(...)` (already exists from Phase 0)
- `sync_watchdog_get_local_recovery_stats(...)` → reads condition
  state for `local_header_refill_needed` (PR-2 work)

Update MCP tools that reference `subsystem=watchdog`:
- `zcl_state subsystem=watchdog` → either keep as alias for
  `subsystem=condition_engine` OR remove and document the new key

Acceptance: `make -j$(nproc)` clean build with file deleted.

### Task 6: Unit tests for the 2 conditions

`lib/test/src/test_watchdog_conditions_pr3.c` — for each:
- detect fires under predicate
- detect does NOT fire when predicate false
- remedy is called and produces correct effect (use test hooks to
  verify `disconnect_outbound_peers` was called for sync_violation_lag)
- cooldown works for sync_violation_lag (second remedy within 1h is
  NOT attempted)
- operator-needed event for sync_violation_lag after exhaustion

Register in `test.c`, `test_parallel.c`, `test_helpers.h`.

### Task 7: Update REFACTOR_STATUS.md mega-module roster

Edit `docs/REFACTOR_STATUS.md` mega-module roster to mark
`sync_watchdog_service.c` as DELETED. **This is the ONLY allowed edit
to REFACTOR_STATUS.md for workers** — and only because PR-3 is the
proof-of-deletion PR.

### Task 8: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt2/phase3-watchdog-dissolve-pr3
```

Append Completion section. Note in the summary the LOC deletion
(1,448 LOC out, ~200 LOC in for the 2 conditions = ~1,248 LOC net out).

---

## Commit cadence

One commit per task (8 total). Push after tasks 3, 5, 6, 7.

---

## Status

**READY** — gated on PR-1 + PR-2 merge. Start when human invokes you in
`~/github/zclassic23-2` AFTER both prior PRs are merged into main.

<!-- Worker: append a Completion section below when done. -->

---

## Completion — 2026-05-24

DONE — branch `wt2/phase3-watchdog-dissolve-pr3` is ready for
orchestrator review.

Shipped the final sync watchdog dissolve slice:

- Added `peer_floor_violated` and `sync_violation_lag` conditions.
- Moved outbound peer rotation into public `connman_force_outbound_rotation`.
- Split the remaining reusable watchdog context/progress helpers into
  `sync_monitor`.
- Deleted `app/services/src/sync_watchdog_service.c`,
  `app/services/include/services/sync_watchdog_service.h`, and the legacy
  `test_sync_watchdog.c` suite.
- Kept `zcl_state subsystem=watchdog` as a compatibility alias to the
  condition engine dump.

Net code movement: 1,595 watchdog service/header lines removed; 596 lines
added for the two final conditions plus `sync_monitor`, before call-site
cleanup and tests.

Verification:

- [x] `make -j$(nproc)` — PASS
- [x] `make lint` — PASS (gate #20 remains WARN with existing baseline)
- [x] `ZCL_TEST_ONLY=watchdog_dissolve_pr2 ./test_zcl` — PASS
- [x] `ZCL_TEST_ONLY=watchdog_conditions_pr3 ./test_zcl` — PASS
- [x] `./test_parallel --jobs=$(nproc)` — PASS:
  `ALL TESTS PASSED — 0/187 groups failed (119.0s wall, 32 workers)`
