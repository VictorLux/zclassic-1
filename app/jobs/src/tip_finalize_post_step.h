/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_post_step — STEP 5 of the reducer-as-ingest design.
 *
 * The post-finalize side-effect step: once tip_finalize has physically
 * advanced the in-mem tip (step 1's active_chain_set_tip), this runs the
 * derived effects the legacy connect_tip path performs at tip-connect —
 * wallet transaction sync + Sapling trial-decrypt/note-persist, nullifier
 * spend marking, mempool removal of confirmed txs, and the MMR/MMB appends.
 *
 * AUTHORITATIVE-only by contract: the sole caller (tip_finalize_stage.c)
 * invokes this only from its AUTHORITATIVE branch, so under the live
 * default (SHADOW) it is never reached and legacy connect_tip remains the
 * sole producer of these effects — DORMANT, no live behaviour change.
 *
 * Split out of tip_finalize_stage.c to keep that file under the E1 800-LOC
 * ceiling (the lifted block carries the heavy wallet/mempool/controller
 * includes). Internal to app/jobs/src — not a public jobs/ API. */

#ifndef ZCL_JOBS_TIP_FINALIZE_POST_STEP_H
#define ZCL_JOBS_TIP_FINALIZE_POST_STEP_H

struct block_index;

/* Run the post-finalize side effects for the just-connected tip block.
 *
 * `pindex_new` is the block_index of the newly finalized tip (already set
 * as chain[] tip by the caller). The block body is read back from disk via
 * GetDataDir() (resolved here, not threaded from the stage). NULL
 * pindex_new is a no-op; a missing on-disk body (HAVE_DATA absent / read
 * failure) is a benign skip.
 *
 * Every subsystem handle (wallet, mempool, node_db) is fetched via the
 * public app_runtime_* accessors and individually NULL-guarded, matching
 * the legacy connect_tip guards exactly. */
void tip_finalize_run_post_finalize(struct block_index *pindex_new);

#endif /* ZCL_JOBS_TIP_FINALIZE_POST_STEP_H */
