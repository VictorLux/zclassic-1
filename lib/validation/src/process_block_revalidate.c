/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * process_block_revalidate — see header for the verify-never-trust
 * contract. */

#include "validation/process_block_revalidate.h"

#include "chain/chain.h"
#include "core/uint256.h"
/* Wave M: the verify-never-trust contract for clearing BLOCK_FAILED_VALID
 * requires (a) ≥2-oracle consensus on the block hash and (b) routing the
 * post-clear connect through the activation controller's mutex. Both
 * services are app-layer concerns conceptually, but the actual nStatus
 * mutation + LevelDB persistence belong to validation. Splitting this
 * into two files across layers (a validation low-level primitive + an
 * app-services orchestrator) adds two files for no behavioral gain.
 * Tagged so the lib_layering gate doesn't have to grow its baseline. */
#include "services/chain_activation_controller.h"  // lib-layer-ok:wave-m-revalidate
#include "services/quorum_oracle_service.h"        // lib-layer-ok:wave-m-revalidate
#include "storage/block_index_db.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The block_tree_db handle is owned by config/src/boot.c and exposed as
 * a process-wide pointer used by connect_tip and friends. We reuse the
 * same handle here so cleared status updates land in the same LevelDB
 * the rest of the validation path persists to. */
extern struct block_tree_db *g_active_block_tree;

const char *reval_result_name(enum reval_result r)
{
    switch (r) {
        case REVAL_NOT_ATTEMPTED:        return "not_attempted";
        case REVAL_NO_FAILURE:           return "no_failure";
        case REVAL_HEIGHT_NOT_FOUND:     return "height_not_found";
        case REVAL_EVIDENCE_INSUFFICIENT:return "evidence_insufficient";
        case REVAL_EVIDENCE_DISAGREES:   return "evidence_disagrees";
        case REVAL_PERSIST_FAILED:       return "persist_failed";
        case REVAL_CONNECT_FAILED:       return "connect_failed";
        case REVAL_RECOVERED:            return "recovered";
    }
    return "?";
}

/* Iterate block_map looking for a pindex at exactly `target_height` that
 * carries BLOCK_FAILED_VALID. Returns NULL if no such entry exists.
 *
 * Multiple pindex entries can share a height (forks). We prefer the
 * entry whose immediate ancestry chains back through the active chain
 * tip, but the simpler "first FAILED_VALID match" is sufficient for the
 * wedge case where the active chain is stuck and the failed block is
 * the only obstruction. */
static struct block_index *find_failed_pindex_at_height(
    struct main_state *ms, int target_height)
{
    if (!ms) return NULL;
    size_t iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p) continue;
        if (p->nHeight != target_height) continue;
        if (!(p->nStatus & BLOCK_FAILED_VALID)) continue;
        return p;
    }
    return NULL;
}

/* Build a disk_block_index snapshot from an in-memory pindex so we can
 * persist a status-only update. Caller has already mutated pindex
 * nStatus; this copies the current state into the disk format.
 *
 * Mirrors the field-by-field copy in connect_tip.c:771-791 — when
 * either side adds a field, both sides must update. */
static void dbi_snapshot_from_pindex(struct disk_block_index *dbi,
                                     const struct block_index *pindex)
{
    disk_block_index_init(dbi);
    if (pindex->pprev && pindex->pprev->phashBlock)
        dbi->hashPrev = *pindex->pprev->phashBlock;
    dbi->nHeight = pindex->nHeight;
    dbi->nStatus = pindex->nStatus;
    dbi->nTx = pindex->nTx;
    dbi->nFile = pindex->nFile;
    dbi->nDataPos = pindex->nDataPos;
    dbi->nUndoPos = pindex->nUndoPos;
    dbi->nCachedBranchId = pindex->nCachedBranchId;
    dbi->nVersion = pindex->nVersion;
    dbi->hashMerkleRoot = pindex->hashMerkleRoot;
    dbi->hashFinalSaplingRoot = pindex->hashFinalSaplingRoot;
    dbi->nTime = pindex->nTime;
    dbi->nBits = pindex->nBits;
    dbi->nNonce = pindex->nNonce;
    if (pindex->nSolution && pindex->nSolutionSize > 0)
        memcpy(dbi->nSolution, pindex->nSolution, pindex->nSolutionSize);
    dbi->nSolutionSize = pindex->nSolutionSize;
}

enum reval_result process_block_revalidate(int target_height,
                                            struct main_state *ms,
                                            struct uint256 *out_hash)
{
    if (out_hash) memset(out_hash, 0, sizeof(*out_hash));
    if (!ms) {
        return REVAL_NOT_ATTEMPTED;
    }
    if (target_height < 0) {
        return REVAL_NOT_ATTEMPTED;
    }

    /* ── Step 1: find the failed pindex at this height ───────────────── */
    struct block_index *failed_pindex =
        find_failed_pindex_at_height(ms, target_height);
    if (!failed_pindex) {
        /* Either there's no entry at this height, or none of the entries
         * have BLOCK_FAILED_VALID set. Either is a non-failure. */
        return REVAL_HEIGHT_NOT_FOUND;
    }
    if (failed_pindex->phashBlock && out_hash) {
        *out_hash = *failed_pindex->phashBlock;
    }

    /* If BLOCK_FAILED_VALID isn't actually set, return NO_FAILURE. The
     * find_failed_pindex_at_height already filters by VALID — this is a
     * belt-and-suspenders re-check for callers that pre-screen. */
    if (!(failed_pindex->nStatus & BLOCK_FAILED_VALID)) {
        return REVAL_NO_FAILURE;
    }

    /* ── Step 2: query the quorum oracle for evidence ────────────────── */
    struct quorum_oracle_result qr;
    memset(&qr, 0, sizeof(qr));
    bool probed = quorum_oracle_probe(target_height, &qr);
    if (!probed) {
        fprintf(stderr,  // obs-ok:revalidate-probe-failure
                "[revalidate] h=%d: quorum_oracle_probe returned false; "
                "leaving FAILED set\n", target_height);
        return REVAL_EVIDENCE_INSUFFICIENT;
    }
    if (qr.verdict != QO_VERDICT_QUORUM_MATCH) {
        fprintf(stderr,  // obs-ok:revalidate-no-quorum
                "[revalidate] h=%d: no quorum (verdict=%d agreeing=%d); "
                "leaving FAILED set\n",
                target_height, (int)qr.verdict, qr.agreeing_sources);
        return REVAL_EVIDENCE_INSUFFICIENT;
    }

    /* ── Step 3: verify the agreed hash matches our pindex ───────────── */
    char our_hash_hex[65];
    our_hash_hex[0] = '\0';
    if (failed_pindex->phashBlock) {
        uint256_get_hex(failed_pindex->phashBlock, our_hash_hex);
    }
    if (our_hash_hex[0] == '\0' ||
        strcasecmp(our_hash_hex, qr.winning_hash_hex) != 0) {
        fprintf(stderr,  // obs-ok:revalidate-disagreement
                "[revalidate] h=%d: oracle agreed on %s but our FAILED "
                "pindex is %s — we're on a fork; leaving FAILED set so "
                "chain selection reorgs through a different mechanism\n",
                target_height, qr.winning_hash_hex,
                our_hash_hex[0] ? our_hash_hex : "(no hash)");
        return REVAL_EVIDENCE_DISAGREES;
    }

    /* ── Step 4: ≥2-oracle verified the same hash. Safe to clear. ────── */
    /* Clear BLOCK_FAILED bits on this pindex AND every descendant above
     * the current active tip. Uses the same shape as
     * chain_restore_clear_failed_above_tip in chain_restore_repair.c —
     * proven safe when there's evidence the canonical chain runs
     * through the cleared blocks. The evidence here is the quorum
     * agreement on the target_height block; descendants will be
     * re-validated by connect_tip on the next chain advance and
     * re-marked FAILED individually if any are genuinely invalid. */
    int tip_h = active_chain_height(&ms->chain_active);
    int cleared = 0;
    int persisted = 0;
    int persist_errors = 0;
    size_t iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p) continue;
        /* Only clear at-or-above the failed height. Below the active
         * tip we must not touch — those are durable history. */
        if (p->nHeight < target_height) continue;
        if (p->nHeight <= tip_h && p != failed_pindex) continue;
        unsigned was_failed = p->nStatus & BLOCK_FAILED_MASK;
        if (!was_failed) continue;
        p->nStatus &= ~(unsigned)BLOCK_FAILED_MASK;
        cleared++;
        /* Persist the status-only update. We use the async write
         * because we'll have many of these on a deeply-wedged chain;
         * one final fsync via the supervisor's natural tick suffices.
         * If persistence fails for one entry we count and continue —
         * worst case is the next boot re-reads FAILED for that entry
         * and the revalidate fires again on the next 900s tick. */
        if (g_active_block_tree) {
            struct disk_block_index dbi;
            dbi_snapshot_from_pindex(&dbi, p);
            if (block_tree_db_write_block_index(g_active_block_tree, &dbi)) {
                persisted++;
            } else {
                persist_errors++;
            }
        }
    }

    fprintf(stderr,  // obs-ok:revalidate-cleared
            "[revalidate] h=%d: oracle-verified; cleared %d FAILED entries "
            "(%d persisted, %d errors)\n",
            target_height, cleared, persisted, persist_errors);

    if (cleared > 0 && persisted == 0 && persist_errors > 0) {
        /* Nothing persisted — won't survive a crash. Treat as a failure. */
        return REVAL_PERSIST_FAILED;
    }

    /* ── Step 5: trigger activation. ─────────────────────────────────── */
    /* The activation controller serializes on its own mutex; we call it
     * with NULL block (no specific block to connect — just kick the
     * activation loop to re-evaluate find_most_work_chain with our
     * newly cleared entries). */
    struct chain_activation_controller *ctl = boot_activation_controller();
    if (!ctl) {
        fprintf(stderr,  // obs-ok:revalidate-no-controller
                "[revalidate] h=%d: no activation controller; cannot "
                "trigger connect — chain will advance on next natural "
                "activation kick\n", target_height);
        /* Still return RECOVERED — we cleared the gate, and the next
         * P2P/watchdog tick will invoke activation naturally. */
        return REVAL_RECOVERED;
    }

    enum activation_state s = activation_get_state(ctl);
    if (s != ACTIVATION_READY && s != ACTIVATION_AT_TIP) {
        fprintf(stderr,  // obs-ok:revalidate-not-ready
                "[revalidate] h=%d: activation state=%s not ready for "
                "kick; leaving cleared and returning RECOVERED — natural "
                "tick will pick up cleared blocks\n",
                target_height, activation_state_name(s));
        return REVAL_RECOVERED;
    }

    struct activation_exec_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    activation_request_connect(ctl, ACTIVATION_SRC_REVALIDATE,
                               NULL, &outcome);

    /* Inspect: did the chain actually advance? */
    int new_tip_h = active_chain_height(&ms->chain_active);
    if (new_tip_h > tip_h) {
        fprintf(stderr,  // obs-ok:revalidate-success
                "[revalidate] h=%d: chain advanced %d → %d after "
                "revalidation\n", target_height, tip_h, new_tip_h);
        return REVAL_RECOVERED;
    }

    /* Activation ran but tip didn't advance. Either connect_block
     * re-marked FAILED on our cleared block (still genuinely invalid)
     * or some other condition blocked. Re-check the original pindex. */
    if (failed_pindex->nStatus & BLOCK_FAILED_VALID) {
        fprintf(stderr,  // obs-ok:revalidate-remarked
                "[revalidate] h=%d: cleared, retried, connect_block "
                "re-marked FAILED; block is genuinely invalid (or "
                "transient resource failure). Next 900s tick will retry.\n",
                target_height);
        return REVAL_CONNECT_FAILED;
    }

    /* Cleared, activation ran, tip didn't move, but pindex isn't
     * re-marked FAILED. This usually means activation skipped (state
     * transitions, in-flight work, etc.). Treat as RECOVERED — the
     * gate is open, future ticks will advance. */
    fprintf(stderr,  // obs-ok:revalidate-cleared-no-advance
            "[revalidate] h=%d: cleared and persisted; activation kick "
            "didn't advance tip this cycle (result=%d). Next tick will "
            "pick it up.\n", target_height, (int)outcome.result);
    return REVAL_RECOVERED;
}
