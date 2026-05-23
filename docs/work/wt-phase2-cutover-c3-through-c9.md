# Worker Assignments — Phase 2 Cutovers C-3 through C-9 (batch spec)

> Each cutover (C-3 through C-9) follows the same 4-commit PR
> structure as C-2 (`wt-phase2-cutover-c2-header-admit.md`).
> This doc lists the per-cutover DELTAS so a worker can pick up any
> one with minimal duplication.

**Branch pattern:** `wt?/phase2-cutover-c<N>-<stage>`
**Plan reference:** [`docs/architecture/wave-s-cutover.md`](../architecture/wave-s-cutover.md)
**Each cutover is independently revertable per the 4-commit PR structure.**

For each cutover below:

- Commit 1: add mode flag (`<stage>_set_mode` / `<stage>_get_mode`)
- Commit 2: add authoritative write path, gated on mode
- Commit 3: add divergence guard in the legacy path
- Commit 4: flip default mode to AUTHORITATIVE

The deltas per cutover are: which `_stage.c` to edit, which legacy
file holds the guard, what the authoritative write actually does, and
the divergence-guard predicate.

---

## C-3 — validate_headers authoritative

**Branch:** `wt?/phase2-cutover-c3-validate-headers`
**Gated on:** C-2 merged + 24h zero-divergence soak

**Stage file:** `app/services/src/validate_headers_stage.c`
**Legacy guard file:** `lib/validation/src/accept_block_header.c`
  (specifically the PoW + Equihash verification call after admit)

**Authoritative write:** the stage SETS `bi->nStatus |= BLOCK_VALID_HEADER`
when it records `validate_headers_log[H] = (ok=1)`.

**Divergence guard:** in legacy `accept_block_header`, if mode is
AUTHORITATIVE, check that
`validate_headers_log[H].ok == 1` BEFORE the legacy path tries to PoW-verify.
If the stage hasn't logged a verify-OK yet, refuse to advance with
`EV_CUTOVER_GUARD_DIVERGED("validate_headers: legacy ingress expects header
verified but stage has no record at h=%d")`.

**Test add:** with mode=AUTHORITATIVE, feed a valid header; verify
the stage's `BLOCK_VALID_HEADER` flag gets set and the legacy path
no-ops.

---

## C-4 — body_fetch (collapse, no flip)

**Note:** body_fetch is pure observer — it just tracks
`BLOCK_HAVE_DATA`. There's no separate authoritative path to flip.

**Action:** in C-5 (body_persist) cutover, also DELETE the body_fetch
stage file. Its observations are subsumed by body_persist's read.

**No separate PR for C-4.** Marked here for bookkeeping only.

---

## C-5 — body_persist authoritative

**Branch:** `wt?/phase2-cutover-c5-body-persist`
**Gated on:** C-3 merged + 24h soak

**Stage file:** `app/services/src/body_persist_stage.c`
**Legacy guard file:** `lib/validation/src/accept_block.c`
  (the `WriteBlockToDisk` or `disk_block_io_write_block` call)

**Authoritative write:** stage calls `disk_block_io_write_block(block)`
(or equivalent) and sets `bi->nStatus |= BLOCK_HAVE_DATA` when it
records `body_persist_log[H] = (ok=1)`.

**Divergence guard:** in legacy `accept_block`, if mode is AUTHORITATIVE,
check `body_persist_log[H].ok == 1` before writing the block to disk.
If absent, refuse with `EV_CUTOVER_GUARD_DIVERGED("body_persist: ...")`.

**ALSO in this PR:** delete `app/services/src/body_fetch_stage.c` and
`body_fetch_stage.h`. Update boot path. Update REFACTOR_STATUS.md
mega-module roster (note: this is the ONE exception to the
"workers don't edit status" rule — body_fetch's deletion is a milestone).

---

## C-6 — script_validate authoritative

**Branch:** `wt?/phase2-cutover-c6-script-validate`
**Gated on:** C-5 merged + 24h soak

**Stage file:** `app/services/src/script_validate_stage.c`
**Legacy guard file:** `lib/validation/src/connect_block.c`
  (the per-input `script_verify` calls inside the connect loop)

**Authoritative write:** stage sets `bi->nStatus |= BLOCK_VALID_SCRIPTS`
when it records `script_validate_log[H] = (ok=1)`.

**Divergence guard:** in legacy `connect_block`, if mode is AUTHORITATIVE,
check `script_validate_log[H].ok == 1` before running per-input script_verify.
If absent OR `script_validate_log[H].ok == 0`, refuse.

**Performance note:** the SHADOW stage already verifies every script.
Cutover means the legacy connect_block STOPS re-verifying. This is
where the saga starts paying for itself in CPU: same work, done once
instead of twice.

---

## C-7 — proof_validate authoritative

**Branch:** `wt?/phase2-cutover-c7-proof-validate`
**Gated on:** C-6 merged + 24h soak

**Stage file:** `app/services/src/proof_validate_stage.c`
**Legacy guard file:** `lib/validation/src/connect_block.c`
  (proof verification inside the connect loop — typically right after
  script verification)

**Authoritative write:** stage sets `bi->nStatus |= BLOCK_VALID_PROOFS`
when it records `proof_validate_log[H] = (ok=1)`.

**Divergence guard:** mirror C-6. Legacy connect_block skips proof
verify if `proof_validate_log[H].ok == 1`, else refuses.

**Performance note:** proof verification is the most expensive single
step in connect_block. C-7 is where the saga's CPU win is largest.

---

## C-8 — utxo_apply authoritative

**Branch:** `wt?/phase2-cutover-c8-utxo-apply`
**Gated on:** C-7 merged + 24h soak

**Stage file:** `app/services/src/utxo_apply_stage.c`
**Legacy guard file:** `lib/validation/src/connect_tip.c`
  (the `update_coins(block, view)` call)

**Authoritative write:** stage calls `update_coins(block, view)` itself
when it records `utxo_apply_log[H] = (ok=1)`.

**Divergence guard:** in legacy `connect_tip`, if mode is AUTHORITATIVE,
check `utxo_apply_log[H].ok == 1` before calling update_coins. If
absent, refuse.

**Special acceptance:** this is the destructive one. The shadow-vs-live
diff (`g_delta_diverged_total`) MUST be 0 in the 24h shadow soak before
C-8 even begins. Otherwise the saga's UTXO delta computation has a
bug and cutover would corrupt the UTXO set.

**ALSO in this PR:** after C-8, `utxo_recovery_service.c` becomes
dissolvable per `docs/dissolve/utxo_recovery_service.md`. Mark in
REFACTOR_STATUS.md.

---

## C-9 — tip_finalize authoritative

**Branch:** `wt?/phase2-cutover-c9-tip-finalize`
**Gated on:** C-8 merged + 24h soak

**Stage file:** `app/services/src/tip_finalize_stage.c`
**Legacy guard file:** `app/services/src/chain_advance_coordinator.c`
  (the `active_chain_set_tip(bi)` call deep in the plan/dispatch path)

**Authoritative write:** stage calls `active_chain_set_tip(bi)` when
it records `tip_finalize_log[H] = (ok=1, status='finalized')`.

**Divergence guard:** in legacy `chain_advance_coordinator_plan`, if
mode is AUTHORITATIVE, check `tip_finalize_log[H].ok == 1` before
ever calling `active_chain_set_tip`. If absent, refuse.

**Special acceptance:** this is the FINAL cutover. After C-9:
- The saga is fully authoritative end-to-end.
- `chain_advance_coordinator.c` no longer drives anything; it's a
  diagnostics shell.
- `legacy_mirror_sync_service.c` becomes dissolvable per its plan.
- Mega-module roster in REFACTOR_STATUS.md gets a major update.

After C-9 + 24h soak, dispatch the Phase 3 mega-module dissolves
(`chain_advance_coordinator.md`, `legacy_mirror_sync_service.md`,
`chain_restore_service.md`, `header_probe_service.md`,
`utxo_recovery_service.md`) — they can land in parallel because
each touches a different module.

---

## Per-cutover acceptance gate (all of C-3..C-9)

1. `make test_parallel` PASS with cutover at AUTHORITATIVE in test
   fixtures.
2. Live 24h with mode SHADOW: zero divergence counters tick up in
   `zcl_state subsystem=<stage>`.
3. Live 24h with mode AUTHORITATIVE (after cutover PR merges): zero
   `EV_CUTOVER_GUARD_DIVERGED` events. Chain advance velocity
   unchanged.

If gate 3 fails: `git revert <commit-4>` of the failing PR. Investigate.
Re-attempt when fixed.

---

## Rollback path

Each cutover's commit 4 can be reverted independently:

```bash
git revert <commit-4-sha>
git push origin main
```

This flips the stage back to SHADOW mode. Legacy path resumes. The
guard stays in place (harmless in SHADOW mode). The next cutover
attempt doesn't need to re-do commits 1-3.

For a deeper unwind:
```bash
git revert <commit-4>..<commit-1>     # revert the whole PR
```

---

## Status

**QUEUED in sequence** — gated on the previous cutover + 24h soak.

Dispatch order:
1. C-3 after C-2 + soak
2. C-5 after C-3 + soak (C-4 is folded into C-5)
3. C-6 after C-5 + soak
4. C-7 after C-6 + soak
5. C-8 after C-7 + soak (most critical — utxo correctness)
6. C-9 after C-8 + soak (final — chain advance ownership)

Each cutover is ~half a worker session because the PR structure is
small. Total cutover work: ~3 worker sessions across 6 cutover PRs.

<!-- Each worker assignment for a specific cutover appends a Completion section. -->
