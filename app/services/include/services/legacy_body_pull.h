/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Legacy body pull — durable backstop for the "tip behind zclassicd"
 * condition.
 *
 * When the local node's tip is behind the legacy zclassicd (sibling)
 * because some blocks past our active tip have no body data — for
 * any reason: K2 still ramping up, peer churn, a marked-invalid
 * subtree blocking the chain selector — this service walks pprev
 * from pindex_best_header down to from_height and, for every
 * block_index entry missing BLOCK_HAVE_DATA, RPC-fetches the block
 * from the legacy node and submits it via process_new_block(). The
 * accept_block + activate_best_chain pipeline does the rest.
 */

#ifndef ZCL_SERVICES_LEGACY_BODY_PULL_H
#define ZCL_SERVICES_LEGACY_BODY_PULL_H

#include <stdbool.h>

struct main_state;
struct coins_view_cache;
struct chain_params;

/* Pull all missing block bodies in [from_height .. to_height] from the
 * sibling zclassicd over loopback JSON-RPC, write them to disk, and
 * activate them onto the active chain. Returns true if every height
 * that has a block_index entry in the window now has BLOCK_HAVE_DATA;
 * false on RPC failure, validation failure, or shutdown. *out_applied
 * (may be NULL) receives the number of blocks newly written + accepted.
 *
 * to_height == -1 means "use pindex_best_header height".
 *
 * Requires:
 *  - ms->pindex_best_header populated up to (or past) to_height
 *    (header_probe must have run first; phase3_block_ingest's
 *    pre-pull already does this).
 *  - zclassicd reachable on 127.0.0.1 with credentials in
 *    ~/.zclassic/zclassic.conf (or via legacy_rpc env).
 *  - our_datadir writable for blocks/blkNNNNN.dat appends.
 */
bool legacy_body_pull_range_blocking(struct main_state *ms,
                                     struct coins_view_cache *coins_tip,
                                     const struct chain_params *params,
                                     const char *our_datadir,
                                     int from_height,
                                     int to_height,
                                     int *out_applied);

/* Same validation path as legacy_body_pull_range_blocking(), but tuned
 * for always-on mirror ticks: bounded caller-supplied ranges and no
 * expensive SHA3 window spotcheck on each small catch-up. The mirror
 * service performs active-chain hash anchors before/after the pull. */
bool legacy_body_pull_range_incremental(struct main_state *ms,
                                        struct coins_view_cache *coins_tip,
                                        const struct chain_params *params,
                                        const char *our_datadir,
                                        int from_height,
                                        int to_height,
                                        int *out_applied);

#endif /* ZCL_SERVICES_LEGACY_BODY_PULL_H */
