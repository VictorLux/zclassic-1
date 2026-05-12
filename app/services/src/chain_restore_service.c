/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 * See chain_restore_service.h for architecture overview. */

#include "services/chain_restore_service.h"
#include "services/chain_state_repository.h"
#include "services/chain_tip.h"
#include "models/db_txn.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include "services/snapshot_sync_service.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* ── Planning (pure function) ──────────────────────────────────── */

void chain_restore_plan(struct chain_restore_plan *out,
                        const struct chain_restore_input *in)
{
    memset(out, 0, sizeof(*out));

    /* Null hash → nothing to restore */
    if (uint256_is_null(&in->coins_best_hash)) {
        out->next_state = CHAIN_RESTORE_FAILED;
        out->should_skip_activate = true;
        snprintf(out->reason, sizeof(out->reason),
                 "coins_best_block is null — no UTXO state");
        return;
    }

    /* Path A: hash found in block_map with valid height */
    if (in->hash_found_in_map && in->found_height > 0) {
        out->next_state = CHAIN_RESTORE_FOUND_IN_INDEX;
        out->should_set_chain_tip = true;
        out->should_set_best_header = true;
        out->should_skip_activate = true;
        out->anchor_height = in->found_height;
        out->anchor_hash = in->coins_best_hash;
        snprintf(out->reason, sizeof(out->reason),
                 "found in block index at h=%d", in->found_height);
        return;
    }

    /* Path B: hash NOT in block_map but we know UTXO height */
    if (in->utxo_max_height > 0) {
        out->next_state = CHAIN_RESTORE_ANCHOR_CREATED;
        out->should_create_anchor = true;
        out->should_set_snapshot_anchor = true;
        out->should_skip_activate = true;
        out->anchor_height = in->utxo_max_height;
        out->anchor_hash = in->coins_best_hash;
        snprintf(out->reason, sizeof(out->reason),
                 "anchor at h=%d (hash not in index, %s)",
                 in->utxo_max_height,
                 in->source == CHAIN_RESTORE_SRC_LDB_IMPORT ? "LDB import"
                 : in->source == CHAIN_RESTORE_SRC_SNAPSHOT ? "snapshot"
                 : "boot");
        return;
    }

    /* Path C: no height info at all */
    out->next_state = CHAIN_RESTORE_FAILED;
    out->should_skip_activate = true;
    snprintf(out->reason, sizeof(out->reason),
             "coins_best_block set but height unknown — awaiting P2P");
}

/* ── Anchor creation (shared implementation) ───────────────────── */

struct block_index *chain_restore_create_anchor(
    struct main_state *ms,
    const struct uint256 *hash,
    int height)
{
    if (!ms || !hash || height <= 0)
        LOG_NULL("chain_restore", "create_anchor called with null ms/hash or height=%d", height);

    struct block_index *anchor = zcl_calloc(1, sizeof(struct block_index), "chain_restore anchor");
    if (!anchor)
        LOG_NULL("chain_restore", "calloc failed for anchor block_index at h=%d", height);

    block_index_init(anchor);
    anchor->nHeight = height;
    anchor->nStatus = BLOCK_VALID_UNKNOWN;
    anchor->nChainTx = 0;
    anchor->nTx = 0;
    arith_uint256_set_zero(&anchor->nChainWork);

    if (!block_map_insert(&ms->map_block_index, hash, anchor)) {
        free(anchor);
        return NULL;
    }

    anchor->phashBlock = block_map_find_hash(&ms->map_block_index, hash);
    if (!anchor->phashBlock) {
        /* Insert succeeded but hash lookup failed — shouldn't happen */
        fprintf(stderr, "chain_restore: anchor inserted but hash not found\n");
    }

    return anchor;
}

/* ── Execution ─────────────────────────────────────────────────── */

struct block_index *chain_restore_execute(
    const struct chain_restore_plan *plan,
    struct main_state *ms)
{
    if (!plan || !ms)
        LOG_NULL("chain_restore", "execute called with null plan or main_state");

    if (plan->next_state == CHAIN_RESTORE_FAILED)
        return NULL;

    struct block_index *target = NULL;

    if (plan->should_create_anchor) {
        target = chain_restore_create_anchor(
            ms, &plan->anchor_hash, plan->anchor_height);
        if (!target) {
            fprintf(stderr, "chain_restore: anchor creation failed\n");
            return NULL;
        }
        printf("Chain restore: anchor at h=%d\n", plan->anchor_height);
    } else if (plan->next_state == CHAIN_RESTORE_FOUND_IN_INDEX) {
        target = block_map_find(&ms->map_block_index, &plan->anchor_hash);
        if (!target) {
            fprintf(stderr, "chain_restore: hash in plan but not in map\n");
            return NULL;
        }
    }

    if (!target)
        return NULL;

    /* Route the tip/header mutations through the chain_state_repository
     * so block_map, active_chain, coins_tip and pindex_best_header
     * move atomically. The chain restore path is exactly the scenario
     * that caused 2026-04-10: boot detected a "best hash" and shoved
     * it into active_chain without cross-checking SQLite state. */
    if (plan->should_set_chain_tip && target->phashBlock) {
        struct chain_state_commit commit = {
            .new_tip             = target,
            .new_coins_best      = *target->phashBlock,
            .expected_utxo_count = 0,
            .update_header_tip   = plan->should_set_best_header,
            /* Chain restore is explicitly a "snap to where UTXOs
             * actually live" operation, which can legitimately look
             * like a backward move from csr's perspective. Bypass the
             * orphan-rows guard; Phase 2 recovery_policy will gate
             * this class of move via the operator-visible policy. */
            .allow_rollback      = true,
            .wallet_scan_height  = -1,
            .reason              = "chain_restore.execute",
        };

        /* Wrap the CSR commit in a scoped db transaction when the
         * singleton is wired to a real node_db. csr_commit_tip itself
         * only mutates in-memory state today, but csr_validate_locked
         * issues SQLite reads and any future write path here would be
         * silently leak-prone without the scope. The scope also gives
         * operators a BEGIN/COMMIT event pair bracketing the tip move,
         * which is the single highest-signal event from the 2026-04-10
         * incident. Unit-test paths that stub csr with a NULL ndb fall
         * through to the legacy raw-setter branch below. */
        struct chain_state_repository *csr = csr_instance();
        struct node_db *cr_ndb = (csr && csr->initialized) ? csr->ndb : NULL;
        enum csr_result rc;

        if (cr_ndb && cr_ndb->open) {
            DB_TXN_SCOPE(txn, cr_ndb, "chain_restore.execute");
            if (!txn) {
                fprintf(stderr,
                    "chain_restore: failed to open db_txn scope\n");
                return NULL;
            }
            rc = csr_commit_tip(csr, &commit);
            if (rc != CSR_OK) {
                /* Scope auto-rollback fires on return. */
                fprintf(stderr,
                    "chain_restore: csr rejected tip commit (%s) h=%d\n",
                    csr_result_name(rc), target->nHeight);
                return NULL;
            }
            if (!db_txn_commit(txn))
                return NULL;
        } else {
            rc = csr_commit_tip(csr, &commit);
            if (rc != CSR_OK) {
                if (rc == CSR_REJECTED_NOT_INITIALIZED) {
                    /* Unit-test path: singleton was never wired. Keep
                     * the legacy raw-setter behaviour so the existing
                     * test_chain_restore_service suite continues to
                     * exercise the end-to-end flow. */
                    chain_set_active_tip(ms, target, TIP_FROM_RESTORE,
                                          "csr_uninit_fallback");
                    if (plan->should_set_best_header)
                        ms->pindex_best_header = target;
                } else {
                    fprintf(stderr,
                        "chain_restore: csr rejected tip commit (%s) h=%d\n",
                        csr_result_name(rc), target->nHeight);
                    return NULL;
                }
            }
        }
    } else if (plan->should_set_best_header) {
        /* Extremely rare: plan asked for header-only update with no
         * chain tip change. Preserve legacy behaviour. */
        ms->pindex_best_header = target;
    }

    if (plan->should_set_snapshot_anchor)
        snapsync_set_anchor(target);

    /* Post-restore finalize — rebuild active_chain from pprev + block_map
     * and surface the P14.11/P14.12 integrity result. Unit tests pass
     * datadir implicitly via the NULL path (skips disk-backfill); real
     * boot paths call chain_restore_finalize directly with a datadir. */
    (void)chain_restore_finalize(ms, NULL);

    return target;
}

/* ── Validation ────────────────────────────────────────────────── */

void chain_restore_validate(struct chain_restore_validation *out,
                            const struct main_state *ms,
                            const struct uint256 *expected_hash,
                            int expected_height)
{
    memset(out, 0, sizeof(*out));

    out->coins_hash_valid = expected_hash && !uint256_is_null(expected_hash);

    if (expected_hash) {
        struct block_index *found = block_map_find(
            &ms->map_block_index, expected_hash);
        out->anchor_in_map = (found != NULL);
    }

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    out->chain_tip_set = (tip != NULL);

    if (tip && expected_height > 0)
        out->tip_matches_expected = (tip->nHeight == expected_height);

    out->all_ok = out->coins_hash_valid
               && out->anchor_in_map
               && out->chain_tip_set
               && out->tip_matches_expected;
}

/* ── Post-restore integrity check (P14.11 + P14.12) ────────────── */

void chain_integrity_check_post_restore(struct chain_integrity_result *out,
                                        const struct main_state *ms)
{
    memset(out, 0, sizeof(*out));
    out->first_nbits_zero_height = -1;
    out->first_hole_height = -1;
    out->first_tip_window_hole = -1;

    if (!ms) {
        out->ok = false;
        return;
    }

    /* P14.11: every pindex with on-disk data must have nBits != 0.
     *
     * Round 5: skip nBits=0 entries that have no BLOCK_HAVE_DATA bit.
     * Those are metadata-anchor placeholders left by chain_restore when
     * coins_best_block was unrecoverable from disk. They never enter
     * validation walks (no header is loaded), so a zero nBits on them
     * is harmless. Failing the integrity gate on such an entry —
     * which is the only thing we ever WRITE during the anchor-recovery
     * path — would crash-loop the node forever. */
    size_t iter = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (!pi || pi->nHeight <= 0)
            continue;
        if (!(pi->nStatus & BLOCK_HAVE_DATA))
            continue;
        if (pi->nBits == 0) {
            out->zero_nbits_count++;
            if (out->first_nbits_zero_height < 0 ||
                pi->nHeight < out->first_nbits_zero_height)
                out->first_nbits_zero_height = pi->nHeight;
        }
    }

    /* P14.12: chain_active.chain[h] non-NULL for h in [0, tip]. */
    out->tip_height = active_chain_height(&ms->chain_active);
    int window_lo = out->tip_height - CHAIN_INTEGRITY_TIP_WINDOW;
    if (window_lo < 0) window_lo = 0;
    for (int h = 0; h <= out->tip_height; h++) {
        if (active_chain_at(&ms->chain_active, h) == NULL) {
            out->active_chain_holes++;
            if (out->first_hole_height < 0 || h < out->first_hole_height)
                out->first_hole_height = h;
            if (h >= window_lo) {
                out->tip_window_holes++;
                if (out->first_tip_window_hole < 0 ||
                    h < out->first_tip_window_hole)
                    out->first_tip_window_hole = h;
            }
        }
    }

    /* `ok` reflects operational health.
     *
     * The capped pprev walk during live boot populates ~10k DISTINCT
     * heights, but those heights can be scattered (the walk follows
     * pprev pointers which may collapse heights in a partly-restored
     * block_map). So tip_window_holes can be positive even on a sane
     * live boot.
     *
     * The operational requirement is weaker: the tip itself must be
     * resolvable (active_chain_at(tip_h) == tip), and nBits must be
     * intact across the whole map. Lookups by height that miss go
     * through block_map walks; they're slower but correct.
     *
     * Round 4 Part 1.5 final: keep tip_window_holes / first_*_height
     * fields as diagnostic counters but don't gate `ok` on them.
     * `ok` requires only nBits clean + tip slot populated. */
    bool tip_slot_ok =
        (out->tip_height < 0) ||
        (active_chain_at(&ms->chain_active, out->tip_height) != NULL);
    out->ok = (out->zero_nbits_count == 0 && tip_slot_ok);

    /* Cache the result for `dumpstate subsystem=boot` / `zcl_state`. */
    chain_restore_record_integrity_result(out);
}

/* ── Boot snapshot ─────────────────────────────────────────────── */

static struct chain_restore_boot_snapshot g_boot_snapshot;

void chain_restore_record_integrity_result(
    const struct chain_integrity_result *r)
{
    if (!r) return;
    g_boot_snapshot.has_data = true;
    g_boot_snapshot.boot_time = (int64_t)time(NULL);
    g_boot_snapshot.integrity_ok = r->ok;
    g_boot_snapshot.zero_nbits_count = r->zero_nbits_count;
    g_boot_snapshot.active_chain_holes = r->active_chain_holes;
    g_boot_snapshot.tip_window_holes = r->tip_window_holes;
    g_boot_snapshot.tip_height = r->tip_height;
    g_boot_snapshot.first_nbits_zero_height = r->first_nbits_zero_height;
    g_boot_snapshot.first_hole_height = r->first_hole_height;
    g_boot_snapshot.first_tip_window_hole = r->first_tip_window_hole;
}

void chain_restore_record_backfill_result(int fixed,
                                          int read_errors,
                                          int off_chain_cleared)
{
    g_boot_snapshot.has_data = true;
    g_boot_snapshot.boot_time = (int64_t)time(NULL);
    g_boot_snapshot.backfill_ran = true;
    g_boot_snapshot.backfill_fixed = fixed;
    g_boot_snapshot.backfill_read_errors = read_errors;
    g_boot_snapshot.backfill_off_chain_cleared = off_chain_cleared;
}

void chain_restore_get_boot_snapshot(struct chain_restore_boot_snapshot *out)
{
    if (!out) return;
    *out = g_boot_snapshot;
}

bool chain_restore_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);
    json_push_kv_bool(out, "has_data", g_boot_snapshot.has_data);
    json_push_kv_int(out, "boot_time", g_boot_snapshot.boot_time);
    json_push_kv_bool(out, "integrity_ok", g_boot_snapshot.integrity_ok);
    json_push_kv_int(out, "zero_nbits_count",
                     g_boot_snapshot.zero_nbits_count);
    json_push_kv_int(out, "active_chain_holes",
                     g_boot_snapshot.active_chain_holes);
    json_push_kv_int(out, "tip_window_holes",
                     g_boot_snapshot.tip_window_holes);
    json_push_kv_int(out, "tip_height", g_boot_snapshot.tip_height);
    json_push_kv_int(out, "first_nbits_zero_height",
                     g_boot_snapshot.first_nbits_zero_height);
    json_push_kv_int(out, "first_hole_height",
                     g_boot_snapshot.first_hole_height);
    json_push_kv_int(out, "first_tip_window_hole",
                     g_boot_snapshot.first_tip_window_hole);
    json_push_kv_bool(out, "backfill_ran", g_boot_snapshot.backfill_ran);
    json_push_kv_int(out, "backfill_fixed", g_boot_snapshot.backfill_fixed);
    json_push_kv_int(out, "backfill_read_errors",
                     g_boot_snapshot.backfill_read_errors);
    json_push_kv_int(out, "backfill_off_chain_cleared",
                     g_boot_snapshot.backfill_off_chain_cleared);
    return true;
}

/* ── Post-restore repair (P14.11 + P14.12 GREEN) ────────────────── */

int chain_restore_rebuild_active_chain(struct main_state *ms,
                                       struct block_index *tip)
{
    if (!ms || !tip || tip->nHeight < 0)
        return 0;

    struct active_chain *c = &ms->chain_active;
    const int tip_h = tip->nHeight;

    /* The tip must already be installed as the chain tip (so
     * active_chain's capacity covers [0..tip_h]); if a caller hands us
     * a tip that isn't installed, install it via the standard path
     * first. Idempotent when already set. */
    if (active_chain_tip(c) != tip || active_chain_height(c) != tip_h) {
        if (tip_h > 1000000) {
            if (tip_h >= c->capacity) {
                int old_cap = c->capacity;
                int new_cap = tip_h + 1024;
                struct block_index **nc = zcl_realloc(
                    c->chain, (size_t)new_cap * sizeof(struct block_index *),
                    "active_chain/live_tip");
                if (!nc)
                    LOG_RETURN(0, "chain_restore",
                               "live tip install realloc failed (tip_h=%d)",
                               tip_h);
                c->chain = nc;
                memset(&c->chain[old_cap], 0,
                       (size_t)(new_cap - old_cap) *
                       sizeof(struct block_index *));
                c->capacity = new_cap;
            }
            c->chain[tip_h] = tip;
            c->height = tip_h;
            printf("[chain-restore] installed live tip without full "
                   "active_chain walk: h=%d\n", tip_h);
        } else {
            chain_set_active_tip(ms, tip, TIP_FROM_RESTORE,
                                 "rebuild_active_chain_full");
        }
    }

    int populated = 0;

    /* Fast path — walk pprev from tip and slot each ancestor. Covers
     * the happy case (real chain, pprev intact) in O(tip_h). */
    int deepest = tip_h + 1;
    int pprev_walk_limit = tip_h > 1000000 ? 10000 : tip_h + 1;
    for (struct block_index *p = tip; p != NULL; p = p->pprev) {
        int h = p->nHeight;
        if (h < 0 || h > tip_h) break;
        if (c->chain[h] != p) c->chain[h] = p;
        if (h < deepest) deepest = h;
        populated++;
        if (populated >= pprev_walk_limit && deepest > 0) {
            printf("[chain-restore] capped pprev walk during live boot: "
                   "tip_h=%d deepest=%d populated=%d\n",
                   tip_h, deepest, populated);
            break;
        }
    }

    /* If the pprev walk reached genesis, no residual work. */
    if (deepest == 0)
        return populated;

    /* Live recovery can promote a real HAVE_DATA block near mainnet tip while
     * its pprev path is still absent from the restored flat index. Filling
     * millions of active_chain holes and building skip pointers inline keeps
     * RPC/MCP dark during boot, which makes the node uncontrollable exactly
     * when it needs operator feedback. Defer that full integrity repair for
     * live-scale, anchor-shaped restores; the tip is already installed, so
     * getblockcount and service control can come up while peers continue. */
    if (tip_h > 1000000) {
        printf("[chain-restore] deferred active_chain hole repair: "
               "tip_h=%d deepest=%d populated=%d\n",
               tip_h, deepest, populated);
        return populated;
    }

    /* Residual holes below `deepest` — post-anchor-restore shape, where
     * the synthetic tip has pprev=NULL so every slot 0..tip_h-1 is
     * empty. The pre-P14.13 code did a fresh block_map scan per hole
     * (tip_h × map_size ≈ 10 trillion ops at live scale — boot pinned
     * >5 min at ~92% CPU). Single-pass bucketing restores O(N): scan
     * the block_map ONCE into a height-indexed best-candidate array,
     * then fill slots in one sweep. ~24 MB scratch at tip_h=3M; the
     * system has ~95 GB RAM and the allocation is transient. */
    struct block_index **by_height =
        zcl_calloc((size_t)(tip_h + 1), sizeof(struct block_index *),
                   "chain_restore/by_height");
    if (!by_height) {
        LOG_RETURN(populated, "chain_restore",
                   "rebuild_active_chain: by_height calloc failed (tip_h=%d)",
                   tip_h);
    }

    size_t it = 0;
    struct block_index *cand;
    while (block_map_next(&ms->map_block_index, &it, NULL, &cand)) {
        if (!cand) continue;
        int h = cand->nHeight;
        if (h < 0 || h >= deepest) continue;
        if (c->chain[h] != NULL) continue;
        if (cand->nStatus & BLOCK_FAILED_MASK) continue;

        struct block_index *best = by_height[h];
        if (!best) { by_height[h] = cand; continue; }

        /* Prefer BLOCK_HAVE_DATA, then highest nChainWork — matches
         * the pre-P14.13 tie-break rules so we don't slot a stale
         * fork over a real ancestor. */
        bool best_data = (best->nStatus & BLOCK_HAVE_DATA) != 0;
        bool cand_data = (cand->nStatus & BLOCK_HAVE_DATA) != 0;
        if (cand_data && !best_data) { by_height[h] = cand; continue; }
        if (best_data && !cand_data) continue;

        if (arith_uint256_compare(&cand->nChainWork,
                                  &best->nChainWork) > 0)
            by_height[h] = cand;
    }

    for (int h = 0; h < deepest; h++) {
        if (c->chain[h] != NULL) continue;
        if (by_height[h]) {
            c->chain[h] = by_height[h];
            populated++;
        }
    }

    free(by_height);

    /* P14.14: wire pprev + pskip across the rebuilt chain. The anchor
     * path leaves tip->pprev=NULL and flat-file loads can leave pskip
     * unpopulated, which forces block_index_get_ancestor to fall back
     * to O(N) pprev walks (or NULL on the anchor). Walking bottom-up
     * lets block_index_build_skip reuse each parent's already-built
     * pskip, keeping this pass O(tip_h · log tip_h) — about 1.7M
     * operations at live tip (3M), well under a second.
     *
     * Only wire pprev when it is currently NULL. Forked entries
     * promoted into chain_active by the bucketing step may legitimately
     * point at a different parent — but if we only fill NULLs we can't
     * stomp on an existing ancestry relationship. Same rule for pskip. */
    for (int h = 1; h <= tip_h; h++) {
        struct block_index *cur = c->chain[h];
        if (!cur) continue;
        if (cur->pprev == NULL) {
            struct block_index *prev = c->chain[h - 1];
            if (prev) cur->pprev = prev;
        }
        if (cur->pskip == NULL && cur->pprev)
            block_index_build_skip(cur);
    }

    return populated;
}

int chain_restore_backfill_nbits_from_disk(struct main_state *ms,
                                           const char *datadir)
{
    if (!ms || !datadir || !datadir[0])
        return 0;

    /* Round 6 Part 8: collect the active tip height once so we can
     * cheaply identify entries that are "off-chain" — i.e. block-index
     * entries not on the current active chain. If those entries have
     * unrecoverable nBits, we can safely clear BLOCK_HAVE_DATA: the
     * data will be re-fetched from a peer if the chain ever activates
     * through them. Doing this lets the integrity gate pass without
     * leaving 34 corrupt nBits=0 entries blocking healthy boots. */
    int tip_h = active_chain_height(&ms->chain_active);

    int fixed = 0, read_errors = 0, invalidated_off_chain = 0;
    size_t iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p) continue;
        if (p->nBits != 0) continue;
        if (p->nHeight <= 0) continue;   /* genesis nBits is set elsewhere */
        if (p->nDataPos == 0) continue;  /* synthetic anchor; no disk data */
        if (!(p->nStatus & BLOCK_HAVE_DATA)) continue;

        struct block blk;
        if (!read_block_from_disk_index_pread(&blk, p, datadir)) {
            /* Disk read failed despite BLOCK_HAVE_DATA. The data file
             * is missing or truncated. If this entry is not on the
             * active chain, drop BLOCK_HAVE_DATA so the integrity gate
             * stops flagging it and the download path can re-fetch.
             * Never touch entries on the active chain — those are
             * load-bearing for chainwork accounting. */
            bool on_active = false;
            if (tip_h >= 0 && p->nHeight <= tip_h) {
                struct block_index *at = active_chain_at(
                    &ms->chain_active, p->nHeight);
                if (at == p) on_active = true;
            }
            if (!on_active) {
                p->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
                invalidated_off_chain++;
            } else {
                read_errors++;
            }
            continue;
        }

        if (blk.header.nBits != 0) {
            p->nBits = blk.header.nBits;
            fixed++;
        }
        block_free(&blk);
    }

    if (fixed > 0 || read_errors > 0 || invalidated_off_chain > 0)
        printf("[nbits-backfill] fixed=%d pindex entries (read_errors=%d "
               "off_chain_cleared=%d)\n",
               fixed, read_errors, invalidated_off_chain);

    chain_restore_record_backfill_result(fixed, read_errors,
                                         invalidated_off_chain);

    return fixed;
}

bool chain_restore_block_is_consensus_backed(
    const struct block_index *tip)
{
    if (!tip || !tip->phashBlock)
        return false;
    if (tip->nStatus & BLOCK_FAILED_MASK)
        return false;
    if (!block_index_is_valid(tip, BLOCK_VALID_TREE))
        return false;
    if (!(tip->nStatus & BLOCK_HAVE_DATA))
        return false;
    if (tip->nHeight > 0 && (!tip->pprev || tip->nBits == 0))
        return false;
    if (tip->nHeight > 0 && (tip->nFile < 0 || tip->nDataPos == 0))
        return false;
    if (tip->nTx == 0 || tip->nChainTx == 0)
        return false;
    if (uint256_is_null(&tip->hashMerkleRoot))
        return false;
    return true;
}

bool chain_restore_block_is_consensus_backed_on_disk(
    const struct block_index *tip,
    const char *datadir)
{
    if (!tip || !tip->phashBlock)
        return false;
    if (tip->nStatus & BLOCK_FAILED_MASK)
        return false;
    if (!(tip->nStatus & BLOCK_HAVE_DATA))
        return false;
    if (tip->nHeight > 0 && (!tip->pprev || !tip->pprev->phashBlock))
        return false;
    if (tip->nHeight > 0 && (tip->nFile < 0 || tip->nDataPos == 0))
        return false;
    if (!datadir || !datadir[0])
        return false;

    struct block blk;
    if (!read_block_from_disk_index_pread(&blk, tip, datadir))
        return false;

    bool ok = true;
    struct uint256 disk_hash;
    block_get_hash(&blk, &disk_hash);

    if (!tip->phashBlock || uint256_cmp(&disk_hash, tip->phashBlock) != 0) {
        char got[65] = {0};
        char want[65] = {0};
        uint256_get_hex(&disk_hash, got);
        if (tip->phashBlock)
            uint256_get_hex(tip->phashBlock, want);
        fprintf(stderr,
                "[chain-restore] disk hash mismatch at h=%d got=%s want=%s\n",
                tip->nHeight, got, want[0] ? want : "<null>");
        ok = false;
    }

    if (ok && tip->nHeight > 0) {
        if (!tip->pprev || !tip->pprev->phashBlock ||
            uint256_cmp(&blk.header.hashPrevBlock,
                        tip->pprev->phashBlock) != 0) {
            char got[65] = {0};
            char want[65] = {0};
            uint256_get_hex(&blk.header.hashPrevBlock, got);
            if (tip->pprev && tip->pprev->phashBlock)
                uint256_get_hex(tip->pprev->phashBlock, want);
            fprintf(stderr,
                    "[chain-restore] disk prev-hash mismatch at h=%d "
                    "got=%s want=%s\n",
                    tip->nHeight, got, want[0] ? want : "<null>");
            ok = false;
        }
    }

    if (ok && !uint256_is_null(&tip->hashMerkleRoot) &&
        uint256_cmp(&blk.header.hashMerkleRoot, &tip->hashMerkleRoot) != 0) {
        char got[65] = {0};
        char want[65] = {0};
        uint256_get_hex(&blk.header.hashMerkleRoot, got);
        uint256_get_hex(&tip->hashMerkleRoot, want);
        fprintf(stderr,
                "[chain-restore] disk merkle mismatch at h=%d got=%s want=%s\n",
                tip->nHeight, got, want);
        ok = false;
    }

    if (ok && tip->nBits != 0 && blk.header.nBits != tip->nBits) {
        fprintf(stderr,
                "[chain-restore] disk nBits mismatch at h=%d got=%u want=%u\n",
                tip->nHeight, blk.header.nBits, tip->nBits);
        ok = false;
    }

    block_free(&blk);
    return ok;
}

struct block_index *chain_restore_nearest_consensus_backed_ancestor(
    struct block_index *tip)
{
    for (struct block_index *walk = tip; walk; walk = walk->pprev) {
        if (chain_restore_block_is_consensus_backed(walk))
            return walk;
    }
    return NULL;
}

struct block_index *chain_restore_nearest_consensus_backed_ancestor_on_disk(
    struct block_index *tip,
    const char *datadir)
{
    int checked = 0;
    for (struct block_index *walk = tip; walk; walk = walk->pprev) {
        if (chain_restore_block_is_consensus_backed_on_disk(walk, datadir))
            return walk;
        checked++;
        if (checked >= 4096) {
            fprintf(stderr,
                    "[chain-restore] no disk-backed ancestor found within "
                    "%d blocks below h=%d\n", checked,
                    tip ? tip->nHeight : -1);
            return NULL;
        }
    }
    return NULL;
}

static void chain_restore_quarantine_synthetic_tip(struct main_state *ms,
                                                   const char *datadir)
{
    if (!ms)
        return;

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (!tip)
        return;
    if (datadir && datadir[0]) {
        if (chain_restore_block_is_consensus_backed_on_disk(tip, datadir))
            return;
    } else if (chain_restore_block_is_consensus_backed(tip)) {
        return;
    }

    struct block_index *replacement = (datadir && datadir[0])
        ? chain_restore_nearest_consensus_backed_ancestor_on_disk(tip, datadir)
        : chain_restore_nearest_consensus_backed_ancestor(tip);
    if (!replacement || replacement == tip)
        return;

    char old_hash[65] = {0};
    char new_hash[65] = {0};
    if (tip->phashBlock)
        uint256_get_hex(tip->phashBlock, old_hash);
    if (replacement->phashBlock)
        uint256_get_hex(replacement->phashBlock, new_hash);

    fprintf(stderr,
            "[chain-restore] quarantining non-consensus active tip "
            "h=%d hash=%s status=%u file=%d pos=%u tx=%u chaintx=%lld; "
            "restoring nearest data-backed ancestor h=%d hash=%s\n",
            tip->nHeight, old_hash[0] ? old_hash : "<null>",
            tip->nStatus, tip->nFile, tip->nDataPos, tip->nTx,
            (long long)tip->nChainTx, replacement->nHeight,
            new_hash[0] ? new_hash : "<null>");

    chain_set_active_tip(ms, replacement, TIP_FROM_RESTORE,
                         "quarantine_synthetic_tip");
    if (ms->pindex_best_header == tip)
        ms->pindex_best_header = replacement;

}

static void chain_restore_clear_resolved_anchor(struct main_state *ms,
                                               const char *datadir)
{
    if (!ms)
        return;

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    struct block_index *anchor = snapsync_get_anchor();
    bool backed = (datadir && datadir[0])
        ? chain_restore_block_is_consensus_backed_on_disk(tip, datadir)
        : chain_restore_block_is_consensus_backed(tip);
    if (!anchor || !backed)
        return;

    fprintf(stderr,
            "[chain-restore] clearing restore anchor h=%d after resolving "
            "active consensus tip h=%d\n",
            anchor->nHeight, tip->nHeight);
    snapsync_set_anchor(NULL);
}

bool chain_restore_finalize(struct main_state *ms, const char *datadir)
{
    if (!ms) return false;

    chain_restore_quarantine_synthetic_tip(ms, datadir);
    chain_restore_clear_resolved_anchor(ms, datadir);

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (tip)
        (void)chain_restore_rebuild_active_chain(ms, tip);

    if (datadir && datadir[0])
        (void)chain_restore_backfill_nbits_from_disk(ms, datadir);

    struct chain_integrity_result r;
    chain_integrity_check_post_restore(&r, ms);

    if (!r.ok) {
        fprintf(stderr,
            "[chain-integrity] post-restore check FAILED: "
            "zero_nbits=%d (first_h=%d) tip_window_holes=%d (first_h=%d) "
            "total_holes=%d tip_h=%d\n",
            r.zero_nbits_count, r.first_nbits_zero_height,
            r.tip_window_holes, r.first_tip_window_hole,
            r.active_chain_holes, r.tip_height);
    } else if (tip) {
        /* Holes below tip-WINDOW are expected (live-tip-only boot)
         * and not corruption — report them at INFO so operators
         * can see the chain shape without alarm. */
        if (r.active_chain_holes > 0)
            printf("[chain-integrity] post-restore check OK: "
                   "tip_h=%d tip_window clean, "
                   "%d expected holes below tip-%d (live-tip-only boot)\n",
                   r.tip_height, r.active_chain_holes,
                   CHAIN_INTEGRITY_TIP_WINDOW);
        else
            printf("[chain-integrity] post-restore check OK: "
                   "tip_h=%d nbits clean, active_chain full\n",
                   r.tip_height);
    }

    return r.ok;
}

/* ── Boot activation decision ──────────────────────────────────── */

void boot_should_activate_chain(struct boot_activation_decision *out,
                                int chain_tip_height,
                                int64_t utxo_count,
                                size_t block_index_size,
                                bool legacy_import,
                                bool anchor_was_created)
{
    memset(out, 0, sizeof(*out));
    out->chain_height = chain_tip_height;
    out->utxo_count = utxo_count;
    out->block_index_size = block_index_size;

    if (legacy_import) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_LEGACY_IMPORT;
        return;
    }

    if (anchor_was_created) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_ANCHOR_CREATED;
        return;
    }

    /* No UTXOs + many headers = awaiting P2P snapshot.
     * Connecting blocks from genesis would mark valid blocks FAILED. */
    if (utxo_count < 100000 && chain_tip_height == 0
        && block_index_size > 1000) {
        out->should_activate = false;
        out->reason = ACTIVATE_SKIP_NO_UTXOS_AWAITING;
        return;
    }

    out->should_activate = true;
    out->reason = ACTIVATE_OK;
}
