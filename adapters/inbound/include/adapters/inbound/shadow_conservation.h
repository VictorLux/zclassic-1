/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * shadow_conservation — process-global conservation ledger for the
 * shadow pipeline.
 *
 * THE NAMED CUTOVER GAP THIS CLOSES
 * ---------------------------------
 * The shadow feeder counts blocks per *handle* (shadow_feeder.observed)
 * and the legacy diff counts heights per *call* (diff.checked_count).
 * Neither is process-global, so there was no place to assert the
 * conservation law that gates a trustworthy cutover:
 *
 *     every block FED into the shadow pipeline was actually DIFFED
 *     against legacy.
 *
 * Without that assertion a silently-dropped block (skipped by the diff,
 * lost between feeder and diff) would leave the shadow pipeline looking
 * healthy while secretly diverging. This ledger makes the drop visible.
 *
 * THE HONEST INVARIANT
 * --------------------
 * The law is NOT a blind `fed == diffed`. There is one legitimate
 * fed-but-not-diffed class: a block whose VALIDATE_BLOCK push hit mutator
 * backpressure. Such a block is appended to the shadow log (intent
 * recorded) but never enters the validate/diff path on this pass — a
 * recovery scan re-feeds it later. We account for it explicitly as
 * `skipped`, so the true conservation law is:
 *
 *     fed == diffed          (at quiesce, every fed block was diffed)
 *     skipped                (separate honest accounting of the
 *                             backpressure drop class — fed to the
 *                             feeder but not into the diff path)
 *
 * `fed` mirrors the feeder's per-handle `observed` (successful append +
 * queue), `skipped` mirrors `backpressure_hits`, and `diffed` is bumped
 * by each diff completion (by the number of heights actually compared).
 *
 * QUIESCED-STATE CAVEAT
 * ---------------------
 * The diff is a batch over a height range; it lags the live feeder. A
 * transient `fed > diffed` while blocks are in flight (fed but not yet
 * covered by a diff pass) is EXPECTED and is not a violation. The
 * equality check is a snapshot — callers assert it only against a
 * QUIESCED pipeline (ingest stopped, a final diff run over the full fed
 * range completed). See test_shadow_conservation.c.
 *
 * BEST-EFFORT, NEVER GATES
 * ------------------------
 * Every recorder is a relaxed atomic add. The counters OBSERVE the
 * pipeline; they never gate it, never change the diff RESULT, and never
 * block the hot ingest path. Recording is fire-and-forget.
 */

#ifndef ZCL_ADAPTERS_INBOUND_SHADOW_CONSERVATION_H
#define ZCL_ADAPTERS_INBOUND_SHADOW_CONSERVATION_H

#include <stdbool.h>

/* Record that `n` block(s) were fed into the shadow pipeline (appended
 * to the shadow log AND queued for validate). Mirrors the point where
 * shadow_feeder bumps its per-handle `observed`. Best-effort. */
void shadow_conservation_record_fed(unsigned long n);

/* Record that `n` block(s) were fed to the feeder but legitimately not
 * pushed into the validate/diff path (mutator backpressure). The block
 * is in the shadow log; a recovery scan re-feeds it later. Best-effort. */
void shadow_conservation_record_skipped(unsigned long n);

/* Record that `n` block(s) completed a legacy byte-diff. Called by the
 * diff-completion callers with the number of heights actually compared
 * (diff report `checked_count`). Best-effort. */
void shadow_conservation_record_diffed(unsigned long n);

/* Snapshot the three counters. Any of the out pointers may be NULL.
 * The reads are independent relaxed loads, so a concurrent recorder may
 * land between them — fine for a quiesced-state assertion. */
void shadow_conservation_snapshot(unsigned long *fed,
                                  unsigned long *diffed,
                                  unsigned long *skipped);

/* The conservation predicate. Returns true iff `fed == diffed` (every
 * fed block was diffed). The optional out pointers receive the snapshot
 * values regardless of the result, so a failing assert can report them.
 *
 * Intended for QUIESCED state only — a transient fed>diffed during
 * in-flight processing is expected and is NOT a violation (see header
 * caveat). The returned bool is a snapshot, not a guarantee held across
 * the call. */
bool shadow_conservation_ok(unsigned long *fed,
                            unsigned long *diffed,
                            unsigned long *skipped);

/* Reset all counters to zero. Test-only seam so a test starting from a
 * clean ledger does not see counts leaked from earlier tests sharing the
 * process. Not called in production. */
void shadow_conservation_reset(void);

#endif /* ZCL_ADAPTERS_INBOUND_SHADOW_CONSERVATION_H */
