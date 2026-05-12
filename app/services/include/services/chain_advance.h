/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chain_advance — atomic per-block chain advance.
 *
 * One call advances the chain by one block with all on-disk state
 * consistent. Crash at any point during the call leaves the on-disk
 * state semantically equivalent to "before the call started":
 *
 *   - block N+1's UTXO deltas are either fully applied to coins.db
 *     or fully absent
 *   - block N+1's sapling tree update is either fully persisted to
 *     node.db or fully absent
 *   - block N+1's block_index status flags are either updated or not
 *   - the in-memory chain tip (active_chain) is committed via the
 *     chain_state_repository only AFTER both disk writes succeed
 *
 * Today's flow (lib/validation/src/process_block.c:connect_tip) does
 * each of these steps in implicit autocommit transactions on two
 * separate SQLite connections (coins.db dedicated handle from P14.1,
 * node.db main handle). The coins write is already atomic within
 * itself (BEGIN IMMEDIATE / COMMIT in coins_view_sqlite.c:667), but
 * the node_db side runs three or four loose writes with no enclosing
 * transaction. A crash mid-flow leaves the chain in one of several
 * "almost advanced" states that the current self-healing UTXO scan,
 * placeholder-block guard, chain_restore off-chain demotion, and the
 * 150k-block backward search exist to paper over.
 *
 * After this contract is implemented and connect_tip is converted
 * to a thin wrapper, those reactive guards become unreachable and
 * are deleted. See the Move 2 section of
 * /home/rhett/.claude/plans/make-a-full-detailed-nifty-melody.md
 * for the deletion plan.
 *
 * ── Contract ──────────────────────────────────────────────
 *
 * Postconditions on CA_OK:
 *   1. coins.db: hash_block == new_tip->hash and all of block N+1's
 *      UTXO deltas are written and fsynced
 *   2. node.db: sapling_tree state row reflects block N+1's tree,
 *      block_index row for new_tip has BLOCK_VALID_CHAIN status,
 *      and the chain tip row is consistent with the active_chain
 *      tip via csr_commit_tip
 *   3. csr_commit_tip returned CSR_OK
 *
 * Postconditions on any non-OK return:
 *   - on-disk state == on-disk state at entry. The coins handle may
 *     have committed (if the failure happened after step 3), in
 *     which case boot will forward-roll the node_db to match via
 *     the existing utxo_recovery_service. The in-memory chain tip
 *     is NOT mutated.
 *
 * Ordering rule (the key invariant):
 *   coins-flush COMMITs BEFORE node_db COMMIT. On a crash where coins
 *   has committed but node_db has not, recovery walks forward; on a
 *   crash where node_db has committed but coins has not, that state
 *   is unreachable because we ordered them.
 *
 * Caller obligations:
 *   - `new_tip` must already be registered in the in-memory block_map
 *     (the same precondition csr_commit_tip requires)
 *   - `pblock`'s txns must be the canonical txns for `new_tip->hash`
 *     (caller is expected to have validated this; chain_advance does
 *     not re-validate the block hash against block data)
 *   - All inputs non-NULL except `pblock` (NULL means "read from disk")
 */

#ifndef ZCL_SERVICES_CHAIN_ADVANCE_H
#define ZCL_SERVICES_CHAIN_ADVANCE_H

struct main_state;
struct coins_view_cache;
struct block_index;
struct block;
struct chain_params;
struct validation_state;

enum chain_advance_result {
    CA_OK = 0,
    CA_REJECTED_VALIDATION,    /* connect_block returned false; reason is
                                * set on `state` */
    CA_REJECTED_CSR,           /* csr_commit_tip returned non-OK */
    CA_FAILED_DISK_WRITE,      /* node_db / coins flush failed; rollback
                                * applied to node_db. coins.db may or may
                                * not have committed — see utxo_recovery_
                                * service for the forward-roll path */
    CA_FAILED_ROLLBACK,        /* unrecoverable: ROLLBACK itself failed.
                                * Caller should treat this as fatal and
                                * exit so the node can boot fresh */
    CA_NUM_RESULTS             /* sentinel */
};

const char *chain_advance_result_name(enum chain_advance_result r);

/* Apply one block on top of the active chain. See file header for the
 * full contract.
 *
 * `state`     — receives validation error details on CA_REJECTED_VALIDATION
 * `ms`        — main_state (active_chain, pindex_best_header, etc.)
 * `coins_tip` — coins cache. chain_advance flushes this on success.
 * `new_tip`   — block_index entry for the block being applied; must
 *               already be in ms->block_map
 * `pblock`    — block data; NULL means read from disk via datadir
 * `params`    — consensus params for connect_block
 * `datadir`   — data directory root for block file reads
 * `reason`    — short human-readable string for the CSR commit + event
 *               log (e.g. "process_block.connect_tip"); must be non-NULL
 *
 * Returns CA_OK on full success; any other value means no on-disk
 * mutation persisted (modulo the coins/ndb ordering noted above).
 */
enum chain_advance_result
chain_advance(struct validation_state *state,
              struct main_state *ms,
              struct coins_view_cache *coins_tip,
              struct block_index *new_tip,
              struct block *pblock,
              const struct chain_params *params,
              const char *datadir,
              const char *reason);

#endif /* ZCL_SERVICES_CHAIN_ADVANCE_H */
