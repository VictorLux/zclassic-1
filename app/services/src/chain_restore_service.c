/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Restore Service — deterministic chain tip restoration.
 * See chain_restore_service.h for architecture overview. */

#include "platform/time_compat.h"
#include "services/chain_restore_service.h"
#include "services/chain_restore_planner.h"
#include "services/chain_state_repository.h"
#include "services/chain_tip.h"
#include "services/block_index_integrity.h"
#include "models/db_txn.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "primitives/block.h"
#include "core/serialize.h"
#include "storage/disk_block_io.h"
#include "services/snapshot_sync_service.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

static const char *chain_restore_state_name(int s)
{
    switch ((enum chain_restore_state)s) {
    case CHAIN_RESTORE_UNRESOLVED:      return "UNRESOLVED";
    case CHAIN_RESTORE_FOUND_IN_INDEX:  return "FOUND_IN_INDEX";
    case CHAIN_RESTORE_ANCHOR_CREATED:  return "ANCHOR_CREATED";
    case CHAIN_RESTORE_RESOLVED:        return "RESOLVED";
    case CHAIN_RESTORE_FAILED:          return "FAILED";
    }
    return "UNKNOWN";
}

static bool chain_restore_commit_tip_via_csr(struct main_state *ms,
                                             struct block_index *target,
                                             bool update_header_tip,
                                             const char *reason)
{
    if (!ms || !target || !target->phashBlock)
        return false;

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_RESTORE,
        .decision = POLICY_ALLOW,
        .from_height = active_chain_height(&ms->chain_active),
        .to_height = target->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "restore_plan_verified",
        .reason = reason ? reason : "chain_restore",
    };
    struct chain_state_commit commit = {
        .new_tip             = target,
        .new_coins_best      = *target->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip   = update_header_tip,
        .rollback_auth       = &rollback_auth,
        .wallet_scan_height  = -1,
        .reason              = reason ? reason : "chain_restore",
    };

    struct chain_state_repository *csr = csr_instance();
    enum csr_result rc = csr_commit_tip(csr, &commit);
    if (rc == CSR_OK)
        return true;

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        chain_set_active_tip(ms, target, TIP_FROM_RESTORE,
                             reason ? reason : "csr_uninit_fallback");
        if (update_header_tip)
            ms->pindex_best_header = target;
        return true;
    }
#endif

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "chain_restore: csr rejected tip commit (%s) reason=%s h=%d\n",
            csr_result_name(rc), reason ? reason : "", target->nHeight);
    return false;
}

static bool chain_restore_commit_header_via_csr(struct main_state *ms,
                                                struct block_index *target,
                                                const char *reason)
{
    if (!ms || !target || !target->phashBlock)
        return false;

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_RESTORE,
        .decision = POLICY_ALLOW,
        .from_height = ms->pindex_best_header
            ? ms->pindex_best_header->nHeight : -1,
        .to_height = target->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "restore_header_verified",
        .reason = reason ? reason : "chain_restore.header",
    };
    struct chain_state_header_commit commit = {
        .new_header_tip = target,
        .rollback_auth = &rollback_auth,
        .reason = reason ? reason : "chain_restore.header",
    };

    enum csr_result rc = csr_commit_header_tip(csr_instance(), &commit);
    if (rc == CSR_OK)
        return true;

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        ms->pindex_best_header = target;
        return true;
    }
#endif

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "chain_restore: csr rejected header commit (%s) reason=%s h=%d\n",
            csr_result_name(rc), reason ? reason : "", target->nHeight);
    return false;
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
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "chain_restore: anchor inserted but hash not found\n");
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
        if (!target)
            LOG_NULL("chain_restore", "anchor creation failed at h=%d",
                     plan->anchor_height);
        printf("Chain restore: anchor at h=%d\n", plan->anchor_height);
    } else if (plan->next_state == CHAIN_RESTORE_FOUND_IN_INDEX) {
        target = block_map_find(&ms->map_block_index, &plan->anchor_hash);
        if (!target)
            LOG_NULL("chain_restore", "hash in plan but not in block_map");
    }

    if (!target)
        return NULL;

    /* Route the tip/header mutations through the chain_state_repository
     * so block_map, active_chain, coins_tip and pindex_best_header
     * move through the single concrete-state boundary. */
    if (plan->should_set_chain_tip && target->phashBlock) {
        struct chain_state_repository *csr = csr_instance();
        struct node_db *cr_ndb = (csr && csr->initialized) ? csr->ndb : NULL;

        if (cr_ndb && cr_ndb->open) {
            DB_TXN_SCOPE(txn, cr_ndb, "chain_restore.execute");
            if (!txn) {
                fprintf(stderr,
                    "chain_restore: failed to open db_txn scope\n");
                return NULL;
            }
            if (!chain_restore_commit_tip_via_csr(
                    ms, target, plan->should_set_best_header,
                    "chain_restore.execute")) {
                /* Scope auto-rollback fires on return. */
                return NULL;
            }
            if (!db_txn_commit(txn))
                return NULL;
        } else if (!chain_restore_commit_tip_via_csr(
                       ms, target, plan->should_set_best_header,
                       "chain_restore.execute")) {
            return NULL;
        }
    } else if (plan->should_set_best_header) {
        /* Extremely rare: plan asked for header-only update with no
         * chain tip change. Preserve legacy behaviour. */
        if (!chain_restore_commit_header_via_csr(
                ms, target, "chain_restore.header_only")) {
            return NULL;
        }
    }

    if (plan->should_set_snapshot_anchor)
        snapsync_set_anchor(target);

    /* Post-restore finalize — rebuild active_chain from pprev + block_map
     * and surface the integrity result. Unit tests pass
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

/* ── Post-restore integrity check ────────────── */

void chain_integrity_check_post_restore(struct chain_integrity_result *out,
                                        const struct main_state *ms)
{
    memset(out, 0, sizeof(*out));
    out->first_nbits_zero_height = -1;
    out->first_hole_height = -1;
    out->first_mismatch_height = -1;
    out->first_tip_window_hole = -1;

    if (!ms) {
        out->ok = false;
        return;
    }

    /* every pindex with on-disk data must have nBits != 0.
     *
     * skip nBits=0 entries that have no BLOCK_HAVE_DATA bit.
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

    /* chain_active.chain[h] non-NULL for h in [0, tip]. */
    out->tip_height = active_chain_height(&ms->chain_active);
    int window_lo = out->tip_height - CHAIN_INTEGRITY_TIP_WINDOW;
    if (window_lo < 0) window_lo = 0;
    for (int h = 0; h <= out->tip_height; h++) {
        struct block_index *at = active_chain_at(&ms->chain_active, h);
        if (at == NULL) {
            out->active_chain_holes++;
            if (out->first_hole_height < 0 || h < out->first_hole_height)
                out->first_hole_height = h;
            if (h >= window_lo) {
                out->tip_window_holes++;
                if (out->first_tip_window_hole < 0 ||
                    h < out->first_tip_window_hole)
                    out->first_tip_window_hole = h;
            }
        } else if (at->nHeight != h) {
            out->active_chain_mismatches++;
            if (out->first_mismatch_height < 0 ||
                h < out->first_mismatch_height)
                out->first_mismatch_height = h;
        } else if (h > 0 && at->pprev != active_chain_at(&ms->chain_active, h - 1)) {
            out->active_chain_mismatches++;
            if (out->first_mismatch_height < 0 ||
                h < out->first_mismatch_height)
                out->first_mismatch_height = h;
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
     * Keep tip_window_holes / first_*_height fields as diagnostic
     * counters but don't gate `ok` on them. `ok` requires only nBits
     * clean + tip slot populated. */
    bool tip_slot_ok =
        (out->tip_height < 0) ||
        (active_chain_at(&ms->chain_active, out->tip_height) != NULL);
    out->ok = (out->zero_nbits_count == 0 && tip_slot_ok &&
               out->tip_window_holes == 0);
    out->ok = out->ok && out->active_chain_mismatches == 0;

    /* Cache the result for `dumpstate subsystem=boot` / `zcl_state`. */
    chain_restore_record_integrity_result(out);
}

/* ── Boot snapshot ─────────────────────────────────────────────── */

extern struct chain_restore_boot_snapshot g_chain_restore_boot_snapshot;

void chain_restore_record_integrity_result(
    const struct chain_integrity_result *r)
{
    if (!r) return;
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.integrity_ok = r->ok;
    g_chain_restore_boot_snapshot.zero_nbits_count = r->zero_nbits_count;
    g_chain_restore_boot_snapshot.active_chain_holes = r->active_chain_holes;
    g_chain_restore_boot_snapshot.active_chain_mismatches =
        r->active_chain_mismatches;
    g_chain_restore_boot_snapshot.tip_window_holes = r->tip_window_holes;
    g_chain_restore_boot_snapshot.tip_height = r->tip_height;
    g_chain_restore_boot_snapshot.first_nbits_zero_height =
        r->first_nbits_zero_height;
    g_chain_restore_boot_snapshot.first_hole_height = r->first_hole_height;
    g_chain_restore_boot_snapshot.first_mismatch_height =
        r->first_mismatch_height;
    g_chain_restore_boot_snapshot.first_tip_window_hole =
        r->first_tip_window_hole;
}

void chain_restore_record_backfill_result(int fixed,
                                          int read_errors,
                                          int off_chain_cleared)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.backfill_ran = true;
    g_chain_restore_boot_snapshot.backfill_fixed = fixed;
    g_chain_restore_boot_snapshot.backfill_read_errors = read_errors;
    g_chain_restore_boot_snapshot.backfill_off_chain_cleared =
        off_chain_cleared;
}

void chain_restore_record_csr_consistency(bool consistent,
                                          int tip_height,
                                          int header_height)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.csr_consistency_checked = true;
    g_chain_restore_boot_snapshot.csr_consistent = consistent;
    g_chain_restore_boot_snapshot.csr_tip_height = tip_height;
    g_chain_restore_boot_snapshot.csr_header_height = header_height;
}

void chain_restore_record_snapshot_import(bool ok,
                                          int64_t utxo_count,
                                          int64_t snap_height)
{
    g_chain_restore_boot_snapshot.has_data = true;
    g_chain_restore_boot_snapshot.boot_time =
        (int64_t)platform_time_wall_time_t();
    g_chain_restore_boot_snapshot.snapshot_imported_pre_restore = ok;
    g_chain_restore_boot_snapshot.snapshot_imported_utxos = utxo_count;
    g_chain_restore_boot_snapshot.snapshot_imported_height = snap_height;
}

void chain_restore_get_boot_snapshot(struct chain_restore_boot_snapshot *out)
{
    if (!out) return;
    *out = g_chain_restore_boot_snapshot;
}

bool chain_restore_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    const struct chain_restore_boot_snapshot *s =
        &g_chain_restore_boot_snapshot;
    json_set_object(out);
    json_push_kv_bool(out, "has_data", s->has_data);
    json_push_kv_int(out, "boot_time", s->boot_time);
    json_push_kv_bool(out, "integrity_ok", s->integrity_ok);
    json_push_kv_int(out, "zero_nbits_count",
                     s->zero_nbits_count);
    json_push_kv_int(out, "active_chain_holes",
                     s->active_chain_holes);
    json_push_kv_int(out, "active_chain_mismatches",
                     s->active_chain_mismatches);
    json_push_kv_int(out, "tip_window_holes",
                     s->tip_window_holes);
    json_push_kv_int(out, "tip_height", s->tip_height);
    json_push_kv_int(out, "first_nbits_zero_height",
                     s->first_nbits_zero_height);
    json_push_kv_int(out, "first_hole_height",
                     s->first_hole_height);
    json_push_kv_int(out, "first_mismatch_height",
                     s->first_mismatch_height);
    json_push_kv_int(out, "first_tip_window_hole",
                     s->first_tip_window_hole);
    json_push_kv_bool(out, "backfill_ran", s->backfill_ran);
    json_push_kv_int(out, "backfill_fixed", s->backfill_fixed);
    json_push_kv_int(out, "backfill_read_errors",
                     s->backfill_read_errors);
    json_push_kv_int(out, "backfill_off_chain_cleared",
                     s->backfill_off_chain_cleared);
    /* chain_restore_plan result */
    json_push_kv_bool(out, "plan_recorded", s->plan_recorded);
    json_push_kv_str(out, "plan_next_state",
                     chain_restore_state_name(s->plan_next_state));
    json_push_kv_int(out, "plan_anchor_height",
                     s->plan_anchor_height);
    json_push_kv_bool(out, "plan_should_skip_activate",
                      s->plan_should_skip_activate);
    json_push_kv_str(out, "plan_reason", s->plan_reason);
    /* CSR consistency snapshot at boot */
    json_push_kv_bool(out, "csr_consistency_checked",
                      s->csr_consistency_checked);
    json_push_kv_bool(out, "csr_consistent", s->csr_consistent);
    json_push_kv_int(out, "csr_tip_height", s->csr_tip_height);
    json_push_kv_int(out, "csr_header_height",
                     s->csr_header_height);
    /* Wave 11A — snapshot-first boot ordering probe outcome. */
    json_push_kv_bool(out, "snapshot_imported_pre_restore",
                      s->snapshot_imported_pre_restore);
    json_push_kv_int(out, "snapshot_imported_utxos",
                     s->snapshot_imported_utxos);
    json_push_kv_int(out, "snapshot_imported_height",
                     s->snapshot_imported_height);
    return true;
}

/* ── Post-restore repair ────────────────── */

static bool chain_restore_candidate_matches_disk(
    const struct block_index *cand,
    const char *datadir)
{
    if (!datadir || !datadir[0])
        return false;
    return chain_restore_block_is_consensus_backed_on_disk(cand, datadir);
}

static bool chain_restore_active_slot_is_canonical(
    const struct active_chain *c,
    int h)
{
    struct block_index *slot = active_chain_at(c, h);
    if (!slot || slot->nHeight != h)
        return false;
    if (h == 0)
        return true;
    return slot->pprev == active_chain_at(c, h - 1);
}

static void chain_restore_log_first_mismatch(
    const struct active_chain *c,
    int h)
{
    if (!c || h < 0)
        return;

    struct block_index *at = active_chain_at(c, h);
    struct block_index *prev_slot = h > 0 ? active_chain_at(c, h - 1) : NULL;
    char at_hash[65] = {0};
    char pprev_hash[65] = {0};
    char prev_slot_hash[65] = {0};
    if (at && at->phashBlock)
        uint256_get_hex(at->phashBlock, at_hash);
    if (at && at->pprev && at->pprev->phashBlock)
        uint256_get_hex(at->pprev->phashBlock, pprev_hash);
    if (prev_slot && prev_slot->phashBlock)
        uint256_get_hex(prev_slot->phashBlock, prev_slot_hash);
    fprintf(stderr, // obs-ok:failure-diagnostic
            "[chain-integrity] first mismatch detail: h=%d at=%p "
            "at_h=%d at_hash=%s pprev=%p pprev_h=%d pprev_hash=%s "
            "prev_slot=%p prev_slot_h=%d prev_slot_hash=%s\n",
            h, (void *)at, at ? at->nHeight : -1,
            at_hash[0] ? at_hash : "<null>",
            at ? (void *)at->pprev : NULL,
            (at && at->pprev) ? at->pprev->nHeight : -1,
            pprev_hash[0] ? pprev_hash : "<null>",
            (void *)prev_slot, prev_slot ? prev_slot->nHeight : -1,
            prev_slot_hash[0] ? prev_slot_hash : "<null>");
}

static bool chain_restore_read_header_at_index(
    const struct block_index *bi,
    const char *datadir,
    FILE **cached,
    int *cached_file,
    struct block_header *hdr,
    struct uint256 *disk_hash_out,
    bool *hash_matches_out)
{
    if (!bi || !datadir || !datadir[0] || !hdr)
        return false;
    if (hash_matches_out)
        *hash_matches_out = false;
    if (!(bi->nStatus & BLOCK_HAVE_DATA) || bi->nFile < 0 || bi->nDataPos == 0)
        return false;

    if (!*cached || *cached_file != bi->nFile) {
        if (*cached) {
            fclose(*cached);
            *cached = NULL;
        }
        char path[576];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, bi->nFile);
        *cached = fopen(path, "rb");
        if (!*cached)
            return false;
        *cached_file = bi->nFile;
    }

    unsigned char buf[4 + 32 + 32 + 32 + 4 + 4 + 32 + 9 + MAX_SOLUTION_SIZE];
    if (fseek(*cached, (long)bi->nDataPos, SEEK_SET) != 0)
        return false;
    size_t nread = fread(buf, 1, sizeof(buf), *cached); // disk-io-lock: boot-local repair
    if (nread < 4 + 32 + 32 + 32 + 4 + 4 + 32 + 1)
        return false;

    struct byte_stream s;
    stream_init_from_data(&s, buf, nread);
    block_header_init(hdr);
    bool ok = block_header_deserialize(hdr, &s);
    stream_free(&s);
    if (!ok)
        return false;

    struct uint256 disk_hash;
    block_header_get_hash(hdr, &disk_hash);
    if (disk_hash_out)
        *disk_hash_out = disk_hash;
    if (hash_matches_out)
        *hash_matches_out = !bi->phashBlock ||
            uint256_cmp(&disk_hash, bi->phashBlock) == 0;

    return true;
}

static int chain_restore_rebuild_active_chain_from_disk(
    struct main_state *ms,
    struct block_index *tip,
    const char *datadir)
{
    if (!ms || !tip || !datadir || !datadir[0])
        return 0;

    struct active_chain *c = &ms->chain_active;
    struct block_index *cur = tip;
    FILE *cached = NULL;
    int cached_file = -1;
    int populated = 0;
    int read_errors = 0;
    int repaired_heights = 0;
    int repaired_hashes = 0;
    char stop_reason[160] = "";
    int stop_height = -1;

    for (int h = tip->nHeight; h >= 0 && cur; h--) {
        if (cur->nHeight != h) {
            read_errors++;
            break;
        }

        struct block_header hdr;
        struct uint256 disk_hash;
        bool hash_matches = false;
        if (!chain_restore_read_header_at_index(cur, datadir,
                                                &cached, &cached_file, &hdr,
                                                &disk_hash, &hash_matches)) {
            read_errors++;
            stop_height = h;
            snprintf(stop_reason, sizeof(stop_reason),
                     "read_header_io_failed hash=%s",
                     cur->phashBlock ? "present" : "null");
            break;
        }
        if (!hash_matches) {
            struct block_index *replacement =
                block_map_find(&ms->map_block_index, &disk_hash);
            if (!replacement) {
                read_errors++;
                stop_height = h;
                char got_hex[65] = {0};
                char want_hex[65] = {0};
                uint256_get_hex(&disk_hash, got_hex);
                if (cur->phashBlock)
                    uint256_get_hex(cur->phashBlock, want_hex);
                snprintf(stop_reason, sizeof(stop_reason),
                         "disk_hash_unindexed got=%s want=%s",
                         got_hex, want_hex[0] ? want_hex : "<null>");
                break;
            }

            if (replacement != cur) {
                replacement->nFile = cur->nFile;
                replacement->nDataPos = cur->nDataPos;
                replacement->nStatus |= cur->nStatus & BLOCK_HAVE_DATA;
                replacement->nHeight = h;
                replacement->pskip = NULL;
                cur = replacement;
            }
            const struct uint256 *stored_hash =
                block_map_find_hash(&ms->map_block_index, &disk_hash);
            if (stored_hash)
                cur->phashBlock = stored_hash;
            if (h < tip->nHeight && c->chain[h + 1])
                c->chain[h + 1]->pprev = cur;
            repaired_hashes++;
        }

        c->chain[h] = cur;
        populated++;

        if (h == 0) {
            cur->pprev = NULL;
            break;
        }

        struct block_index *prev =
            block_map_find(&ms->map_block_index, &hdr.hashPrevBlock);
        if (!prev) {
            read_errors++;
            stop_height = h;
            char prev_hex[65] = {0};
            uint256_get_hex(&hdr.hashPrevBlock, prev_hex);
            snprintf(stop_reason, sizeof(stop_reason),
                     "prev_lookup_failed prev=%s found=0 want_h=%d",
                     prev_hex, h - 1);
            break;
        }
        if (prev->nHeight != h - 1) {
            prev->nHeight = h - 1;
            prev->pskip = NULL;
            repaired_heights++;
        }
        cur->pprev = prev;
        if (cur->pskip == NULL)
            block_index_build_skip(cur);
        cur = prev;
    }

    if (cached)
        fclose(cached);

    if (read_errors > 0)
        printf("[chain-restore] disk ancestry rebuild stopped early: "
               "tip_h=%d populated=%d repaired_heights=%d repaired_hashes=%d "
               "read_errors=%d stop_h=%d reason=%s\n",
               tip->nHeight, populated, repaired_heights, repaired_hashes,
               read_errors,
               stop_height, stop_reason[0] ? stop_reason : "unknown");
    else
        printf("[chain-restore] disk ancestry rebuilt active chain: "
               "tip_h=%d populated=%d repaired_heights=%d "
               "repaired_hashes=%d\n",
               tip->nHeight, populated, repaired_heights, repaired_hashes);

    return populated;
}

struct chain_restore_disk_pos_entry {
    struct uint256 hash;
    int file;
    unsigned int pos;
    bool occupied;
};

struct chain_restore_disk_pos_map {
    struct chain_restore_disk_pos_entry *entries;
    size_t capacity;
    size_t size;
};

static uint64_t chain_restore_hash_key(const struct uint256 *h)
{
    uint64_t v;
    memcpy(&v, h->data, sizeof(v));
    return v;
}

static bool chain_restore_disk_pos_map_init(
    struct chain_restore_disk_pos_map *m,
    size_t expected)
{
    size_t cap = 4096;
    while (cap < expected * 2)
        cap *= 2;
    m->entries = zcl_calloc(cap, sizeof(*m->entries),
                            "chain_restore/disk_pos_map");
    if (!m->entries)
        return false;
    m->capacity = cap;
    m->size = 0;
    return true;
}

static void chain_restore_disk_pos_map_free(
    struct chain_restore_disk_pos_map *m)
{
    if (!m)
        return;
    free(m->entries);
    m->entries = NULL;
    m->capacity = 0;
    m->size = 0;
}

static bool chain_restore_disk_pos_map_put(
    struct chain_restore_disk_pos_map *m,
    const struct uint256 *hash,
    int file,
    unsigned int pos)
{
    if (!m || !m->entries || m->capacity == 0)
        return false;
    size_t idx = chain_restore_hash_key(hash) & (m->capacity - 1);
    for (size_t i = 0; i < m->capacity; i++) {
        size_t slot = (idx + i) & (m->capacity - 1);
        struct chain_restore_disk_pos_entry *e = &m->entries[slot];
        if (!e->occupied) {
            e->hash = *hash;
            e->file = file;
            e->pos = pos;
            e->occupied = true;
            m->size++;
            return true;
        }
        if (uint256_eq(&e->hash, hash)) {
            e->file = file;
            e->pos = pos;
            return true;
        }
    }
    return false;
}

static const struct chain_restore_disk_pos_entry *
chain_restore_disk_pos_map_find(
    const struct chain_restore_disk_pos_map *m,
    const struct uint256 *hash)
{
    if (!m || !m->entries || m->capacity == 0)
        return NULL;
    size_t idx = chain_restore_hash_key(hash) & (m->capacity - 1);
    for (size_t i = 0; i < m->capacity; i++) {
        size_t slot = (idx + i) & (m->capacity - 1);
        const struct chain_restore_disk_pos_entry *e = &m->entries[slot];
        if (!e->occupied)
            return NULL;
        if (uint256_eq(&e->hash, hash))
            return e;
    }
    return NULL;
}

static bool chain_restore_scan_block_files(
    const char *datadir,
    struct chain_restore_disk_pos_map *map)
{
    if (!datadir || !datadir[0] || !map)
        return false;
    const struct chain_params *cp = chain_params_get();
    const unsigned char *magic = cp->pchMessageStart;
    int files = 0;
    int blocks = 0;

    for (int file_num = 0; file_num < 9999; file_num++) {
        char path[576];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, file_num);
        FILE *f = fopen(path, "rb");
        if (!f)
            break;
        files++;
        unsigned char prefix[8];
        while (fread(prefix, 1, sizeof(prefix), f) == sizeof(prefix)) {
            if (memcmp(prefix, magic, 4) != 0) {
                if (fseek(f, -7L, SEEK_CUR) != 0)
                    break;
                continue;
            }
            uint32_t block_size;
            memcpy(&block_size, prefix + 4, sizeof(block_size));
            if (block_size < 4 + 32 + 32 + 32 + 4 + 4 + 32 + 1 ||
                block_size > 4000000) {
                if (fseek(f, -7L, SEEK_CUR) != 0)
                    break;
                continue;
            }
            long data_pos = ftell(f);
            if (data_pos < 0)
                break;
            unsigned char hdr_buf[4 + 32 + 32 + 32 + 4 + 4 + 32 + 9 +
                                  MAX_SOLUTION_SIZE];
            size_t want = block_size < sizeof(hdr_buf)
                ? (size_t)block_size : sizeof(hdr_buf);
            size_t nread = fread(hdr_buf, 1, want, f);
            if (nread < 4 + 32 + 32 + 32 + 4 + 4 + 32 + 1)
                break;
            struct byte_stream s;
            stream_init_from_data(&s, hdr_buf, nread);
            struct block_header hdr;
            block_header_init(&hdr);
            bool ok = block_header_deserialize(&hdr, &s);
            stream_free(&s);
            if (ok) {
                struct uint256 hash;
                block_header_get_hash(&hdr, &hash);
                if (!chain_restore_disk_pos_map_put(map, &hash, file_num,
                                                    (unsigned int)data_pos)) {
                    fclose(f);
                    return false;
                }
                blocks++;
            }
            long next_pos = data_pos + (long)block_size;
            if (fseek(f, next_pos, SEEK_SET) != 0)
                break;
        }
        fclose(f);
    }

    printf("[chain-restore] scanned block files for canonical positions: "
           "files=%d blocks=%d indexed=%zu\n",
           files, blocks, map->size);
    return map->size > 0;
}

static bool chain_restore_read_header_at_pos(
    const char *datadir,
    int file,
    unsigned int pos,
    struct block_header *hdr)
{
    if (!datadir || !datadir[0] || file < 0 || pos == 0 || !hdr)
        return false;
    char path[576];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat", datadir, file);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char buf[4 + 32 + 32 + 32 + 4 + 4 + 32 + 9 + MAX_SOLUTION_SIZE];
    bool ok = false;
    if (fseek(f, (long)pos, SEEK_SET) == 0) {
        size_t nread = fread(buf, 1, sizeof(buf), f); // disk-io-lock: boot-local repair
        if (nread >= 4 + 32 + 32 + 32 + 4 + 4 + 32 + 1) {
            struct byte_stream s;
            stream_init_from_data(&s, buf, nread);
            block_header_init(hdr);
            ok = block_header_deserialize(hdr, &s);
            stream_free(&s);
        }
    }
    fclose(f);
    return ok;
}

static int chain_restore_rebuild_active_chain_from_block_files(
    struct main_state *ms,
    struct block_index *tip,
    const char *datadir)
{
    if (!ms || !tip || !tip->phashBlock || !datadir || !datadir[0])
        return 0;

    struct chain_restore_disk_pos_map pos_map = {0};
    size_t expected = block_map_count(&ms->map_block_index);
    if (!chain_restore_disk_pos_map_init(&pos_map, expected ? expected : 4096))
        return 0;
    if (!chain_restore_scan_block_files(datadir, &pos_map)) {
        chain_restore_disk_pos_map_free(&pos_map);
        return 0;
    }

    struct active_chain *c = &ms->chain_active;
    struct uint256 want = *tip->phashBlock;
    struct block_index *child = NULL;
    int populated = 0;
    int repaired = 0;

    for (int h = tip->nHeight; h >= 0; h--) {
        const struct chain_restore_disk_pos_entry *pos =
            chain_restore_disk_pos_map_find(&pos_map, &want);
        if (!pos)
            break;
        struct block_header hdr;
        if (!chain_restore_read_header_at_pos(datadir, pos->file, pos->pos,
                                              &hdr))
            break;
        struct uint256 disk_hash;
        block_header_get_hash(&hdr, &disk_hash);
        if (!uint256_eq(&disk_hash, &want))
            break;
        struct block_index *cur =
            block_map_find(&ms->map_block_index, &want);
        if (!cur)
            break;

        cur->nHeight = h;
        cur->nFile = pos->file;
        cur->nDataPos = pos->pos;
        cur->nStatus |= BLOCK_HAVE_DATA;
        cur->nVersion = hdr.nVersion;
        cur->hashMerkleRoot = hdr.hashMerkleRoot;
        cur->hashFinalSaplingRoot = hdr.hashFinalSaplingRoot;
        cur->nTime = hdr.nTime;
        cur->nBits = hdr.nBits;
        cur->nNonce = hdr.nNonce;
        cur->pskip = NULL;
        cur->pprev = NULL;
        if (child) {
            child->pprev = cur;
            block_index_build_skip(child);
        }
        c->chain[h] = cur;
        populated++;
        repaired++;
        child = cur;
        want = hdr.hashPrevBlock;
    }

    if (child)
        child->pprev = NULL;
    chain_restore_disk_pos_map_free(&pos_map);
    printf("[chain-restore] block-file ancestry rebuilt active chain: "
           "tip_h=%d populated=%d repaired=%d\n",
           tip->nHeight, populated, repaired);
    return populated;
}

int chain_restore_rebuild_active_chain(struct main_state *ms,
                                       struct block_index *tip,
                                       const char *datadir)
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
            if (!chain_restore_commit_tip_via_csr(
                    ms, tip, false, "rebuild_active_chain_full"))
                return 0;
        }
    }

	    int populated = 0;

    if (datadir && datadir[0]) {
        populated = chain_restore_rebuild_active_chain_from_disk(ms, tip,
                                                                datadir);
        if (populated == tip_h + 1) {
            struct chain_integrity_result r;
            chain_integrity_check_post_restore(&r, ms);
            if (r.active_chain_mismatches == 0 &&
                r.tip_window_holes == 0)
                return populated;
            populated = chain_restore_rebuild_active_chain_from_block_files(
                ms, tip, datadir);
            if (populated == tip_h + 1)
                return populated;
        } else {
            populated = chain_restore_rebuild_active_chain_from_block_files(
                ms, tip, datadir);
            if (populated == tip_h + 1)
                return populated;
        }
    }

    /* Fast path — walk pprev from tip and slot each ancestor. Covers
     * the happy case (real chain, pprev intact) in O(tip_h). */
    int deepest = tip_h + 1;
    int pprev_walk_limit = tip_h > 1000000 ? 10000 : tip_h + 1;
    int pprev_walk_budget = tip_h + 1;
    for (struct block_index *p = tip; p != NULL; p = p->pprev) {
        if (--pprev_walk_budget < 0) {
            printf("[chain-restore] stopped cyclic pprev walk during live boot: "
                   "tip_h=%d deepest=%d populated=%d\n",
                   tip_h, deepest, populated);
            break;
        }
        int h = p->nHeight;
        if (h < 0 || h > tip_h) break;
        bool disk_ok = true;
        if (datadir && datadir[0] &&
            (p->nStatus & BLOCK_HAVE_DATA) && p->nDataPos != 0)
            disk_ok = chain_restore_candidate_matches_disk(p, datadir);
        if (disk_ok) {
            if (c->chain[h] != p) c->chain[h] = p;
        } else if (c->chain[h] == p) {
            c->chain[h] = NULL;
        }
        if (h < deepest) deepest = h;
        populated++;
        if (populated >= pprev_walk_limit && deepest > 0) {
            printf("[chain-restore] capped pprev walk during live boot: "
                   "tip_h=%d deepest=%d populated=%d\n",
                   tip_h, deepest, populated);
            break;
        }
    }

    /* If the pprev walk reached genesis, no residual slot work remains,
     * but flat-file loads may still have left pskip empty. Rebuild
     * missing skip pointers bottom-up so ancestor lookups stay O(log N). */
    if (deepest == 0) {
        for (int h = 1; h <= tip_h; h++) {
            struct block_index *cur = c->chain[h];
            if (cur && cur->pprev && cur->pskip == NULL)
                block_index_build_skip(cur);
        }
        return populated;
    }

    /* Residual holes/mismatches. The capped pprev walk fixes the common
     * high-tip path, but a stale chain_active slot can still contain a
     * non-NULL block_index from a different height. Scan the block_map
     * once across the full active range, then repair every invalid slot
     * in one sweep. This keeps boot O(N) while guaranteeing a bad pointer
     * is not left behind as canonical state. */
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
        if (h < 0 || h > tip_h) continue;
        if (chain_restore_active_slot_is_canonical(c, h))
            continue;
        if (cand->nStatus & BLOCK_FAILED_MASK) continue;

        struct block_index *best = by_height[h];
        if (!best) { by_height[h] = cand; continue; }

        /* Prefer BLOCK_HAVE_DATA, then highest nChainWork — matches
         * the tie-break rules so we don't slot a stale
         * fork over a real ancestor. */
        bool best_data = (best->nStatus & BLOCK_HAVE_DATA) != 0;
        bool cand_data = (cand->nStatus & BLOCK_HAVE_DATA) != 0;
        if (cand_data && !best_data) { by_height[h] = cand; continue; }
        if (best_data && !cand_data) continue;

        if (datadir && datadir[0] && cand_data && best_data) {
            bool cand_disk = chain_restore_candidate_matches_disk(cand, datadir);
            bool best_disk = chain_restore_candidate_matches_disk(best, datadir);
            if (cand_disk && !best_disk) { by_height[h] = cand; continue; }
            if (best_disk && !cand_disk) continue;
        }

        if (arith_uint256_compare(&cand->nChainWork,
                                  &best->nChainWork) > 0)
            by_height[h] = cand;
    }

    for (int h = 0; h <= tip_h; h++) {
        if (chain_restore_active_slot_is_canonical(c, h))
            continue;
        if (by_height[h]) {
            c->chain[h] = by_height[h];
            populated++;
        } else {
            c->chain[h] = NULL;
        }
    }

    free(by_height);

    /* wire pprev + pskip across the rebuilt chain. The anchor
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

    /* collect the active tip height once so we can
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
            p->nVersion = blk.header.nVersion;
            p->hashMerkleRoot = blk.header.hashMerkleRoot;
            p->hashFinalSaplingRoot = blk.header.hashFinalSaplingRoot;
            p->nTime = blk.header.nTime;
            p->nBits = blk.header.nBits;
            p->nNonce = blk.header.nNonce;
            if (p->pprev) {
                block_index_build_skip(p);
                struct arith_uint256 proof = GetBlockProof(p);
                arith_uint256_add(&p->nChainWork,
                                  &p->pprev->nChainWork, &proof);
            } else {
                p->nChainWork = GetBlockProof(p);
            }
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

int chain_restore_clear_failed_above_tip(struct main_state *ms)
{
    if (!ms)
        return 0;

    int tip_h = active_chain_height(&ms->chain_active);

    int cleared = 0;
    size_t iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p) continue;
        if (p->nHeight <= tip_h) continue;
        unsigned failed = p->nStatus & (unsigned)BLOCK_FAILED_MASK;
        if (!failed) continue;
        p->nStatus &= ~(unsigned)BLOCK_FAILED_MASK;
        cleared++;
    }

    if (cleared > 0)
        printf("[chain-restore] cleared %d stale BLOCK_FAILED_VALID "
               "flags above tip h=%d\n", cleared, tip_h);

    return cleared;
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
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[chain-restore] disk merkle mismatch at h=%d got=%s want=%s\n",
                tip->nHeight, got, want);
        ok = false;
    }

    if (ok && tip->nBits != 0 && blk.header.nBits != tip->nBits) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
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

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[chain-restore] quarantining non-consensus active tip "
            "h=%d hash=%s status=%u file=%d pos=%u tx=%u chaintx=%lld; "
            "restoring nearest data-backed ancestor h=%d hash=%s\n",
            tip->nHeight, old_hash[0] ? old_hash : "<null>",
            tip->nStatus, tip->nFile, tip->nDataPos, tip->nTx,
            (long long)tip->nChainTx, replacement->nHeight,
            new_hash[0] ? new_hash : "<null>");

    if (!chain_restore_commit_tip_via_csr(
            ms, replacement, ms->pindex_best_header == tip,
            "quarantine_synthetic_tip")) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[chain-restore] failed to quarantine synthetic tip via csr\n");
    }
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
    if (!anchor || !tip || !backed)
        return;
    if (tip->nHeight < anchor->nHeight)
        return;

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
        (void)chain_restore_rebuild_active_chain(ms, tip, datadir);

    if (datadir && datadir[0])
        (void)chain_restore_backfill_nbits_from_disk(ms, datadir);

    struct chain_integrity_result r;
    chain_integrity_check_post_restore(&r, ms);

    /* also record csr-side tip ↔ coins_best_block
     * consistency in the boot snapshot. csr_snapshot is idempotent
     * and returns tip_height=-1 if csr isn't initialized (some test
     * paths), in which case we leave csr_consistency_checked=false. */
    {
        struct chain_state_repository *csr = csr_instance();
        if (csr && csr->initialized) {
            struct chain_state_view view;
            csr_snapshot(csr, &view);
            chain_restore_record_csr_consistency(
                view.consistent, view.tip_height, view.header_height);
            if (!view.consistent) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[chain-integrity] CSR tip/coins divergence at boot: "
                    "tip_h=%d header_h=%d — first activate_best_chain "
                    "pass should reconcile\n",
                    view.tip_height, view.header_height);
            }
        }
    }

    if (!r.ok) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[chain-integrity] post-restore check FAILED: "
            "zero_nbits=%d (first_h=%d) tip_window_holes=%d (first_h=%d) "
            "total_holes=%d mismatches=%d (first_h=%d) tip_h=%d\n",
            r.zero_nbits_count, r.first_nbits_zero_height,
            r.tip_window_holes, r.first_tip_window_hole,
            r.active_chain_holes, r.active_chain_mismatches,
            r.first_mismatch_height, r.tip_height);
        chain_restore_log_first_mismatch(&ms->chain_active,
                                         r.first_mismatch_height);
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
        bii_record_recovery_status(
            BII_OK, BII_RECOVERY_ACCEPTED,
            "post-restore integrity clean; active chain reconciled",
            false, false);
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
