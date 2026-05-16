/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain state repository — implementation. See header for the design
 * rationale and the 2026-04-10 incident that motivated this service. */

#include "services/chain_state_repository.h"

#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

/* ── Default tunables ───────────────────────────────────────── */
#define CSR_DEFAULT_MAX_ORPHAN_ROWS   1000
#define CSR_DEFAULT_STALE_INDEX_GAP   100

/* ── Result names (for events / logs) ───────────────────────── */
const char *csr_result_name(enum csr_result r)
{
    switch (r) {
    case CSR_OK:                          return "ok";
    case CSR_REJECTED_NULL_INPUT:         return "null_input";
    case CSR_REJECTED_NOT_INITIALIZED:    return "not_initialized";
    case CSR_REJECTED_TIP_NOT_IN_INDEX:   return "tip_not_in_index";
    case CSR_REJECTED_HASH_MISMATCH:      return "hash_mismatch";
    case CSR_REJECTED_MISSING_PREV:       return "missing_prev";
    case CSR_REJECTED_STALE_INDEX:        return "stale_index";
    case CSR_REJECTED_UTXO_DELTA_TOO_BIG: return "utxo_delta_too_big";
    case CSR_REJECTED_COINS_MISMATCH:     return "coins_mismatch";
    case CSR_REJECTED_HEADER_REGRESSION:  return "header_regression";
    case CSR_REJECTED_ROLLBACK_AUTH:      return "rollback_auth";
    case CSR_REJECTED_PERSIST:            return "persist";
    case CSR_REJECTED_OOM:                return "oom";
    case CSR_NUM_RESULTS:                 break;
    }
    return "unknown";
}

/* ── SQLite helpers ─────────────────────────────────────────────
 * The repository must work even when ndb is NULL (unit tests, early
 * boot phases). All helpers return false / -1 when there is no DB,
 * and the caller treats that as "skip this cross-check". */

static int64_t csr_sqlite_max_block_height(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !ndb->db)
        LOG_RETURN(-1, "csr", "ndb not available (null or closed)");
    sqlite3_stmt *st = NULL;
    int64_t result = -1;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT MAX(height) FROM blocks", -1, &st, NULL) == SQLITE_OK) {
        if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW &&
            sqlite3_column_type(st, 0) != SQLITE_NULL) {
            result = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    return result;
}

/* Look up a block hash in the SQLite blocks table. Returns:
 *    1 = found, *out_height set
 *    0 = not found
 *   -1 = no DB available (caller should skip the cross-check) */
static int csr_sqlite_block_height(struct node_db *ndb,
                                    const struct uint256 *hash,
                                    int64_t *out_height)
{
    if (!ndb || !ndb->open || !ndb->db || !hash || !out_height)
        LOG_ERR("csr", "invalid args (ndb=%p hash=%p out=%p)",
                (void *)ndb, (void *)hash, (void *)out_height);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT height FROM blocks WHERE hash=? LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_ERR("csr", "sqlite3_prepare_v2 failed: %s", sqlite3_errmsg(ndb->db));
    }
    sqlite3_bind_blob(st, 1, hash->data, 32, SQLITE_STATIC);
    int rc = AR_STEP_ROW_READONLY(st);
    int result;
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int64(st, 0);
        result = 1;
    } else {
        result = 0;
    }
    sqlite3_finalize(st);
    return result;
}

static int64_t csr_sqlite_utxo_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        LOG_RETURN(-1, "csr", "ndb not available");
    return node_db_utxo_count(ndb);
}

/* ── Validation (caller holds csr->lock) ─────────────────────── */

static bool csr_rollback_authorization_valid(
    const struct chain_state_rollback_authorization *auth)
{
    if (!auth)
        return false;
    if (auth->source == CSR_ROLLBACK_SOURCE_NONE)
        return false;
    if (auth->decision != POLICY_ALLOW)
        return false;
    if (!auth->evidence_class || !*auth->evidence_class)
        return false;
    if (!auth->reason || !*auth->reason)
        return false;
    if (auth->from_height >= 0 && auth->to_height >= 0 &&
        auth->from_height > auth->to_height && auth->max_depth >= 0) {
        int64_t depth = auth->from_height - auth->to_height;
        if (depth > auth->max_depth)
            return false;
    }
    return true;
}

static bool csr_commit_has_rollback_authorization(
    const struct chain_state_commit *commit)
{
    return csr_rollback_authorization_valid(
        commit ? commit->rollback_auth : NULL);
}

static enum csr_result csr_validate_locked(
    struct chain_state_repository *csr,
    const struct chain_state_commit *commit)
{
    /* Step 0: structural NULL checks. */
    if (!commit || !commit->new_tip) return CSR_REJECTED_NULL_INPUT;
    if (!commit->reason || !*commit->reason) return CSR_REJECTED_NULL_INPUT;

    struct block_index *new_tip = commit->new_tip;
    if (!new_tip->phashBlock) return CSR_REJECTED_NULL_INPUT;
    if (new_tip->nHeight < 0) return CSR_REJECTED_NULL_INPUT;

    /* Step 1: the proposed coins_best_block must equal the tip hash.
     * If they disagree, the caller has a bug — refuse rather than
     * silently picking one. This is the fundamental invariant. */
    if (memcmp(commit->new_coins_best.data,
               new_tip->phashBlock->data, 32) != 0) {
        return CSR_REJECTED_COINS_MISMATCH;
    }

    /* Step 2: the new tip must be registered in the in-memory block
     * map. block_map_find returns the canonical block_index pointer
     * for that hash; if it isn't equal to new_tip then the caller is
     * passing a stale or stack-allocated index that the rest of the
     * system doesn't know about. */
    if (csr->block_map) {
        struct block_index *found =
            block_map_find(csr->block_map, new_tip->phashBlock);
        if (!found) return CSR_REJECTED_TIP_NOT_IN_INDEX;
        if (found != new_tip) return CSR_REJECTED_HASH_MISMATCH;
    }

    /* Step 3: pprev must also be in the index (or be NULL for genesis). */
    if (csr->block_map && new_tip->pprev) {
        if (!new_tip->pprev->phashBlock) return CSR_REJECTED_MISSING_PREV;
        struct block_index *prev =
            block_map_find(csr->block_map, new_tip->pprev->phashBlock);
        if (prev != new_tip->pprev) return CSR_REJECTED_MISSING_PREV;
    }

    /* Step 4: SQLite cross-check on the new tip hash itself. If
     * SQLite knows this hash, the height must agree. This is the
     * direct test for the h=60-vs-h=3M class of bug. */
    int64_t sql_h = -1;
    int hf = csr_sqlite_block_height(csr->ndb, new_tip->phashBlock, &sql_h);
    if (hf == 1 && sql_h != new_tip->nHeight) {
        return CSR_REJECTED_HASH_MISMATCH;
    }

    /* Step 5: SQLite tip vs proposed tip. If SQLite holds blocks far
     * above the proposed tip, this commit is rolling the chain
     * backwards in a way that has historically corrupted the UTXO
     * set. Combined with a non-trivial UTXO row count it is the
     * exact disaster shape from 2026-04-10. */
    int64_t sql_max = csr_sqlite_max_block_height(csr->ndb);
    if (sql_max >= 0 &&
        sql_max - (int64_t)new_tip->nHeight > csr->stale_index_height_gap) {
        int64_t cur_utxos = csr_sqlite_utxo_count(csr->ndb);
        if (cur_utxos > csr->max_utxo_orphan_rows &&
            !csr_commit_has_rollback_authorization(commit)) {
            return CSR_REJECTED_STALE_INDEX;
        }
    }

    /* Step 6: explicit expected_utxo_count check. The caller can
     * pass the count it believes the new tip should imply; if the
     * SQLite count differs by more than 50% we refuse. */
    if (commit->expected_utxo_count > 0) {
        int64_t actual = csr_sqlite_utxo_count(csr->ndb);
        if (actual >= 0) {
            int64_t diff = actual - commit->expected_utxo_count;
            if (diff < 0) diff = -diff;
            int64_t denom = actual > commit->expected_utxo_count
                ? actual : commit->expected_utxo_count;
            if (denom > 0 && diff * 2 > denom) {  /* >50% drift */
                return CSR_REJECTED_UTXO_DELTA_TOO_BIG;
            }
        }
    }

    /* Step 7: orphan-rows guard for backward moves. Even without an
     * obviously stale block_index, we refuse to silently orphan a
     * large UTXO set unless the caller explicitly opted into a
     * rollback with a typed policy authorization. */
    if (csr->chain_active && !csr_commit_has_rollback_authorization(commit)) {
        int cur_h = active_chain_height(csr->chain_active);
        if (cur_h >= 0 && new_tip->nHeight < cur_h) {
            int64_t cur_utxos = csr_sqlite_utxo_count(csr->ndb);
            if (cur_utxos > csr->max_utxo_orphan_rows) {
                return CSR_REJECTED_UTXO_DELTA_TOO_BIG;
            }
        }
    }

    return CSR_OK;
}

static enum csr_result csr_validate_header_locked(
    struct chain_state_repository *csr,
    const struct chain_state_header_commit *commit)
{
    if (!commit || !commit->new_header_tip)
        return CSR_REJECTED_NULL_INPUT;
    if (!commit->reason || !*commit->reason)
        return CSR_REJECTED_NULL_INPUT;

    struct block_index *new_tip = commit->new_header_tip;
    if (!new_tip->phashBlock)
        return CSR_REJECTED_NULL_INPUT;
    if (new_tip->nHeight < 0)
        return CSR_REJECTED_NULL_INPUT;

    if (csr->block_map) {
        struct block_index *found =
            block_map_find(csr->block_map, new_tip->phashBlock);
        if (!found)
            return CSR_REJECTED_TIP_NOT_IN_INDEX;
        if (found != new_tip)
            return CSR_REJECTED_HASH_MISMATCH;
    }

    if (csr->block_map && new_tip->pprev) {
        if (!new_tip->pprev->phashBlock)
            return CSR_REJECTED_MISSING_PREV;
        struct block_index *prev =
            block_map_find(csr->block_map, new_tip->pprev->phashBlock);
        if (prev != new_tip->pprev)
            return CSR_REJECTED_MISSING_PREV;
    }

    if (csr->pindex_best_hdr && *csr->pindex_best_hdr &&
        new_tip->nHeight < (*csr->pindex_best_hdr)->nHeight &&
        !csr_rollback_authorization_valid(commit->rollback_auth)) {
        return CSR_REJECTED_HEADER_REGRESSION;
    }

    return CSR_OK;
}

/* ── Event helpers ──────────────────────────────────────────── */

static void csr_emit_commit_event(struct chain_state_repository *csr,
                                   int from_height,
                                   const struct chain_state_commit *commit)
{
    (void)csr;
    event_emitf(EV_CHAIN_TIP_COMMIT, 0,
                "from=%d to=%d reason=%s",
                from_height,
                commit->new_tip ? commit->new_tip->nHeight : -1,
                commit->reason ? commit->reason : "");
}

static void csr_emit_rejected_event(struct chain_state_repository *csr,
                                     int from_height,
                                     const struct chain_state_commit *commit,
                                     enum csr_result rc)
{
    (void)csr;
    int to_h = (commit && commit->new_tip) ? commit->new_tip->nHeight : -1;
    const char *reason = (commit && commit->reason) ? commit->reason : "";
    event_emitf(EV_CHAIN_TIP_REJECTED, 0,
                "code=%s from=%d to=%d reason=%s",
                csr_result_name(rc), from_height, to_h, reason);
}

/* ── Public API ─────────────────────────────────────────────── */

/* Process-lifetime singleton. Storage is zero-initialized by the C
 * runtime; the mutex is brought to life exactly once by pthread_once
 * the first time csr_instance() is called. Boot wires real pointers
 * later via csr_init(csr_instance(), ...). */
static struct chain_state_repository g_csr_singleton;
static pthread_once_t g_csr_singleton_once = PTHREAD_ONCE_INIT;

static void csr_singleton_once_init(void)
{
    pthread_mutex_init(&g_csr_singleton.lock, NULL);
    g_csr_singleton.max_utxo_orphan_rows  = CSR_DEFAULT_MAX_ORPHAN_ROWS;
    g_csr_singleton.stale_index_height_gap = CSR_DEFAULT_STALE_INDEX_GAP;
    /* initialized stays false until csr_init is called with real
     * pointers — any commit attempt before that returns
     * CSR_REJECTED_NOT_INITIALIZED with no side effects. */
}

struct chain_state_repository *csr_instance(void)
{
    pthread_once(&g_csr_singleton_once, csr_singleton_once_init);
    return &g_csr_singleton;
}

void csr_init(struct chain_state_repository *csr,
              struct block_map        *block_map,
              struct active_chain     *chain_active,
              struct block_index     **pindex_best_hdr,
              struct coins_view_cache *coins_tip,
              struct node_db          *ndb,
              int64_t                 *wallet_scan_h)
{
    if (!csr) return;

    /* Stack-allocated repositories (tests, short-lived helpers) need
     * a full reset and a fresh mutex. The singleton already has its
     * mutex set up by pthread_once — never touch it here, just assign
     * the field pointers under the existing lock so callers that are
     * mid-commit observe a consistent view. */
    if (csr != &g_csr_singleton) {
        memset(csr, 0, sizeof(*csr));
        pthread_mutex_init(&csr->lock, NULL);
    }

    pthread_mutex_lock(&csr->lock);
    csr->block_map        = block_map;
    csr->chain_active     = chain_active;
    csr->pindex_best_hdr  = pindex_best_hdr;
    csr->coins_tip        = coins_tip;
    csr->ndb              = ndb;
    csr->wallet_scan_h    = wallet_scan_h;
    csr->max_utxo_orphan_rows  = CSR_DEFAULT_MAX_ORPHAN_ROWS;
    csr->stale_index_height_gap = CSR_DEFAULT_STALE_INDEX_GAP;
    csr->commits_ok = 0;
    memset(csr->commits_rejected, 0, sizeof(csr->commits_rejected));
    csr->initialized = true;
    pthread_mutex_unlock(&csr->lock);
}

void csr_free(struct chain_state_repository *csr)
{
    if (!csr || !csr->initialized) return;
    /* Singleton lives for the whole process — its mutex is owned by
     * pthread_once and must not be destroyed. Just clear the flag so
     * subsequent commits are rejected cleanly. */
    if (csr != &g_csr_singleton) {
        pthread_mutex_destroy(&csr->lock);
    }
    csr->initialized = false;
}

void csr_set_max_utxo_orphan_rows(struct chain_state_repository *csr,
                                   int64_t max_rows)
{
    if (!csr) return;
    pthread_mutex_lock(&csr->lock);
    csr->max_utxo_orphan_rows = max_rows;
    pthread_mutex_unlock(&csr->lock);
}

void csr_set_stale_index_gap(struct chain_state_repository *csr, int gap)
{
    if (!csr) return;
    pthread_mutex_lock(&csr->lock);
    csr->stale_index_height_gap = gap;
    pthread_mutex_unlock(&csr->lock);
}

enum csr_result csr_commit_header_tip(
    struct chain_state_repository *csr,
    const struct chain_state_header_commit *commit)
{
    if (!csr)
        return CSR_REJECTED_NULL_INPUT;
    if (!csr->initialized)
        return CSR_REJECTED_NOT_INITIALIZED;

    pthread_mutex_lock(&csr->lock);
    enum csr_result rc = csr_validate_header_locked(csr, commit);
    if (rc != CSR_OK) {
        csr->commits_rejected[rc]++;
        pthread_mutex_unlock(&csr->lock);
        fprintf(stderr,
                "csr: HEADER_REJECTED code=%s to=%d reason=%s\n",
                csr_result_name(rc),
                (commit && commit->new_header_tip)
                    ? commit->new_header_tip->nHeight : -1,
                (commit && commit->reason) ? commit->reason : "");
        return rc;
    }

    if (csr->pindex_best_hdr)
        *csr->pindex_best_hdr = commit->new_header_tip;
    pthread_mutex_unlock(&csr->lock);

    printf("csr: header tip committed to=%d reason=%s\n",
           commit->new_header_tip->nHeight, commit->reason);
    return CSR_OK;
}

enum csr_result csr_clear_active_tip(
    struct chain_state_repository *csr,
    const struct chain_state_clear_commit *commit)
{
    if (!csr || !commit || !commit->reason)
        return CSR_REJECTED_NULL_INPUT;
    if (!csr->initialized)
        return CSR_REJECTED_NOT_INITIALIZED;
    if (!csr_rollback_authorization_valid(commit->rollback_auth))
        return CSR_REJECTED_ROLLBACK_AUTH;

    pthread_mutex_lock(&csr->lock);

    int from_height = csr->chain_active
        ? active_chain_height(csr->chain_active) : -1;
    if (csr->chain_active) {
        if (!active_chain_set_tip(csr->chain_active, NULL)) {
            csr->commits_rejected[CSR_REJECTED_OOM]++;
            pthread_mutex_unlock(&csr->lock);
            return CSR_REJECTED_OOM;
        }
    }

    csr->commits_ok++;
    event_emitf(EV_CHAIN_TIP_COMMIT, 0,
                "from=%d to=-1 reason=%s", from_height, commit->reason);
    pthread_mutex_unlock(&csr->lock);

    printf("csr: active tip cleared from=%d reason=%s\n",
           from_height, commit->reason);
    return CSR_OK;
}

enum csr_result csr_repair_set_coins_best(
    struct chain_state_repository *csr,
    const struct chain_state_coins_best_repair *repair)
{
    if (!csr || !repair || !repair->reason || !*repair->reason)
        return CSR_REJECTED_NULL_INPUT;
    if (!csr->initialized)
        return CSR_REJECTED_NOT_INITIALIZED;
    if (!csr->coins_tip)
        return CSR_REJECTED_NULL_INPUT;
    if (!csr_rollback_authorization_valid(repair->repair_auth))
        return CSR_REJECTED_ROLLBACK_AUTH;

    pthread_mutex_lock(&csr->lock);
    int from_height = csr->chain_active
        ? active_chain_height(csr->chain_active) : -1;
    coins_view_cache_set_best_block(csr->coins_tip, &repair->new_coins_best);
    csr->commits_ok++;
    event_emitf(EV_CHAIN_TIP_COMMIT, 0,
                "from=%d to=%d reason=%s coins_best_repair=1",
                from_height, from_height, repair->reason);
    pthread_mutex_unlock(&csr->lock);

    printf("csr: coins best repaired active_height=%d reason=%s\n",
           from_height, repair->reason);
    return CSR_OK;
}

enum csr_result csr_commit_tip(struct chain_state_repository *csr,
                                const struct chain_state_commit *commit)
{
    if (!csr) return CSR_REJECTED_NULL_INPUT;
    if (!csr->initialized) return CSR_REJECTED_NOT_INITIALIZED;

    pthread_mutex_lock(&csr->lock);

    int from_height = csr->chain_active
        ? active_chain_height(csr->chain_active) : -1;

    enum csr_result rc = csr_validate_locked(csr, commit);
    if (rc != CSR_OK) {
        csr->commits_rejected[rc]++;
        csr_emit_rejected_event(csr, from_height, commit, rc);
        pthread_mutex_unlock(&csr->lock);
        fprintf(stderr,
                "csr: REJECTED code=%s from=%d to=%d reason=%s\n",
                csr_result_name(rc), from_height,
                (commit && commit->new_tip) ? commit->new_tip->nHeight : -1,
                (commit && commit->reason) ? commit->reason : "");
        return rc;
    }

    printf("[csr] commit_tip h=%d reason=%s\n",
           commit->new_tip->nHeight, commit->reason);

    if (commit->persist_coins_best) {
        if (!csr->ndb || !csr->ndb->open ||
            !node_db_state_set(csr->ndb, "coins_best_block",
                               commit->new_coins_best.data, 32)) {
            csr->commits_rejected[CSR_REJECTED_PERSIST]++;
            csr_emit_rejected_event(csr, from_height, commit,
                                    CSR_REJECTED_PERSIST);
            pthread_mutex_unlock(&csr->lock);
            fprintf(stderr,
                    "csr: REJECTED code=%s from=%d to=%d reason=%s\n",
                    csr_result_name(CSR_REJECTED_PERSIST), from_height,
                    commit->new_tip ? commit->new_tip->nHeight : -1,
                    commit->reason ? commit->reason : "");
            return CSR_REJECTED_PERSIST;
        }
    }

    /* Publish the concrete in-memory view only after optional durable
     * cursor persistence succeeds. The remaining failure point is
     * active_chain_set_tip realloc/OOM, which aborts before the other
     * in-memory sources are touched. */
    if (csr->chain_active) {
        if (!active_chain_set_tip(csr->chain_active, commit->new_tip)) {
            csr->commits_rejected[CSR_REJECTED_OOM]++;
            csr_emit_rejected_event(csr, from_height, commit, CSR_REJECTED_OOM);
            pthread_mutex_unlock(&csr->lock);
            return CSR_REJECTED_OOM;
        }
    }
    if (csr->coins_tip) {
        coins_view_cache_set_best_block(csr->coins_tip,
                                         &commit->new_coins_best);
    }
    if (csr->pindex_best_hdr && commit->update_header_tip) {
        struct block_index *cur_hdr = *csr->pindex_best_hdr;
        if (!cur_hdr || commit->new_tip->nHeight >= cur_hdr->nHeight ||
            csr_commit_has_rollback_authorization(commit)) {
            *csr->pindex_best_hdr = commit->new_tip;
        }
    }
    if (csr->wallet_scan_h && commit->wallet_scan_height >= 0) {
        *csr->wallet_scan_h = commit->wallet_scan_height;
    }

    csr->commits_ok++;
    csr_emit_commit_event(csr, from_height, commit);
    pthread_mutex_unlock(&csr->lock);

    printf("csr: tip committed from=%d to=%d reason=%s\n",
           from_height, commit->new_tip->nHeight, commit->reason);
    return CSR_OK;
}

void csr_snapshot(struct chain_state_repository *csr,
                   struct chain_state_view *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->tip_height    = -1;
    out->header_height = -1;
    out->utxo_count    = -1;
    out->sql_max_height = -1;
    if (!csr || !csr->initialized) return;

    pthread_mutex_lock(&csr->lock);
    if (csr->chain_active) {
        out->tip_height = active_chain_height(csr->chain_active);
        struct block_index *tip = active_chain_tip(csr->chain_active);
        if (tip && tip->phashBlock) out->tip_hash = *tip->phashBlock;
    }
    if (csr->pindex_best_hdr && *csr->pindex_best_hdr) {
        out->header_height = (*csr->pindex_best_hdr)->nHeight;
    }
    if (csr->coins_tip) {
        coins_view_cache_get_best_block(csr->coins_tip, &out->coins_best_block);
    }
    out->utxo_count    = csr_sqlite_utxo_count(csr->ndb);
    out->sql_max_height = csr_sqlite_max_block_height(csr->ndb);
    out->consistent = (memcmp(out->tip_hash.data,
                              out->coins_best_block.data, 32) == 0);
    out->commits_ok = csr->commits_ok;
    out->commits_rejected_total = 0;
    for (int i = 0; i < CSR_NUM_RESULTS; i++)
        out->commits_rejected_total += csr->commits_rejected[i];
    pthread_mutex_unlock(&csr->lock);
}
