# wt-consolidate-recovery-paths — DRY the recovery sprawl, purge the band-aids

## Status

**IN PROGRESS (wt3) — claimed 2026-05-25.** Root fix is marked deployed with
live forward progress in `wt-connect-bip30-selfwrite.md`; start with the bounded
rewind helper to avoid broad condition churn.

Task A complete in wt3: `coins_rewind_above_tip(db, tip_height, max_rows)` is
now the single storage-layer helper for bounded auto-rewind and explicit
above-tip UTXO pruning; `coins_view_sqlite`, `utxo_recovery_service`, and
`boot.c` all call it.

> Why queued, not now: days of whack-a-mole accreted overlapping wedge-recovery
> paths. Most exist only because `connect_block` kept false-wedging. Fix the cause
> first; then a lot of this is provably dead and safe to delete.

## Research inventory (2026-05-25, read-only survey)

Codebase: 754 `.c`, ~370k LOC, 15 MB binary.

### A. Verified DRY violation — collapse to ONE
The single-block "rewind UTXOs above tip" logic (`DELETE FROM utxos WHERE
height > ?` + by-txid + transactions + clear `utxo_commitment`) is **copy-pasted
in three places**:
- `lib/storage/src/coins_view_sqlite.c` (`coins_view_sqlite_rewind_above_tip`)
- `app/services/src/utxo_recovery_service.c` (`utxo_recovery_clean_above_tip`, `dbf4845a1`)
- `config/src/boot.c`

→ Extract one `coins_rewind_above_tip(db, tip_height, max_rows)` (storage layer),
call it from all three. One bounded-guard implementation, one test. ~150 LOC net
deletion expected.

### B. Wedge/stall Conditions — re-evaluate after the root fix (17 today)
Many tip/stall conditions are safety nets for the BIP30 false-wedge the root fix
removes. After it lands, audit each for "does this still fire on a healthy node?":
candidates to retire or merge — `block_failed_mask_at_tip`,
`tip_wedged_resnapshot`, `chain_stalled_with_data`, `contradiction_frozen`,
`sync_state_stuck`. **Do NOT blind-delete** — keep the ones that catch genuine
distinct failures (resilience doctrine). Merge overlapping detectors; delete only
those whose symptom is now impossible. Target: 17 → ~10, each with a distinct,
still-reachable trigger.

### C. Doc-vs-reality drift to fix (cheap, do in this PR)
- `LEGACY_LIFECYCLE.md` says `legacy_body_pull` is "disabled at boot," but it ran
  every few seconds in live logs (gap-fill invokes it at runtime). Either narrow
  the API as the doc promises ("slated for narrower API") or correct the doc.
- The doc lists `-fastimport`, `-importfromlegacy`, `-nolegacyimport`,
  `-legacy-auto-import`; only `-cold-import` and `-legacy-attach` appear as string
  literals in the tree. Reconcile: delete dead flag docs or wire the missing flags.

### D. Import paths — confirm, don't assume
3 import modules (`legacy_cold_import`, `legacy_direct_import`,
`legacy_oneshot_import`) + `legacy_body_pull`. LEGACY_LIFECYCLE marks cold/direct
Active. Confirm `legacy_oneshot_import` (5 refs) vs `legacy_direct_import` (1 ref)
aren't redundant; collapse if one wraps the other.

## Tasks (after the gate)
1. [x] Extract `coins_rewind_above_tip` helper; rewire all 3 callers; one test. (A)
2. Live-audit the 17 conditions; merge/retire the now-unreachable ones. (B)
3. Fix the LEGACY_LIFECYCLE drift + dead flag docs. (C)
4. Confirm/collapse redundant import wrappers. (D)

## Acceptance
- `make test_parallel` clean, `make lint`; net LOC **down**, condition count down.
- Node still survives the chaos harness (`make chaos`) and a kill-9 loop — purging
  band-aids must not regress real recovery. Live forward progress unaffected.

## Non-goals
- Touching connect_block/coins/boot while Agent 1 is active (collision).
- Deleting a safety net whose failure mode is still reachable (verify first).

## References
- Root fix this is gated on: `wt-connect-bip30-selfwrite.md`.
- `LEGACY_LIFECYCLE.md` (active/deprecated source of truth).
- Memory: `feedback_less_is_more_holistic`, `feedback_stop_drifting_ideal_first`.
