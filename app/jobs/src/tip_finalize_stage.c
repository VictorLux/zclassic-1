/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * tip_finalize_stage — implementation. See jobs/tip_finalize_stage.h. */

#include "platform/time_compat.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_anchor.h"
#include "jobs/stage_helpers.h"
#include "tip_finalize_post_step.h"
#include "tip_finalize_log_store.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STAGE_NAME "tip_finalize"


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static tip_finalize_utxo_count_fn g_utxo_counter = NULL;
static void *g_utxo_counter_user = NULL;

static _Atomic uint64_t g_finalized_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_reorg_detected_total = 0;
static _Atomic uint64_t g_utxo_count_diverged_total = 0;
static _Atomic uint64_t g_precondition_failed_total = 0;
static _Atomic uint64_t g_total_work_added_high = 0;
static _Atomic uint64_t g_total_work_added_low = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;
static uint8_t         g_last_advance_hash[32];
static zcl_mutex_t     g_last_advance_hash_mu;

/* Last specific precondition that blocked tip_finalize. The persisted
 * tip_finalize_log status column stays the generic "precondition_failed"
 * token (downstream + tests match on it); this names WHICH check failed so
 * a script-validation stall is not masked. Guarded by g_block_reason_mu. */
static _Atomic int64_t  g_last_precondition_height = -1;
static char             g_last_precondition_reason[40] = "";
static zcl_mutex_t      g_block_reason_mu;

static void update_last_advance(int height, const uint8_t hash[32])
{
    atomic_store(&g_last_advance_height, (int64_t)height);
    zcl_mutex_lock(&g_last_advance_hash_mu);
    memcpy(g_last_advance_hash, hash, 32);
    zcl_mutex_unlock(&g_last_advance_hash_mu);
}

static bool get_last_advance(int64_t *height, uint8_t hash[32])
{
    *height = atomic_load(&g_last_advance_height);
    if (*height < 0) return false;
    zcl_mutex_lock(&g_last_advance_hash_mu);
    memcpy(hash, g_last_advance_hash, 32);
    zcl_mutex_unlock(&g_last_advance_hash_mu);
    return true;
}


static bool ensure_authority_anchor_row(sqlite3 *db, int height,
                                        const uint8_t hash[32])
{
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, height, &row))
        return false;
    if (row.found && row.ok && row.has_tip_hash &&
        memcmp(row.tip_hash.data, hash, 32) == 0)
        return true;

    struct uint256 tip_hash;
    memcpy(tip_hash.data, hash, 32);
    return log_insert(db, height, "anchor", true, NULL, 0, 0, &tip_hash);
}

static bool anchor_cursor_to_authority(sqlite3 *db, int height,
                                       const uint8_t hash[32],
                                       bool require_prior_progress,
                                       const char *reason)
{
    if (!db || !g_stage || height < 0 || !hash)
        return true;

    uint64_t target = (uint64_t)height + 1u;
    uint64_t cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
    int64_t rows = stage_log_row_count(db, STAGE_NAME, "tip_finalize_log");
    if (require_prior_progress && cursor == 0 && rows <= 0)
        return true;
    if (!ensure_authority_anchor_row(db, height, hash))
        return false;
    if (!stage_anchor_upstream_cursors_to(db, target, STAGE_NAME, reason))
        return false;
    if (cursor >= target)
        return true;
    if (!stage_set_cursor(g_stage, db, target)) {
        LOG_WARN("tip_finalize",
                 "[tip_finalize] authority anchor cursor failed from=%llu to=%llu reason=%s",
                 (unsigned long long)cursor,
                 (unsigned long long)target,
                 reason ? reason : "");
        return false;
    }
    LOG_INFO("tip_finalize",
             "[tip_finalize] authority anchor cursor from=%llu to=%llu reason=%s",
             (unsigned long long)cursor,
             (unsigned long long)target,
             reason ? reason : "");
    return true;
}

static int reorg_depth_from(struct block_index *old_tip,
                            struct block_index *new_tip)
{
    int depth = 0;
    struct block_index *p = new_tip;
    while (p && p->nHeight > old_tip->nHeight) {
        p = p->pprev;
        depth++;
    }
    while (p && old_tip && p != old_tip) {
        p = p->pprev;
        old_tip = old_tip->pprev;
        depth++;
    }
    return depth > 0 ? depth : 1;
}

/* Returns the specific precondition that fails, or NULL if all pass.
 * Check order is identical to the prior preconditions_ok() it replaces, so
 * the derived bool (reason == NULL) is bit-for-bit the same verdict. Set:
 *   block_missing       — no candidate block_index for this height
 *   have_data_missing   — body not on disk (BLOCK_HAVE_DATA clear)
 *   not_script_valid    — validity below BLOCK_VALID_SCRIPTS (script stall)
 *   not_header_valid    — validity below BLOCK_VALID_HEADER */
static const char *precondition_block_reason(const struct block_index *bi)
{
    if (!bi) return "block_missing";
    if (!(bi->nStatus & BLOCK_HAVE_DATA)) return "have_data_missing";
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
        return "not_script_valid";
    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_HEADER)
        return "not_header_valid";
    return NULL;
}

static void record_precondition_block(int height, const char *reason)
{
    atomic_store(&g_last_precondition_height, (int64_t)height);
    zcl_mutex_lock(&g_block_reason_mu);
    snprintf(g_last_precondition_reason, sizeof g_last_precondition_reason,
             "%s", reason ? reason : "");
    zcl_mutex_unlock(&g_block_reason_mu);
    LOG_WARN("tip_finalize",
             "[tip_finalize] precondition_failed height=%d reason=%s",
             height, reason ? reason : "");
}

static bool finalized_row_active_match(sqlite3 *db, int row_height,
                                       bool *out_known,
                                       bool *out_matches)
{
    *out_known = false;
    *out_matches = false;
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, row_height, &row))
        return false;
    if (!row.found || !row.ok || !row.has_tip_hash)
        return true;

    struct main_state *ms = g_ms;
    struct block_index *active =
        ms ? active_chain_at(&ms->chain_active, row_height + 1) : NULL;
    if (!active || !active->phashBlock)
        return true;
    *out_known = true;
    *out_matches = uint256_eq(&row.tip_hash, active->phashBlock);
    return true;
}

static bool rewind_cursor_if_active_chain_reorged(sqlite3 *db)
{
    if (!g_stage || !g_ms)
        return true;

    uint64_t cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
    if (cursor == 0)
        return true;
    if (cursor > (uint64_t)INT32_MAX) {
        LOG_WARN("tip_finalize", "[tip_finalize] reorg rewind cursor too large: %llu", (unsigned long long)cursor);
        return false;
    }

    bool known = false;
    bool matches = false;
    if (!finalized_row_active_match(db, (int)cursor - 1, &known, &matches))
        return false;
    if (!known || matches)
        return true;

    uint64_t rewind_to = 0;
    for (int h = (int)cursor - 2; h >= 0; h--) {
        known = false;
        matches = false;
        if (!finalized_row_active_match(db, h, &known, &matches))
            return false;
        if (known && matches) {
            rewind_to = (uint64_t)h + 1u;
            break;
        }
    }
    if (rewind_to == cursor)
        return true;

    if (!stage_set_cursor(g_stage, db, rewind_to)) {
        LOG_WARN("tip_finalize", "[tip_finalize] reorg rewind failed from=%llu to=%llu", (unsigned long long)cursor, (unsigned long long)rewind_to);
        return false;
    }

    atomic_fetch_add(&g_reorg_detected_total, 1);
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    event_emitf(EV_BLOCK_REJECTED, 0,
                "tip_finalize reorg_cursor_rewind from=%llu to=%llu",
                (unsigned long long)cursor,
                (unsigned long long)rewind_to);
    return true;
}

static bool live_utxo_count_after(int height_after, int64_t *out_count)
{
    *out_count = -1;
    if (!g_utxo_counter)
        return true;
    return g_utxo_counter(height_after, out_count, g_utxo_counter_user);
}

static job_result_t step_finalize(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t uv_cursor = stage_cursor_persisted(db, "utxo_apply",
                                               STAGE_NAME);
    if ((uint64_t)next_h > uv_cursor) {
        LOG_WARN("tip_finalize",
            "[tip_finalize] cursor_in=%d exceeds utxo_apply cursor=%llu",
            next_h, (unsigned long long)uv_cursor);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }
    if ((uint64_t)next_h >= uv_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct utxo_apply_row upstream;
    int found = utxo_apply_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        struct arith_uint256 zero;
        arith_uint256_set_zero(&zero);
        if (!log_insert(db, next_h, "upstream_failed", false, &zero,
                        -1, 0, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *old_tip = active_chain_at(&ms->chain_active, next_h);
    struct block_index *new_tip = active_chain_at(&ms->chain_active,
                                                  next_h + 1);
    if (!new_tip) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }
    if (!old_tip) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct arith_uint256 work_delta;
    arith_uint256_set_zero(&work_delta);

    if (new_tip->pprev != old_tip) {
        int depth = reorg_depth_from(old_tip, new_tip);
        if (!log_insert(db, next_h, "reorg_detected", false, &work_delta,
                        -1, depth, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_reorg_detected_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "tip_finalize reorg_detected height=%d depth=%d",
                    next_h, depth);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    /* Name WHICH precondition blocks before collapsing to the bool, so the
     * stall reason (e.g. not_script_valid) is not masked. Evaluated in the
     * same order: the block-status checks first, then the chainwork compare,
     * exactly matching the prior (!preconditions_ok || chainwork<=0)
     * verdict — the branch is taken iff precond_reason != NULL, identical. */
    const char *precond_reason = precondition_block_reason(new_tip);
    if (precond_reason == NULL &&
        arith_uint256_compare(&new_tip->nChainWork,
                              &old_tip->nChainWork) <= 0)
        precond_reason = "chainwork_not_greater";
    if (precond_reason != NULL) {
        if (!log_insert(db, next_h, "precondition_failed", false,
                        &work_delta, -1, 0, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_precondition_failed_total, 1);
        record_precondition_block(next_h, precond_reason);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "tip_finalize precondition_failed height=%d reason=%s",
                    next_h, precond_reason);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    arith_uint256_sub(&work_delta, &new_tip->nChainWork,
                      &old_tip->nChainWork);

    int64_t utxo_size_after = -1;
    if (!live_utxo_count_after(next_h + 1, &utxo_size_after))
        return JOB_FATAL;
    if (utxo_size_after >= 0) {
        int64_t spent = 0, added = 0;
        if (!utxo_apply_sums_through(db, next_h, &spent, &added))
            return JOB_FATAL;
        int64_t expected = added - spent;
        if (utxo_size_after != expected) {
            if (!log_insert(db, next_h, "utxo_count_diverged", false,
                            &work_delta, utxo_size_after, 0, NULL))
                return JOB_FATAL;
            atomic_fetch_add(&g_utxo_count_diverged_total, 1);
            event_emitf(EV_BLOCK_REJECTED, 0,
                        "tip_finalize utxo_count_diverged height=%d "
                        "live=%lld expected=%lld",
                        next_h, (long long)utxo_size_after,
                        (long long)expected);
            c->cursor_out = c->cursor_in + 1;
            return JOB_ADVANCED;
        }
    }

    if (!log_insert(db, next_h, "finalized", true, &work_delta,
                    utxo_size_after, 0, new_tip->phashBlock))
        return JOB_FATAL;

    /* Durable row first; then move the local chain[] cache/window. Public
     * authority is published explicitly below. */
    if (!active_chain_move_window_tip(&ms->chain_active, new_tip)) { // one-write-path-ok:reducer-tip-authority
        LOG_WARN("tip_finalize",
            "[tip_finalize] chain_active set_tip failed height=%d",
            next_h);
        return JOB_FATAL;
    }

    /* Run derived tip side effects after the local cache moves. */
    tip_finalize_run_post_finalize(new_tip);

    atomic_fetch_add(&g_finalized_total, 1);
    atomic_fetch_add(&g_total_work_added_low,
                     arith_uint256_get_low64(&work_delta));
    atomic_fetch_add(&g_total_work_added_high,
                     ((uint64_t)work_delta.pn[3] << 32) | work_delta.pn[2]);
    update_last_advance(next_h, new_tip->phashBlock->data);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

static bool is_authoritative(void)
{
    return true;
}

static int64_t get_height(void)
{
    return tip_finalize_stage_last_height();
}

static bool get_hash(uint8_t hash[32])
{
    int64_t h;
    return get_last_advance(&h, hash);
}

bool tip_finalize_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("tip_finalize", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("tip_finalize", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    zcl_mutex_init(&g_last_advance_hash_mu);
    zcl_mutex_init(&g_block_reason_mu);
    g_ms = ms;

    struct block_index *existing_tip =
        active_chain_cached_tip(&ms->chain_active);
    if (existing_tip && existing_tip->phashBlock &&
        atomic_load(&g_last_advance_height) < 0)
        update_last_advance(existing_tip->nHeight,
                            existing_tip->phashBlock->data);

    struct active_chain_authority auth = {
        .get_height = get_height,
        .get_hash = get_hash,
        .is_authoritative = is_authoritative
    };
    active_chain_register_authority(&auth);
    active_chain_register_block_map(&ms->map_block_index);

    if (g_stage != NULL) {
        if (existing_tip && existing_tip->phashBlock &&
            !anchor_cursor_to_authority(
                db, existing_tip->nHeight, existing_tip->phashBlock->data,
                false, "init_existing_tip_reanchor")) {
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        pthread_mutex_unlock(&g_lock);
        return true;
    }

    if (!ensure_log_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_finalize, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("tip_finalize", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    if (existing_tip && existing_tip->phashBlock &&
        !anchor_cursor_to_authority(db, existing_tip->nHeight,
                                    existing_tip->phashBlock->data,
                                    true, "init_existing_tip")) {
        stage_destroy(s);
        g_stage = NULL;
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("tip_finalize",
             "[tip_finalize] stage initialised (authoritative)");
    return true;
}

job_result_t tip_finalize_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    /* Re-widen chain[] to the most-work candidate so step_finalize's
     * one-block lookahead finds next_h+1 — its own set_tip collapses the
     * window each step. stage_helpers.h */
    reducer_extend_window_to_candidate(g_ms, true);
    progress_store_tx_lock();
    bool rewind_ok = rewind_cursor_if_active_chain_reorged(db);
    if (!rewind_ok) {
        progress_store_tx_unlock();
        return JOB_FATAL;
    }
    job_result_t r = stage_run_once(g_stage, db);
    progress_store_tx_unlock();
    return r;
}

STAGE_DRAIN_IMPL(tip_finalize)

void tip_finalize_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_utxo_counter = NULL;
    g_utxo_counter_user = NULL;
    atomic_store(&g_finalized_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_reorg_detected_total, (uint64_t)0);
    atomic_store(&g_utxo_count_diverged_total, (uint64_t)0);
    atomic_store(&g_precondition_failed_total, (uint64_t)0);
    atomic_store(&g_total_work_added_high, (uint64_t)0);
    atomic_store(&g_total_work_added_low, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    atomic_store(&g_last_precondition_height, (int64_t)-1);
    zcl_mutex_lock(&g_block_reason_mu);
    g_last_precondition_reason[0] = '\0';
    zcl_mutex_unlock(&g_block_reason_mu);
    zcl_mutex_destroy(&g_last_advance_hash_mu);
    zcl_mutex_destroy(&g_block_reason_mu);
    pthread_mutex_unlock(&g_lock);
}

void tip_finalize_stage_set_authoritative_tip(int height,
                                              const uint8_t hash[32])
{
    update_last_advance(height, hash);
    sqlite3 *db = progress_store_db();
    if (db && g_stage) {
        progress_store_tx_lock();
        (void)anchor_cursor_to_authority(db, height, hash, false,
                                         "trusted_tip");
        progress_store_tx_unlock();
    }
}

bool tip_finalize_stage_finalized_tip_at(sqlite3 *db, int height,
                                         uint8_t out_hash[32])
{
    if (!db || !out_hash || height < 0)
        return false;
    progress_store_tx_lock();
    struct finalized_tip_row row;
    if (!finalized_tip_row_at(db, height, &row)) {
        progress_store_tx_unlock();
        return false;
    }
    if (!row.found || !row.ok || !row.has_tip_hash) {
        progress_store_tx_unlock();
        return false;
    }
    memcpy(out_hash, row.tip_hash.data, 32);
    progress_store_tx_unlock();
    return true;
}

bool tip_finalize_stage_seed_anchor(int height, const uint8_t hash[32])
{
    if (height < 0 || !hash)
        return false;

    sqlite3 *db = progress_store_db();
    if (!db) {
        /* Not wired (very early boot, or unit tests without a progress
         * store). The cold-start seed is best-effort until the stage is
         * available. */
        return false;
    }
    progress_store_tx_lock();
    if (!ensure_log_schema(db)) {
        progress_store_tx_unlock();
        return false;
    }

    struct uint256 tip_hash;
    memcpy(tip_hash.data, hash, 32);

    /* Snapshot/trusted anchors have no per-block work or UTXO delta. */
    if (!log_insert(db, height, "anchor", true, NULL, 0, 0, &tip_hash)) {
        progress_store_tx_unlock();
        return false;
    }

    if (!stage_anchor_upstream_cursors_to(db, (uint64_t)height + 1u,
                                          STAGE_NAME, "seed_anchor")) {
        progress_store_tx_unlock();
        return false;
    }

    /* Resume after the anchor; rebuild reads cursor-1 == anchor. */
    if (g_stage && !stage_set_cursor(g_stage, db, (uint64_t)height + 1)) {
        LOG_WARN("tip_finalize",
                 "[tip_finalize] anchor seed: cursor stamp to %d failed",
                 height + 1);
        progress_store_tx_unlock();
        return false;
    }

    /* Publish immediately, not only after the next boot. */
    update_last_advance(height, hash);
    progress_store_tx_unlock();
    return true;
}

void tip_finalize_stage_set_utxo_counter(tip_finalize_utxo_count_fn fn,
                                         void *user)
{
    pthread_mutex_lock(&g_lock);
    g_utxo_counter = fn;
    g_utxo_counter_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t tip_finalize_stage_cursor(void) { return g_stage ? stage_cursor(g_stage) : 0; }
int64_t tip_finalize_stage_last_height(void) { return atomic_load(&g_last_advance_height); }
uint64_t tip_finalize_stage_finalized_total(void) { return atomic_load(&g_finalized_total); }
uint64_t tip_finalize_stage_upstream_failed_total(void) { return atomic_load(&g_upstream_failed_total); }
uint64_t tip_finalize_stage_reorg_detected_total(void) { return atomic_load(&g_reorg_detected_total); }
uint64_t tip_finalize_stage_utxo_count_diverged_total(void) { return atomic_load(&g_utxo_count_diverged_total); }
uint64_t tip_finalize_stage_precondition_failed_total(void) { return atomic_load(&g_precondition_failed_total); }
uint64_t tip_finalize_stage_total_work_added_high(void) { return atomic_load(&g_total_work_added_high); }
uint64_t tip_finalize_stage_total_work_added_low(void) { return atomic_load(&g_total_work_added_low); }

bool tip_finalize_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "finalized_total",
                      (int64_t)atomic_load(&g_finalized_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "reorg_detected_total",
                      (int64_t)atomic_load(&g_reorg_detected_total));
    json_push_kv_int (out, "utxo_count_diverged_total",
                      (int64_t)atomic_load(&g_utxo_count_diverged_total));
    json_push_kv_int (out, "precondition_failed_total",
                      (int64_t)atomic_load(&g_precondition_failed_total));
    json_push_kv_int (out, "last_precondition_height",
                      atomic_load(&g_last_precondition_height));
    {
        /* g_block_reason_mu is only live while the stage is (init..shutdown);
         * guard the read so a dump before init / after shutdown is safe. */
        char reason_buf[40] = "";
        if (g_stage) {
            zcl_mutex_lock(&g_block_reason_mu);
            snprintf(reason_buf, sizeof reason_buf, "%s",
                     g_last_precondition_reason);
            zcl_mutex_unlock(&g_block_reason_mu);
        }
        json_push_kv_str(out, "last_precondition_reason", reason_buf);
    }
    json_push_kv_int (out, "total_work_added_high",
                      (int64_t)atomic_load(&g_total_work_added_high));
    json_push_kv_int (out, "total_work_added_low",
                      (int64_t)atomic_load(&g_total_work_added_low));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "tip_finalize_log") : 0);
    stage_dump_counters(out, g_stage);
    return true;
}
