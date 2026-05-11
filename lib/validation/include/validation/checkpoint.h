/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * checkpoint — hard reorg-depth invariant.
 *
 * MAX_REORG_LENGTH (= COINBASE_MATURITY - 1 = 99) is the protocol
 * promise: any block more than 99 deep from the tip is permanently
 * immutable. The chain's economics depend on this — coinbase outputs
 * can be spent after 100 confirmations precisely because reorgs past
 * that depth are considered impossible.
 *
 * Before this module, the invariant was checked only at one point
 * (`process_block.c:3494`, in the extending-reorg branch). The
 * fork-point walks at lines 3413/3435/etc. could legitimately try to
 * walk past it, then the cycle guards (Round 3 Part O) would abort
 * the walk after the damage was done.
 *
 * `reorg_is_allowed` is the single helper that callers query BEFORE
 * starting any walk. If the target fork is below
 * `tip - MAX_REORG_LENGTH`, the answer is no and the caller refuses
 * the operation — saving the wasted walk, removing one cause of
 * silent CPU stalls, and giving the operator a clear log line. */

#ifndef ZCL_VALIDATION_CHECKPOINT_H
#define ZCL_VALIDATION_CHECKPOINT_H

#include <stdbool.h>

/* Return true iff a reorg from `tip_h` whose deepest disconnected
 * block is at `target_fork_h` is permitted. `target_fork_h` is the
 * HEIGHT of the fork point — the highest block both chains share.
 * The disconnected depth is therefore `tip_h - target_fork_h`.
 *
 * If false, `*reason_out` (if non-NULL) is set to a static string
 * describing why. The pointer is owned by this module and must not
 * be freed by the caller.
 *
 * Semantics:
 *   tip_h - target_fork_h <= MAX_REORG_LENGTH → allowed
 *   tip_h - target_fork_h >  MAX_REORG_LENGTH → refused
 *
 * The function does not log — callers decide whether to emit a
 * stderr line or an event when refusing. */
bool reorg_is_allowed(int tip_h, int target_fork_h,
                      const char **reason_out);

/* Convenience: returns true iff `h` is below the "immutable floor"
 * tip_h - MAX_REORG_LENGTH. Useful for background validators that
 * want to skip re-verifying historic blocks. */
bool height_is_immutable(int tip_h, int h);

#endif /* ZCL_VALIDATION_CHECKPOINT_H */
