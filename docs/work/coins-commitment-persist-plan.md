# Plan: persist the `utxo_sha3` commitment so the §3 self-heal auto-fires

**Status:** designed + adversarially vetted (2026-06-03), NOT yet implemented.
Deliberately deferred: it lands in the synchronous block-connect/flush path
and should not be rushed. This doc is implementation-ready.

## Why

The shipped boot self-heal `coins_reconcile_stale_anchor`
(`lib/storage/src/coins_view_sqlite.c`, commit `11f87b8b6`) heals a stale
`coins_best_block` anchor **only** when a stored height-stamped SHA3 commitment
(`utxo_sha3`) recompute-matches the live `utxos` table. But `utxo_sha3` is
currently written only at snapshot-import + one RPC path, so a long-running
node never has a fresh one — the self-heal can't fire on a *recurring* §3 wedge.
This closes that gap by persisting the commitment periodically at a consistent
boundary. (`-reindex-chainstate` reachability — commit `50b33285d` — is the
operator fallback; this makes the *automatic* path real.)

## Boundary

The single point where the durable `utxos` table is provably consistent with
`coins_best_block` at a known height is **immediately after
`coins_view_sqlite_batch_write_ex()` returns true inside
`flush_coins_if_needed()`** (`lib/validation/src/process_block_flush_policy.c`):
that flush committed the UTXO mutations AND `coins_best_block` atomically on the
dedicated coins handle — no mid-flush state exists.

## The change (file: `lib/validation/src/process_block_flush_policy.c`)

Add a cadence-gated helper called in the `if (ok)` branch after the WAL
checkpoint, plus a `#ifdef ZCL_TESTING` hook so a test can drive it without a
live chain. Cadence counters: `_Atomic int64_t g_flushes_since_commitment`,
`g_last_commitment_unix`; macros `COINS_COMMITMENT_FLUSH_INTERVAL` (8),
`COINS_COMMITMENT_MIN_SECS` (1800).

## The 3 adversary fixes — MANDATORY (each was a high-severity finding)

1. **Performance — never scan on the sync hot path.** The ~1s SHA3 scan over
   1.34M rows must NOT fire during IBD. Gate it on a *caught-up-to-tip* signal
   (suppress while syncing) in addition to the Nth-flush + wall-clock cadence.
   Fix the first-fire hole: initialise `g_last_commitment_unix` to `now` (or
   treat `last==0` as defer-one-window) so the 1800s floor is never bypassed.

2. **Consistency — prove the heal's invariant over one snapshot.** Run
   resolve-height + a `rows_above(height)==0` check + the SHA3 scan inside a
   single read transaction on `cvs->db`, holding `cvs->mutex` for the whole
   helper, so no other cvs writer interleaves and the committed set is exactly
   the one stamped. (`utxos` can legitimately hold rows above the durable tip
   after a one-block overshoot — the persist must refuse to stamp then, the
   same invariant `coins_reconcile_stale_anchor` requires.)

3. **Height-correctness — bind to block IDENTITY, not just height.** Store the
   32-byte `coins_best_block` hash alongside the commitment (extend the
   `utxo_sha3` record to 76 bytes, back-compat load on len, OR a sibling key
   `utxo_sha3_anchor`). Then `coins_reconcile_stale_anchor` should re-point the
   anchor to the STORED hash instead of re-resolving height→hash via the
   non-unique `blocks WHERE height=? AND status>=3` (which can name an orphan
   sibling after a mid-reorg crash). This also closes the documented
   orphan-sibling residual in the shipped heal.

## Skip conditions (return without stamping; retry next boundary)

- flushed hash not yet `status>=3` in `blocks` (block-index lag) — LOG_WARN, skip.
- height > INT32_MAX. - rows above the resolved height (overshoot) — skip.
- not caught up to tip. - cadence not due.

## Test (`lib/test/src/test_coins_commitment_persist.c`, new)

Driving the real `flush_coins_if_needed` needs a full chain, so instead: build a
min node.db, apply a set, invoke the `ZCL_TESTING` hook
`process_block_test_persist_commitment_once(cvs, flushed_hash)` (forces cadence,
runs the real helper), tear the anchor, and assert `coins_view_sqlite_open()`
heals against the just-persisted commitment. Register in the usual 3 places.
The full drafted code is in the workflow result
(`finish-s3-unhaltable`, run `wf_77539f37-539`).

## Out of scope (D audit, all P3 — not worth the churn)

DRY-extract the anchor-resolve SQL (3 sites); route `EV_OPERATOR_NEEDED` from
the boot coins `_exit` (blocked on `alerts_init` ordering); redundant ROLLBACK
after a failed COMMIT in `coins_rewind_above_tip`. Surfaces audited clean
otherwise.
