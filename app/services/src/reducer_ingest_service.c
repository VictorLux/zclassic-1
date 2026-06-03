/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reducer-ingest service — the synchronous block-intake path that drives the
 * staged Job pipeline as the authoritative chain-advance engine.
 *
 * This is the single responsibility split out of chain_activation_service.c
 * (which keeps the activation state machine). The reducer wrapper drives the
 * eight staged-Job step bodies and the stateless check_block gate, then reads
 * back the verdict from the freshly-written stage log rows. Public entry
 * points (reducer_is_authoritative / reducer_kick / reducer_ingest_block) are
 * declared in services/chain_activation_service.h and keep identical
 * names/signatures; the activation FSM shares only reducer_drain_to_convergence
 * via the private services/reducer_ingest_service.h seam. */

// one-result-type-ok:reducer-drive-counts
/* The reducer entry points return advance-counts / authority bools; a
 * failure surfaces via the stage FATAL latch + EV_OPERATOR_NEEDED, not a
 * return-value reason (same rationale as the parent chain_activation_service.c). */

#include "services/chain_activation_service.h"
#include "services/reducer_ingest_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "event/event.h"
#include "core/utiltime.h"
#include <stdio.h>
#include <string.h>

#include "util/log_macros.h"

/* ── Reducer-as-ingest includes ─────────────────────────────────────
 * The synchronous reducer wrapper drives the staged Job pipeline and the
 * stateless check_block gate, then reads back the verdict from the
 * freshly-written stage log rows. */
#include "consensus/validation.h"
#include "validation/check_block.h"
#include "primitives/block.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "storage/progress_store.h"
#include "storage/disk_block_io.h"
#include "services/header_admit_inbox.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/block_header_emit.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"

/* ── Reducer-as-ingest: synchronous wrapper driving the staged Job pipeline.
 * Drain the eight stage step bodies once, in pipeline order — the SAME
 * *_stage_drain functions the per-stage supervisor children tick
 * (staged_sync_supervisor.c). One pass; caller loops to convergence. */
static int reducer_drain_all_stages(int max_steps_per_stage)
{
    int advanced = 0;
    advanced += header_admit_stage_drain(max_steps_per_stage);
    advanced += validate_headers_stage_drain(max_steps_per_stage);
    advanced += body_fetch_stage_drain(max_steps_per_stage);
    advanced += body_persist_stage_drain(max_steps_per_stage);
    advanced += script_validate_stage_drain(max_steps_per_stage);
    advanced += proof_validate_stage_drain(max_steps_per_stage);
    advanced += utxo_apply_stage_drain(max_steps_per_stage);
    advanced += tip_finalize_stage_drain(max_steps_per_stage);
    return advanced;
}

/* Loop reducer_drain_all_stages to convergence within a bounded latency
 * budget. A no-advance pass is convergence UNLESS a stage went FATAL this
 * pass (fatal_generation moved) — a wedged stage masquerading as idle, so
 * we page EV_OPERATOR_NEEDED before breaking. */
int reducer_drain_to_convergence(void)
{
    const int64_t drain_budget_us = 2000 * 1000; /* 2s, same as legacy */
    const int     drain_hard_cap  = 4096;
    const int     per_stage_batch = 100;
    int64_t       start_us        = GetTimeMicros();
    uint64_t      fatal_gen0      = stage_fatal_generation();
    int           total           = 0;
    for (int round = 0; round < drain_hard_cap; round++) {
        int adv = reducer_drain_all_stages(per_stage_batch);
        total += adv;
        if (adv == 0) {
            char st[STAGE_NAME_MAX] = {0}, why[128] = {0};
            if (stage_fatal_generation() != fatal_gen0 &&
                stage_last_fatal(st, sizeof(st), why, sizeof(why)))
                event_emitf(EV_OPERATOR_NEEDED, 0,
                            "condition=reducer_stage_fatal stage=%s reason=%s",
                            st, why);
            break;
        }
        if (GetTimeMicros() - start_us > drain_budget_us)
            break;
    }
    return total;
}

bool reducer_is_authoritative(void)
{
    return true;
}

int reducer_kick(struct chain_activation_controller *ctl)
{
    if (!ctl)
        return 0;
    zcl_mutex_lock(&ctl->mutex);
    int advanced = reducer_drain_to_convergence();
    zcl_mutex_unlock(&ctl->mutex);
    return advanced;
}

/* Map freshly-written stage log rows for `height`/`hash` into `out`. */
static bool reducer_read_back_verdict(int height,
                                      const struct uint256 *hash,
                                      struct validation_state *out)
{
    sqlite3 *pdb = progress_store_db();

    struct validate_headers_window_report rep;
    if (validate_headers_stage_window_report(height, height, &rep) &&
        rep.failed_count > 0) {
        validation_state_dos(out, 100, false, REJECT_INVALID,
                             rep.first_fail_reason[0]
                                 ? rep.first_fail_reason
                                 : "header-validation-failed",
                             false, NULL);
        return false;
    }

    uint8_t finalized[32];
    if (pdb &&
        tip_finalize_stage_finalized_tip_at(pdb, height, finalized) &&
        memcmp(finalized, hash->data, 32) == 0) {
        return true; /* out left MODE_VALID by the caller's init */
    }

    validation_state_invalid(out, false, REJECT_INVALID,
                             "block-not-finalized-by-reducer", NULL);
    return false;
}

static bool reducer_header_rejected_at(int height, struct validation_state *out)
{
    struct validate_headers_window_report rep;
    if (!validate_headers_stage_window_report(height, height, &rep) ||
        rep.failed_count == 0)
        return false;

    validation_state_dos(out, 100, false, REJECT_INVALID,
                         rep.first_fail_reason[0]
                             ? rep.first_fail_reason
                             : "header-validation-failed",
                         false, NULL);
    return true;
}

static bool reducer_pending_body_is_accepted(
        const struct block_index *bi,
        struct validation_state *out)
{
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA) ||
        (bi->nStatus & BLOCK_FAILED_MASK))
        return false;

    if (reducer_header_rejected_at(bi->nHeight, out))
        return false;

    /* Consensus gate: the live tip is accepted ONLY if it cleared utxo_apply
     * (HAVE_DATA && !FAILED is no witness — stage fails record ok=0, never
     * BLOCK_FAILED_MASK). Caller already confirmed bi IS the active tip. */
    if (!utxo_apply_stage_succeeded_at(bi->nHeight))
        return false;

    validation_state_init(out);
    return true;
}

static bool reducer_persist_ingested_body_locked(
        struct chain_activation_controller *ctl,
        const struct uint256 *block_hash,
        struct block *pblock,
        struct validation_state *out)
{
    if (!ctl || !ctl->ms || !block_hash || !pblock) {
        LOG_WARN("reducer", "body persist invalid args ctl=%p ms=%p hash=%p block=%p",
                 (void *)ctl, ctl ? (void *)ctl->ms : NULL,
                 (const void *)block_hash, (void *)pblock);
        return validation_state_error(out, "reducer-body-null-arg");
    }

    struct block_index *bi = block_map_find(&ctl->ms->map_block_index,
                                            block_hash);
    if (!bi)
        return true;

    if (bi->nStatus & BLOCK_HAVE_DATA)
        return true;

    if (reducer_header_rejected_at(bi->nHeight, out))
        return false;

    if (!ctl->datadir || !ctl->datadir[0] || !ctl->params) {
        LOG_WARN("reducer", "body persist missing runtime wiring h=%d datadir=%p params=%p",
                 bi->nHeight, (const void *)ctl->datadir,
                 (const void *)ctl->params);
        return validation_state_error(out, "reducer-body-runtime-unwired");
    }

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    if (!write_block_to_disk(pblock, &pos, ctl->datadir,
                             ctl->params->pchMessageStart)) {
        LOG_WARN("reducer", "body persist write failed h=%d", bi->nHeight);
        return validation_state_error(out, "reducer-body-write-failed");
    }

    if (!block_index_set_have_data_verified(bi, &pos, ctl->datadir)) {
        LOG_WARN("reducer", "body persist verify failed h=%d file=%d pos=%u",
                 bi->nHeight, pos.nFile, pos.nPos);
        return validation_state_error(out, "reducer-body-verify-failed");
    }

    block_index_emit_header_event(bi, "reducer_ingest", NULL, NULL);
    LOG_INFO("reducer", "persisted ingested block body h=%d file=%d pos=%u",
             bi->nHeight, pos.nFile, pos.nPos);
    return true;
}

bool reducer_ingest_block(struct chain_activation_controller *ctl,
                          struct block *pblock,
                          enum reducer_source source,
                          bool force,
                          struct validation_state *out)
{
    (void)source; /* informational; `force` carries the live semantics */

    if (!out)
        return false;
    validation_state_init(out);

    if (!ctl || !pblock)
        return validation_state_error(out, "reducer-null-arg");

    /* (1) Stateless gate FIRST, inline, BEFORE any log/stage mutation. A
     * garbage block is rejected with the verdict already in `out`; nothing is
     * admitted to the inbox. The `force`/requested flag does not relax the
     * stateless checks; it gates the relay pre-filters inside the admit
     * producer path, not this gate. */
    if (!check_block(pblock, out, ctl->params, true, true, true)) {
        LOG_FAIL("reducer", "check_block failed: %s",
                 out->reject_reason[0] ? out->reject_reason : "unknown");
        return false;
    }

    /* (2) Push the header + raw bytes into the header_admit_inbox so the
     * producer path (step 2) can CREATE the block_index entry
     * without legacy accept_block_header. Hash-hint is the block hash. */
    struct uint256 block_hash;
    block_get_hash(pblock, &block_hash);

    struct header_admit_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.hash = block_hash;
    msg.observed_unix = (int64_t)GetTime();
    msg.has_header = true;
    msg.header = pblock->header;
    /* height hint: one past the prev block's height when known, else -1
     * (admit verifies against the active chain regardless of the hint). */
    msg.height = -1;
    if (!mailbox_header_admit_push(&msg)) {
        /* Inbox full: cannot admit this block right now. Not a consensus
         * reject — report a transient error so the caller can retry. */
        return validation_state_error(out, "header-admit-inbox-full");
    }

    /* (3) Drain the eight stage step bodies synchronously under the SAME
     * mutex reducer activation serializes on, ahead of the 2s supervisor
     * tickers, so a single at-tip block reaches tip_finalize within the
     * call. The reorg disconnect (step 4) is driven from inside utxo_apply
     * when a better fork is selected. */
    zcl_mutex_lock(&ctl->mutex);
    (void)force; /* relay pre-filter gating lives in the admit producer */
    struct block_index *anchor_tip = active_chain_tip(&ctl->ms->chain_active);
    if (anchor_tip && anchor_tip->phashBlock &&
        tip_finalize_stage_cursor() < (uint64_t)anchor_tip->nHeight + 1u)
        (void)tip_finalize_stage_seed_anchor(anchor_tip->nHeight,
                                             anchor_tip->phashBlock->data);
    (void)reducer_drain_to_convergence();
    if (!reducer_persist_ingested_body_locked(ctl, &block_hash, pblock, out)) {
        zcl_mutex_unlock(&ctl->mutex);
        return false;
    }
    (void)reducer_drain_to_convergence();

    struct block_index *ingested =
        block_map_find(&ctl->ms->map_block_index, &block_hash);

    /* Prefer the just-ingested height for the read-back. The active tip may
     * still be one block behind while tip_finalize waits for lookahead, but
     * header/stateful rejects are recorded at the ingested height. */
    struct block_index *tip = active_chain_tip(&ctl->ms->chain_active);
    int target_h = ingested ? ingested->nHeight : (tip ? tip->nHeight : 0);
    zcl_mutex_unlock(&ctl->mutex);

    /* (4) Read back the verdict from the freshly-written log rows. */
    if (reducer_read_back_verdict(target_h, &block_hash, out))
        return true;

    /* Pending fallback: accept ONLY the live active tip (ingested == tip,
     * snapshotted under the lock) — a fork can't borrow another block's row. */
    if (ingested && ingested == tip &&
        reducer_pending_body_is_accepted(ingested, out))
        return true;

    return false;
}
