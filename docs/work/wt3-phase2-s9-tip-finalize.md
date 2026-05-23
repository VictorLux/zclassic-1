# wt3 Assignment — Phase 2: S-9 tip_finalize shadow stage (Wave S FINAL)

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase2-tip-finalize-shadow`
**Phase:** 2 (Wave S → S-12 cutover) — LAST shadow stage
**Depends on:** S-8 utxo_apply merged

**Owns:**
- NEW `app/services/include/services/tip_finalize_stage.h`
- NEW `app/services/src/tip_finalize_stage.c`
- NEW `lib/test/src/test_tip_finalize_stage.c`
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`
- Boot path: after `utxo_apply_stage_init`
- Schema migration for `tip_finalize_log`

**MUST NOT touch:**
- Any prior Wave S stage (read-only)
- `app/services/src/chain_advance_coordinator.c` — this is the module
  S-9 prepares to dissolve; read only
- `app/conditions/`, `app/controllers/`, `tools/mcp/` (wt2 owns)
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

S-9 is the LAST Wave S shadow stage. It tracks the "tip advance"
operation — the moment the chain's authoritative tip moves from H to
H+1. In production today this is driven by `chain_advance_coordinator`
through a chain of helpers; the shadow stage observes the live tip
advance and records what HAPPENED, comparing it to what the saga
WOULD do.

After S-9 lands and runs cleanly for 24h with zero divergence, the
**Wave S authoritative cutover** can begin — flipping each shadow
stage to authoritative one at a time, then deleting `chain_advance_coordinator.c`
(1,715 LOC) and the rest of the pre-saga chain-advance code.

---

## Behaviour spec

For each height H starting at `tip_finalize_cursor + 1`:

1. **Floor:** if `H > utxo_apply_cursor` → STAGE_IDLE.
2. **Upstream log:** read `utxo_apply_log[H]`. If `ok=0`:
   log `tip_finalize_log[H] = (status='upstream_failed', ok=0, ...)`,
   advance cursor.
3. **Read consensus state at H+1:** active_chain_at(ms, H+1). If
   absent: STAGE_IDLE (live path hasn't advanced yet).
4. **Verify finalize preconditions:**
   - Block at H+1 exists, `BLOCK_HAVE_DATA` set, `BLOCK_VALID_SCRIPTS`
     set (or equivalent — verify against current chainstate.c).
   - PoW is verified (`bi->nStatus & BLOCK_VALID_HEADER`).
   - The chainwork at H+1 > chainwork at H.
5. **Verify chain selection invariant:**
   - `bi->pprev == active_chain_at(H)` — the new tip MUST be a direct
     child of the old tip in shadow mode.
   - If not (reorg): log `(status='reorg_detected', ok=0,
     reorg_depth=...)` and STAGE_IDLE. Reorgs handled by S-9
     authoritative cutover, not in shadow.
6. **Verify cumulative consistency:**
   - Sum of `(spent_count, added_count)` across `utxo_apply_log[1..H+1]`
     matches the live UTXO set size delta. (Sanity check.)
   - Mismatch → `status='utxo_count_diverged', ok=0`.
7. **Success:** log `(status='finalized', ok=1, work_delta=...,
   utxo_size_after=...)`, advance cursor.

Shadow mode: this is RECORDING the finalize event, not driving it. The
live path still drives. Cutover means flipping `tip_finalize_stage` to
be the AUTHORITY that calls `active_chain_set_tip()`, which is a
follow-up sub-stage NOT in this assignment.

---

## Architecture reference

- Pattern: `app/services/src/utxo_apply_stage.c` (just shipped).
- Chain tip read: `lib/validation/src/chainstate.c` —
  `active_chain_at`, `active_chain_tip_height`.
- Block index: `lib/validation/include/validation/main_state.h` —
  `bi->nStatus` flags, `bi->pprev`.
- UTXO set size: `chain_projection_utxo_count()` — add if not present.

---

## Tasks (in order)

### Task 1: Schema migration `tip_finalize_log`

```sql
CREATE TABLE IF NOT EXISTS tip_finalize_log (
    height            INTEGER PRIMARY KEY,
    status            TEXT    NOT NULL,
    ok                INTEGER NOT NULL,
    work_delta_high   INTEGER NOT NULL,    -- chainwork delta high 64
    work_delta_low    INTEGER NOT NULL,    -- chainwork delta low 64
    utxo_size_after   INTEGER NOT NULL,
    reorg_depth       INTEGER NOT NULL,    -- 0 if not a reorg
    finalized_at      INTEGER NOT NULL
);
```

Bump migration version.

### Task 2: `tip_finalize_stage.h`

Standard 3-function API.

### Task 3: `tip_finalize_stage.c`

Mirror `utxo_apply_stage.c` structure. Counters:
- `g_finalized_total`, `g_upstream_failed_total`, `g_reorg_detected_total`,
  `g_utxo_count_diverged_total`, `g_precondition_failed_total`
- `g_total_work_added_high`, `g_total_work_added_low`

If you add `chain_projection_utxo_count()`, document the SQL:
`SELECT COUNT(*) FROM utxos` (or whatever the canonical table is).

Discipline same as prior stages.

### Task 4: Boot wiring

After `utxo_apply_stage_init(ms)`, add `tip_finalize_stage_init(ms)`.

### Task 5: Test `test_tip_finalize_stage.c`

Mirror `test_utxo_apply_stage.c`. Test cases:
- Happy path: 3 sequential finalizations all ok=1.
- Reorg: feed a block where pprev != current tip
  → `status='reorg_detected'`, STAGE_IDLE.
- Precondition failure: block missing BLOCK_HAVE_DATA →
  `status='precondition_failed'`.
- UTXO count divergence: tamper with utxo_apply_log sums
  → `status='utxo_count_diverged'`.
- Upstream failure propagates.
- Idle.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt3/phase2-tip-finalize-shadow
```

Append Completion section. Note in the summary: this is the LAST Wave
S shadow stage; Wave S cutover work follows.

---

## Live observability + cutover gate

The orchestrator runs the live node and watches:
```
zcl_state subsystem=tip_finalize
```
for 24h. **Zero divergences across `g_reorg_detected_total`,
`g_utxo_count_diverged_total`, `g_precondition_failed_total`** is the
gate for starting the Wave S authoritative cutover work in a
follow-up assignment.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.

---

## Status

**READY** — gated on S-8 merge. Start when human invokes you in
`~/github/zclassic23-3` AFTER S-8 is merged.

<!-- Worker: append a Completion section below when done. -->
