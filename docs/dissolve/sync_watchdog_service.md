# Dissolve plan: `sync_watchdog_service.c` → 8 Conditions

**Module:** `app/services/src/sync_watchdog_service.c` (1,448 LOC)
**Header:** `app/services/include/services/sync_watchdog_service.h` (147 LOC)
**Phase:** 2 (Wave S → S-12 cutover) and Phase 3 (mega-module dissolution)
**Strategy:** strangler — extract one condition at a time, prove parity,
delete the corresponding watchdog branch. Module is FULLY DELETED only
when the last branch is migrated.

The 10 enum `watchdog_recovery_type` entries collapse into ~8 Conditions
(some merge, some split). Each Condition is a `(detect, remedy, witness)`
auto-healer registered into the engine wt2 shipped in Phase 0.

---

## Why dissolve

- 1,448 LOC of branching, atomics, and escalation state — every change
  is high-risk. 4 of the 5 recent operator-page-ish incidents traced
  back to one of these branches: peer floor escalation locking out
  legitimate inbound, header-refill spinning on a poisoned tip, etc.
- `sync_watchdog_check` is invoked from two places (msg-processing
  loop + heartbeat thread) and was silently disabled in production
  once when the per-peer-id dispatch lost peer 0. The Condition pattern
  removes the dispatch coupling — each Condition is supervised
  independently.
- The recovery actions are remediations that the Condition primitive
  was designed for. Today they're inline state-machine transitions
  that can fire L2 escalation when an L0 cause is still active —
  observability is opaque. Conditions emit
  `EV_CONDITION_DETECTED / REMEDY_ATTEMPTED / CLEARED` for every
  attempt, with witness data on every clear.

---

## The 8 conditions

### 1. `header_stall_at_height` (CRITICAL)

Maps from: `WATCHDOG_HEADER_STALL` + `WATCHDOG_HEADER_LAG` (these two
fire on the same observable — they collapse).

- **detect** — best_header_height has not advanced for ≥300s while
  peer max > local header height. Witness payload: `{stuck_at_height,
  age_s, peer_max_height, gap}`.
- **remedy** — `header_probe_kick(reason="condition:header_stall")`
  (the existing function inside header_probe_service). One call,
  backoff if it fails.
- **clear** — header height advances OR peer floor drops below 3
  (then `peer_floor_violated` owns the recovery).
- **max_attempts** before operator page: 3 over 15 minutes.

### 2. `block_stall_at_tip` (CRITICAL)

Maps from: `WATCHDOG_BLOCK_STALL`.

- **detect** — chain height has not advanced for ≥300s while
  `BLOCK_HAVE_DATA` on next height. (We have the body but can't
  connect.)
- **remedy** — `process_block_revalidate(next_block, &g_ms, &out_hash)`
  same call path as the existing `block_failed_mask_at_tip` condition.
  Should partially overlap; verify and DEDUPE — likely this condition
  IS `block_failed_mask_at_tip` with a different detect predicate. If
  so, extend the existing condition with the new predicate rather than
  shipping a duplicate.
- **clear** — chain height advances.
- **max_attempts**: 5 (the existing block_failed condition).

### 3. `sync_state_stuck` (WARN)

Maps from: `WATCHDOG_STATE_STUCK`.

- **detect** — any non-tip sync state unchanged for ≥600s with no
  more-specific condition active (i.e., not header_stall, not
  block_stall, not peer_floor). Witness:
  `{state_name, entered_unix, height_at_entry}`.
- **remedy** — `sync_state_kick(reason)` — try to push the FSM forward
  by calling the appropriate handler for the current state (e.g.,
  request more headers if in HEADERS, kick body downloads if in
  BODIES). NO automatic state transition — the kick is purely a poke.
- **clear** — state changes OR the more-specific condition that should
  own the stuck case fires.
- **max_attempts**: 3 over 30 minutes, then operator page.

### 4. `peer_floor_violated` (WARN)

Maps from: `WATCHDOG_PEER_FLOOR`.

- **detect** — outbound healthy peers < 3 for ≥60s (current threshold).
  Witness: `{outbound_count, inbound_count, first_violation_unix}`.
- **remedy** — `outbound_floor_kick()` — tells connman to attempt N
  more outbound connections from the addrman pool. If that fails (no
  addresses), trigger a DNS seed re-query (existing fn).
- **clear** — outbound count ≥ 3 OR the node is intentionally
  isolated (env var `ZCL_PEERLESS_OK=1` — for test fixtures only).
- **max_attempts**: unbounded (peer floor is a real network reality;
  don't page the operator for a network outage — surface it via the
  status dashboard instead).

### 5. `sync_violation_lag` (CRITICAL)

Maps from: `WATCHDOG_SYNC_VIOLATION`.

- **detect** — peer_max_height - local_tip > 100 for ≥600s.
- **remedy** — L2 escalation: `disconnect_outbound_peers()` (current
  behaviour) — force fresh peer rotation. Single attempt then escalate.
- **clear** — gap drops to ≤ 100.
- **max_attempts**: 1 inside a 1-hour window (pager-friendly). After
  that, page the operator — peer rotation hasn't helped, something
  deeper is wrong.

### 6. `utxo_activation_paused` (CRITICAL)

Maps from: `WATCHDOG_UTXO_PAUSE`.

- **detect** — chain activation paused (`g_activation_paused == true`)
  for ≥300s. Witness: `{pause_reason, paused_at}`.
- **remedy** — `chain_advance_coordinator_resume(reason)` —
  the existing resume entry point. If `pause_reason ==
  "utxo_audit_drift"` the remedy is REPAIR via `utxo_recovery_kick()`
  (different code path).
- **clear** — activation resumes.
- **max_attempts**: 2 (resume is cheap to retry; if it doesn't work
  twice in a row something else is wrong).

### 7. `download_queue_starved` (WARN)

Maps from: `WATCHDOG_QUEUE_STARVED`.

- **detect** — download in-flight slots < 10% for ≥120s while sync
  state is BODIES.
- **remedy** — `download_manager_refill_queue()` from the existing
  download.c entry point.
- **clear** — in-flight ≥ 50%.
- **max_attempts**: unbounded (refill is cheap and idempotent).

### 8. `local_header_refill_needed` (WARN)

Maps from: `WATCHDOG_LOCAL_HEADER_REFILL`.

- **detect** — `active_chain_tip + 1` not present in block index AND
  the corresponding header is not staged anywhere AND peer count > 0.
- **remedy** — `header_probe_kick_for_height(active_chain_tip + 1)`
  with explicit peer rotation (the local_recovery_reset path).
- **clear** — the missing header appears in the block index.
- **max_attempts**: 3 with peer rotation between each. After 3,
  emit `EV_OPERATOR_NEEDED` with witness `{missing_height,
  attempted_peers, distinct_peers_tried}`.

The `WATCHDOG_REPEATED_RESTART` circuit breaker is NOT a condition —
it's a property of the condition engine itself (max_attempts +
operator-paging). The engine already enforces a per-condition cap.

---

## Migration sequence (3 PRs)

### PR-1: Extract conditions 2 + 6 (overlap with existing Phase 0 conditions)

- Condition 2 `block_stall_at_tip` is morally the same as the existing
  `block_failed_mask_at_tip`. EXTEND the existing condition's `detect`
  predicate to also fire on "BLOCK_HAVE_DATA at tip+1 but tip hasn't
  advanced in 300s". Verify against live node before/after.
- Condition 6 `utxo_activation_paused` is new — wire it up.
- DELETE the corresponding branches from
  `sync_watchdog_check()`: `WATCHDOG_BLOCK_STALL` and
  `WATCHDOG_UTXO_PAUSE`. Add a `/* moved to conditions/ */`
  marker so reviewers know what happened.
- Ship gate: live node runs for 72h, condition #2 fires at least once
  (or no real block-stall in that window), no operator pages.

### PR-2: Extract conditions 1, 3, 7, 8 (the kick-only conditions)

These are "poke the right subsystem" remediations with no destructive
recovery action. Safe to land together — failures don't cascade.

- Delete corresponding watchdog branches.
- After this PR, sync_watchdog_service.c is down to ~500 LOC of
  just peer-related logic (conditions 4, 5).

### PR-3: Extract conditions 4 + 5; DELETE sync_watchdog_service.c

These two own the destructive `disconnect_outbound_peers` action.
Land last so we can prove the condition engine's `max_attempts +
operator paging` works under the most aggressive remedy.

After this PR:
- `app/services/src/sync_watchdog_service.c` — DELETED (1,448 LOC gone)
- `app/services/include/services/sync_watchdog_service.h` — DELETED
- `app/conditions/src/header_stall_at_height.c` — NEW
- `app/conditions/src/block_stall_at_tip.c` — folded into existing
  `block_failed_mask_at_tip.c`
- `app/conditions/src/sync_state_stuck.c` — NEW
- `app/conditions/src/peer_floor_violated.c` — NEW
- `app/conditions/src/sync_violation_lag.c` — NEW
- `app/conditions/src/utxo_activation_paused.c` — NEW
- `app/conditions/src/download_queue_starved.c` — NEW
- `app/conditions/src/local_header_refill_needed.c` — NEW
- Net: ~1,450 LOC out, ~600 LOC in. **~850 LOC deletion.**
- `chain_tip_watchdog` (shipped pre-refactor as `fb32df981`) can
  ALSO retire — it was a single-purpose precursor to condition #2.

---

## Acceptance gates (each PR)

- `make lint` PASS.
- `make test_parallel` PASS, including new unit tests for each new
  condition (mirror `test_condition_engine.c` pattern from Phase 0).
- Live node smoke: 24h with `zcl_conditions` polled hourly. Every
  detected condition either remedied (cleared) or hit max_attempts and
  paged. Zero unexplained stalls.
- `zcl_state subsystem=supervisor` lists the registered conditions.
- Mega-module roster in REFACTOR_STATUS.md ticks down each PR.

---

## Worker assignment shape (future)

When the orchestrator dispatches PR-1, the worker assignment is:

> Extract `WATCHDOG_BLOCK_STALL` + `WATCHDOG_UTXO_PAUSE` into
> Conditions per `docs/dissolve/sync_watchdog_service.md` §"PR-1".
> Owns: `app/conditions/src/block_failed_mask_at_tip.c` (extend),
> NEW `app/conditions/src/utxo_activation_paused.c`,
> edits to `app/services/src/sync_watchdog_service.c` (delete branches).
> MUST NOT touch other watchdog branches (PR-2/PR-3 owns those).

Same structure as the Phase 1 assignments — explicit scope, MUST-NOT
list, tasks-in-order, acceptance gates.

---

## Status

DRAFT — orchestrator-owned. Update each PR with the actual outcome,
parity findings, and any branch that ended up larger or smaller than
the spec.
