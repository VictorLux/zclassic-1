# Wave S cutover — flipping shadow stages to authoritative

**Status:** PLAN (draft 2026-05-23)
**Phase:** 2 (Wave S → S-12 cutover)
**Gated on:** S-9 tip_finalize shadow stage shipped + 24h zero-divergence soak
**Estimated scope:** 9 cutover PRs (one per stage), ~3,800 LOC net deletion unlocked

> "Shadow runs FIRST, lives long enough to prove zero divergence,
> then becomes the authority while the legacy path becomes a guard."
> — Wave S convention

---

## What "cutover" means

A **shadow stage** observes the live path and records what it WOULD do
in its `<stage>_log` table. The live path is still authoritative.

An **authoritative stage** is what produces the canonical state. Other
code reads from its log.

**Cutover** = flipping shadow → authoritative. Done one stage at a
time, with rollback at every step. The previous shadow stays in place
as a **divergence guard** for one more cycle.

The cutover order is the saga order: S-2 → S-3 → S-4 → S-5 → S-6 → S-7
→ S-8 → S-9. You CAN'T cut over S-5 before S-2 because S-5 reads
S-4's log which reads S-3's log which reads S-2's log.

---

## Per-stage cutover playbook

Each cutover lands as ONE PR per stage. The PR has 4 commits.

### Commit 1: Add `<stage>_authoritative` flag

```c
/* in stage's .h */
typedef enum {
    STAGE_MODE_SHADOW = 0,    /* observe only — current default */
    STAGE_MODE_AUTHORITATIVE  /* drive the live state */
} stage_mode_t;

void <stage>_set_mode(stage_mode_t mode);
stage_mode_t <stage>_get_mode(void);
```

Default mode stays SHADOW. The flag is toggleable for testing.

### Commit 2: Add the authoritative write path (behind the flag)

For each stage, the authoritative path is concrete:

| Stage | Shadow records | Authoritative writes |
|---|---|---|
| S-2 header_admit | row in `header_admit_log` | calls `block_index_admit_header(bi)` |
| S-3 validate_headers | PoW verified flag | sets `bi->nStatus |= BLOCK_VALID_HEADER` |
| S-4 body_fetch | tracks BLOCK_HAVE_DATA | (no authoritative write — pure observer; cutover collapses it into S-5) |
| S-5 body_persist | header+merkle verified | calls `write_block_to_canonical_storage(block)` |
| S-6 script_validate | per-input verify result | sets `bi->nStatus |= BLOCK_VALID_SCRIPTS` |
| S-7 proof_validate | per-proof verify result | sets `bi->nStatus |= BLOCK_VALID_PROOFS` |
| S-8 utxo_apply | computed UTXO delta | calls `update_coins(block, view)` |
| S-9 tip_finalize | observed tip advance | calls `active_chain_set_tip(bi)` |

In the cutover PR, the authoritative write is added inside the stage
but gated on `stage_mode == AUTHORITATIVE`. SHADOW mode is the default,
so nothing changes for shipping.

### Commit 3: Add the legacy-path bypass + guard

The legacy code path that USED to do this work (e.g., the chain-advance
coordinator's body of work) is gated:

```c
if (<stage>_get_mode() == STAGE_MODE_SHADOW) {
    /* legacy path: do the work, as today */
    legacy_do_work();
} else {
    /* authoritative cutover: stage owns this; we just guard */
    if (legacy_compute_expected() != stage_get_last_observed()) {
        EMIT(EV_CUTOVER_GUARD_DIVERGED, ...);
        /* refuse to advance — operator intervention required */
    }
    /* else proceed; stage already did the work */
}
```

The **divergence guard** is the safety net. If the legacy path's
expectation differs from the stage's record, we KNOW something's
wrong and we stop the chain advance with an EV_OPERATOR_NEEDED before
corrupting state.

### Commit 4: Flip the default mode to AUTHORITATIVE

Default mode becomes AUTHORITATIVE. Cutover live.

The PR ships in this order so each commit is independently revertable:
- Commit 1 ships the flag (always-off default, no behavior change)
- Commit 2 ships the authoritative implementation (still gated off)
- Commit 3 ships the guard (still gated off — guard only runs in
  AUTHORITATIVE mode)
- Commit 4 flips the default. If something breaks live, REVERT JUST
  COMMIT 4 — the rest of the code stays and the next cutover attempt
  doesn't need to re-do commits 1-3.

---

## Per-stage cutover acceptance

Each cutover PR has THREE gates:

1. **`make test_parallel` PASS** with `<stage>_set_mode(AUTHORITATIVE)`
   set at test fixture init. All existing per-stage tests + integration
   tests pass with the saga running authoritatively.

2. **Live 24h shadow soak with zero divergence** (mode still SHADOW).
   The orchestrator queries
   `zcl_state subsystem=<stage>` hourly. Counter
   `<stage>_diverged_total` must stay at 0 throughout.

3. **Live 24h authoritative soak** AFTER the cutover PR is merged.
   Mode flipped to AUTHORITATIVE. The orchestrator watches for:
   - `EV_CUTOVER_GUARD_DIVERGED` events — should be 0
   - Chain advance velocity unchanged (blocks/sec, lag-to-tip)
   - No operator pages

If gate 3 fails, REVERT COMMIT 4 of that PR. Investigate. Re-flip when
the divergence is understood and fixed.

---

## The 9 cutover PRs in order

| PR | Stage | What dies |
|---|---|---|
| C-2 | S-2 header_admit | legacy `accept_block_header` ingress path's direct block-index write |
| C-3 | S-3 validate_headers | legacy header PoW verify in `accept_block_header` |
| C-4 | S-4 body_fetch | this stage collapses into S-5 (it's pure observer) — no separate cutover, just delete the stage at C-5 time |
| C-5 | S-5 body_persist | direct `write_block_to_canonical_storage` from `accept_block` |
| C-6 | S-6 script_validate | per-input `script_verify` in `connect_block` |
| C-7 | S-7 proof_validate | proof verification in `connect_block` |
| C-8 | S-8 utxo_apply | direct `update_coins` in `connect_tip` |
| C-9 | S-9 tip_finalize | `active_chain_set_tip` in `chain_advance_coordinator_plan` |

After C-9 lands and soaks, the entire `chain_advance_coordinator.c`
authoritative role is gone — it's just a router and diagnostics shell.
That's when `docs/dissolve/chain_advance_coordinator.md` becomes
actionable.

---

## Parallelism

C-2 through C-9 are NOT parallelizable across workers — each one
mutates the same legacy code path. They go strictly sequentially.

However, between cutovers, **dissolves are parallel**:
- After C-2 lands, header_probe.c can begin dissolving
  (`docs/dissolve/header_probe_service.md` — to be drafted).
- After C-5 lands, block-data-related dissolves can begin.
- After C-8 lands, utxo_recovery_service.c can begin dissolving.
- After C-9 lands, both chain_advance_coordinator.c AND
  legacy_mirror_sync_service.c become deletable in parallel.

So the schedule is interleaved: **C-N cutover** is sequential with
the previous, but **dissolves after C-N** are parallel work for wt3 and wt2.

---

## Rollback path

Each cutover PR has commit-level rollback:

```bash
git revert <commit-4-of-the-PR>     # flip mode back to SHADOW
git push origin main                  # divergence guard still in place, no risk
```

If the underlying stage code has a bug, ALSO revert commits 1-3:
```bash
git revert <commit-4>..<commit-1>
```

The legacy path is still 100% intact — reverting puts us back at
SHADOW mode, where the legacy path runs and the stage observes.

This is the load-bearing reason for the 4-commit structure: **the
default can be flipped without code changes**, and the cutover can be
rolled back without disturbing the rest of the stage's code.

---

## What survives cutover

After all 9 cutovers land:

- 9 stage modules, each ~500-700 LOC = ~5,000 LOC. AUTHORITATIVE.
- Their logs (`<stage>_log` tables) are the audit trail.
- 1 supervisor child running the saga = ~50 LOC.
- The "shadow" mode flag remains as a debug aid (set
  `ZCL_STAGE_SHADOW=1` to revert any stage to observe-only for
  forensics).

What disappears:
- `chain_advance_coordinator.c` (1,715 LOC) — replaced by source_scorer
  + saga driver per dissolve plan
- `sync_watchdog_service.c` (1,448 LOC) — replaced by 8 conditions
- `legacy_mirror_sync_service.c` (1,411 LOC) — replaced by legacy_bridge
  + legacy_poll job + 1 condition
- Large chunks of `process_block.c`, `connect_block.c`, `connect_tip.c`
  — the work moves into stages; these files shrink to thin glue

**Net deletion across Wave S authoritative cutover: ~7,000 LOC** per
FRAMEWORK.md §8.

---

## Status

DRAFT — actionable AFTER S-9 ships and soaks 24h with zero divergence.

The cutover PRs are not yet worker-dispatchable; each one needs a small
per-stage assignment doc when the previous cutover proves stable.

The first cutover assignment will be `wt?-phase2-cutover-c2-header-admit.md`,
gated on:
1. S-9 shipped and soaked
2. Orchestrator green-lights via REFACTOR_STATUS.md update
