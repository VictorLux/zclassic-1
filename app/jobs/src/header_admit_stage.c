/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_admit_stage — implementation. See jobs/header_admit_stage.h.
 *
 * Single-process singleton. The F-2 stage primitive does all the
 * cursor / replay heavy lifting; this module is just the step body and
 * the schema-bootstrap glue for the `header_admit_log` table that lives
 * in progress.kv alongside `stage_cursor`. */

#include "jobs/header_admit_stage.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "jobs/stage_helpers.h"
#include "models/header_admit_log.h"
#include "platform/time_compat.h"
#include "services/cutover_modes.h"
#include "services/header_admit_inbox.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_NAME       "header_admit"

/* Cap the reorg-rewind backward scan. Normal reorgs are 1-6 blocks; a
 * deep stale divergence must not pin the CPU walking back across 3.1M
 * heights every supervisor tick. Mirrors HEADER_ADMIT_DIFF_MAX_RANGE. */
#define HEADER_ADMIT_REORG_REWIND_MAX_DEPTH  10000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static _Atomic uint64_t g_admitted_total = 0;
static _Atomic uint64_t g_inbox_drained_total = 0;
static _Atomic uint64_t g_inbox_logged_total = 0;
static _Atomic uint64_t g_reorg_rewind_total = 0;
static _Atomic uint64_t g_header_event_emit_total = 0;
static _Atomic uint64_t g_header_event_emit_fail_total = 0;
static _Atomic int64_t  g_last_admit_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
#ifdef ZCL_TESTING
static header_admit_authoritative_hook g_authoritative_hook = NULL;
static void *g_authoritative_hook_user = NULL;
#endif

MAILBOX_DEFINE(header_admit, struct header_admit_msg,
               HEADER_ADMIT_INBOX_CAPACITY)

/* ── Step body ─────────────────────────────────────────────────────── */

/* Write one header_admit_log row through the AR lifecycle (Law 2: the
 * HeaderAdmitLog model is the only writer of its table). `db` is the
 * progress.kv handle. */
static bool log_insert(sqlite3 *db, int height,
                        const struct uint256 *hash,
                        const struct uint256 *parent_hash)
{
    struct db_header_admit_log row = {
        .height      = (int64_t)height,
        .has_parent  = (parent_hash != NULL),
        .admitted_at = platform_time_wall_unix(),
    };
    memcpy(row.hash, hash->data, 32);
    if (parent_hash)
        memcpy(row.parent_hash, parent_hash->data, 32);

    if (!db_header_admit_log_save(db, &row)) {
        LOG_WARN("header_admit", "[header_admit] log save height=%d failed", height);
        return false;
    }
    return true;
}

static void handle_header_admit_msg(const struct header_admit_msg *m)
{
    if (!m) return;

    atomic_fetch_add(&g_inbox_drained_total, 1);

    struct main_state *ms = g_ms;
    sqlite3 *db = progress_store_db();
    if (!ms || !db || m->height < 0 || m->height > INT32_MAX)
        return;

    struct block_index *bi = active_chain_at(&ms->chain_active,
                                             (int)m->height);
    if (!bi || !bi->phashBlock)
        return;

    if (memcmp(bi->phashBlock->data, m->hash.data, 32) != 0) {
        LOG_WARN("header_admit", "[header_admit] inbox hash mismatch height=%lld peer=%u", (long long)m->height, m->peer_id);
        return;
    }

    const struct uint256 *parent_hash = NULL;
    if (m->height > 0) {
        if (!bi->pprev || !bi->pprev->phashBlock)
            return;
        parent_hash = bi->pprev->phashBlock;
    }

    if (log_insert(db, (int)m->height, bi->phashBlock, parent_hash))
        atomic_fetch_add(&g_inbox_logged_total, 1);
}

static bool authoritative_admit(struct main_state *ms, struct block_index *bi)
{
#ifdef ZCL_TESTING
    if (g_authoritative_hook)
        return g_authoritative_hook(ms, bi, g_authoritative_hook_user);
#endif

    if (!ms || !bi || !bi->phashBlock)
        return false;

    struct block_index *mapped =
        block_map_find(&ms->map_block_index, bi->phashBlock);
    if (mapped && mapped != bi)
        return false;

    if ((bi->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
        bi->nStatus = (bi->nStatus & ~BLOCK_VALID_MASK) |
                      BLOCK_VALID_TREE;
    return true;
}

/* ── EV_BLOCK_HEADER emit (2nd emitter; mirrors block_index_db.c) ────────
 *
 * The legacy LevelDB writer (lib/storage/src/block_index_db.c
 * emit_block_header_event) is today the SOLE feed of block_index_projection.
 * Single-engine BLOCKER 1 PIECE 1: when header_admit is AUTHORITATIVE it
 * becomes a second, independent emitter so the projection survives the
 * eventual deletion of the legacy writer. EV_BLOCK_HEADER into
 * block_index_projection is idempotent (INSERT OR REPLACE keyed on hash),
 * so emitting alongside the legacy path is harmless.
 *
 * Sources its scalars from the in-memory `struct block_index` (rather than
 * the disk_block_index the legacy path serializes). hashPrev is the parent's
 * phashBlock (all-zero for genesis). Best-effort/counted, never fatal —
 * exactly the block_index_db.c semantics. */
static void emit_block_header_event_from_bi(const struct block_index *bi)
{
    if (!bi || !bi->phashBlock)
        return;

    event_log_t *log = event_log_singleton();
    if (!log) {
        /* Not wired yet (very early boot, or tests). The projection
         * catches up once boot completes — not a hard failure. */
        return;
    }

    if (bi->nSolutionSize > EV_BLOCK_HEADER_MAX_SOLUTION) {
        LOG_WARN("header_admit",
                 "[header_admit] header emit: solution size %zu > max %u "
                 "for h=%d; skipping",
                 bi->nSolutionSize, (unsigned)EV_BLOCK_HEADER_MAX_SOLUTION,
                 bi->nHeight);
        atomic_fetch_add_explicit(&g_header_event_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }

    struct ev_block_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.hash, bi->phashBlock->data, 32);
    if (bi->pprev && bi->pprev->phashBlock)
        memcpy(h.hashPrev, bi->pprev->phashBlock->data, 32);
    /* else: genesis — hashPrev stays all-zero (memset above) */
    h.height        = bi->nHeight;
    h.nStatus       = bi->nStatus;
    h.nFile         = bi->nFile;
    h.nDataPos      = bi->nDataPos;
    h.nUndoPos      = bi->nUndoPos;
    h.nTime         = bi->nTime;
    h.nBits         = bi->nBits;
    memcpy(h.nNonce, bi->nNonce.data, 32);
    memcpy(h.hashMerkleRoot, bi->hashMerkleRoot.data, 32);
    memcpy(h.hashFinalSaplingRoot, bi->hashFinalSaplingRoot.data, 32);
    h.nVersion      = bi->nVersion;
    h.nTx           = bi->nTx;
    h.nSolutionSize = (uint16_t)bi->nSolutionSize;

    size_t bufcap = ev_block_header_wire_size(h.nSolutionSize);
    uint8_t stackbuf[256 + 1344];  /* fixed 200 + max solution 1344 */
    if (bufcap > sizeof(stackbuf)) {
        /* Shouldn't happen — capped above. Defensive bail. */
        atomic_fetch_add_explicit(&g_header_event_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }
    size_t written = 0;
    if (!ev_block_header_serialize(&h, bi->nSolution, stackbuf, bufcap,
                                   &written)) {
        LOG_WARN("header_admit",
                 "[header_admit] header emit: serialize failed h=%d",
                 bi->nHeight);
        atomic_fetch_add_explicit(&g_header_event_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }

    uint64_t off = event_log_append(log, EV_BLOCK_HEADER, stackbuf, written);
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_header_event_emit_fail_total, 1,
                                  memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&g_header_event_emit_total, 1,
                              memory_order_relaxed);
}

/* ── Reorg-rewind (mirrors tip_finalize_stage.c rewind) ─────────────── */

/* Read the header_admit_log row hash at `height` and compare it to the
 * active chain's hash at that height. `out_known` is false (a no-op) when
 * the log has no row there OR the chain has no block there; otherwise
 * `out_matches` reflects whether the bytes are equal. Returns false only
 * on a SQL-prepare failure (treated as fatal by the caller). Reuses the
 * SELECT-hash pattern of header_admit_stage_has_record (above). */
static bool log_row_active_match(sqlite3 *db, int height,
                                 bool *out_known, bool *out_matches)
{
    *out_known   = false;
    *out_matches = false;
    if (!db || height < 0)
        return true;

    struct main_state *ms = g_ms;
    struct block_index *bi =
        ms ? active_chain_at(&ms->chain_active, height) : NULL;
    if (!bi || !bi->phashBlock)
        return true;  /* chain has no block here → no-op (out_known=false) */

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hash FROM header_admit_log WHERE height=?",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("header_admit", "[header_admit] rewind prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, height);
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        const void *blob = sqlite3_column_blob(st, 0);
        int nb = sqlite3_column_bytes(st, 0);
        if (blob && nb == 32) {
            *out_known   = true;
            *out_matches = (memcmp(blob, bi->phashBlock->data, 32) == 0);
        }
    }
    sqlite3_finalize(st);
    return true;
}

/* Detect a reorg below the cursor and rewind to the fork point so the
 * stale rows get re-admitted (INSERT OR REPLACE) with the canonical
 * hashes on the forward re-walk.
 *
 * Unlike tip_finalize — whose reorg always touches the tip — a
 * header_admit divergence can sit BELOW a matching tip (the live case:
 * cursor=3129674, tip rows at 3129672/3129673 match, but 3129671 holds a
 * stale pre-reorg hash). A tip-only `cursor-1` check would miss it. So we
 * scan the recent window [cursor-2 .. floor] (capped) and rewind to the
 * DEEPEST divergent height found — re-admitting it and everything above.
 *
 * The scan is bounded by HEADER_ADMIT_REORG_REWIND_MAX_DEPTH so a deep
 * stale fork can't pin the CPU. A height whose log row matches, or whose
 * chain has no block (out_known=false — the LOG_AHEAD shrink case), is a
 * no-op and never triggers a rewind.
 *
 * SHADOW-only: touches only the stage cursor and the header_admit_log;
 * never the legacy block_index. Returns false (→ JOB_FATAL) only on a
 * SQL/persist failure. */
static bool rewind_cursor_if_active_chain_reorged(sqlite3 *db)
{
    if (!g_stage || !g_ms)
        return true;

    uint64_t cursor = stage_cursor_persisted(db, STAGE_NAME, STAGE_NAME);
    if (cursor == 0)
        return true;
    if (cursor > (uint64_t)INT32_MAX) {
        LOG_WARN("header_admit", "[header_admit] reorg rewind cursor too large: %llu", (unsigned long long)cursor);
        return false;
    }

    /* Scan the recent window below the cursor for the deepest height
     * whose logged hash no longer matches the active chain. */
    int floor_h = (int)cursor - HEADER_ADMIT_REORG_REWIND_MAX_DEPTH;
    if (floor_h < 0) floor_h = 0;
    int deepest_divergent = -1;
    for (int h = (int)cursor - 1; h >= floor_h; h--) {
        bool known = false, matches = false;
        if (!log_row_active_match(db, h, &known, &matches))
            return false;
        if (known && !matches)
            deepest_divergent = h;  /* keep going: find the lowest one */
    }
    if (deepest_divergent < 0)
        return true;  /* recent window is consistent → no rewind */

    if (deepest_divergent == floor_h && floor_h > 0)
        LOG_WARN("header_admit", "[header_admit] reorg rewind cap hit (depth=%d): divergence may extend below floor=%d", HEADER_ADMIT_REORG_REWIND_MAX_DEPTH, floor_h);

    uint64_t rewind_to = (uint64_t)deepest_divergent;
    if (rewind_to >= cursor)
        return true;

    if (!stage_set_cursor(g_stage, db, rewind_to)) {
        LOG_WARN("header_admit", "[header_admit] reorg rewind failed from=%llu to=%llu", (unsigned long long)cursor, (unsigned long long)rewind_to);
        return false;
    }

    atomic_fetch_add(&g_reorg_rewind_total, 1);
    atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
    event_emitf(EV_REORG_START, 0,
                "header_admit reorg_cursor_rewind from=%llu to=%llu",
                (unsigned long long)cursor,
                (unsigned long long)rewind_to);
    return true;
}

static job_result_t step_admit(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;

    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !bi->phashBlock) return JOB_IDLE;

    const struct uint256 *parent_hash = NULL;
    if (next_h > 0) {
        if (!bi->pprev || !bi->pprev->phashBlock) {
            blocker_init(&c->blocker, "header_admit",
                          "missing_parent",
                          BLOCKER_PERMANENT,
                          "block_index entry has no pprev linkage");
            atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
            return JOB_BLOCKED;
        }
        parent_hash = bi->pprev->phashBlock;
    }

    if (header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE) {
        if (!authoritative_admit(ms, bi)) {
            LOG_WARN("header_admit", "[header_admit] authoritative admit failed height=%d", next_h);
            return JOB_FATAL;
        }
        /* Single-engine PIECE 1: 2nd EV_BLOCK_HEADER emitter, alongside
         * the legacy block_index_db.c writer. Idempotent (INSERT OR
         * REPLACE in block_index_projection); best-effort, never fatal —
         * emit AFTER the VALID_TREE promotion so the persisted nStatus
         * reflects it. */
        emit_block_header_event_from_bi(bi);
    }

    if (!log_insert(db, next_h, bi->phashBlock, parent_hash))
        return JOB_FATAL;

    c->cursor_out = c->cursor_in + 1;
    atomic_fetch_add(&g_admitted_total, 1);
    atomic_store(&g_last_admit_height, (int64_t)next_h);
    return JOB_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

void header_admit_set_mode(header_admit_mode_t mode)
{
    cutover_modes_set_header_admit(
        mode == HEADER_ADMIT_MODE_AUTHORITATIVE
            ? CUTOVER_STAGE_MODE_AUTHORITATIVE
            : CUTOVER_STAGE_MODE_SHADOW);
}

header_admit_mode_t header_admit_get_mode(void)
{
    return cutover_modes_get_header_admit() ==
               CUTOVER_STAGE_MODE_AUTHORITATIVE
        ? HEADER_ADMIT_MODE_AUTHORITATIVE
        : HEADER_ADMIT_MODE_SHADOW;
}

#ifdef ZCL_TESTING
void header_admit_stage_set_authoritative_hook(
    header_admit_authoritative_hook hook,
    void *user)
{
    g_authoritative_hook = hook;
    g_authoritative_hook_user = user;
}
#endif

bool header_admit_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("header_admit", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("header_admit",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    /* Idempotent: same ms, already initialised → success. */
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("header_admit",
                "init: already bound to a different main_state");
        return true;
    }

    if (!db_header_admit_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_admit, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("header_admit", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("header_admit", "[header_admit] stage initialised (mode=%s)", header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE ? "authoritative" : "shadow");
    return true;
}

job_result_t header_admit_stage_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;
    if (!rewind_cursor_if_active_chain_reorged(db))
        return JOB_FATAL;
    (void)mailbox_header_admit_drain(handle_header_admit_msg);
    return stage_run_once(g_stage, db);
}

STAGE_DRAIN_IMPL(header_admit)

void header_admit_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    /* Reset per-init observability state. The persisted cursor in
     * progress.kv is preserved (that's the whole point of the saga);
     * what we reset is what would be misleading across re-inits —
     * lifetime counters that should restart with each bind. */
    atomic_store(&g_admitted_total, (uint64_t)0);
    atomic_store(&g_inbox_drained_total, (uint64_t)0);
    atomic_store(&g_inbox_logged_total, (uint64_t)0);
    atomic_store(&g_reorg_rewind_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_fail_total, (uint64_t)0);
    atomic_store(&g_last_admit_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
#ifdef ZCL_TESTING
    g_authoritative_hook = NULL;
    g_authoritative_hook_user = NULL;
#endif
    pthread_mutex_unlock(&g_lock);
}

uint64_t header_admit_stage_cursor(void)
{
    if (!g_stage)
        return 0;
    uint64_t cached = stage_cursor(g_stage);
    sqlite3 *db = progress_store_db();
    if (!db)
        return cached;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name=?",
            -1, &st, NULL) != SQLITE_OK)
        return cached;
    sqlite3_bind_text(st, 1, STAGE_NAME, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        cached = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return cached;
}

uint64_t header_admit_stage_admitted_total(void)
{
    return atomic_load(&g_admitted_total);
}

uint64_t header_admit_stage_reorg_rewind_total(void)
{
    return atomic_load(&g_reorg_rewind_total);
}

bool header_admit_stage_has_record(int32_t height,
                                   const struct uint256 *hash)
{
    if (height < 0 || !hash)
        return false;

    sqlite3 *db = progress_store_db();
    if (!db)
        return false;

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM header_admit_log WHERE height=?",
        -1, &st, NULL);
    if (rc != SQLITE_OK)
        return false;

    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        const void *blob = sqlite3_column_blob(st, 0);
        int nb = sqlite3_column_bytes(st, 0);
        found = (blob && nb == 32 &&
                 memcmp(blob, hash->data, 32) == 0);
    }
    sqlite3_finalize(st);
    return found;
}

bool header_admit_stage_dump_state_json(struct json_value *out,
                                         const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "admitted_total",
                      (int64_t)atomic_load(&g_admitted_total));
    json_push_kv_int (out, "inbox_drained_total",
                      (int64_t)atomic_load(&g_inbox_drained_total));
    json_push_kv_int (out, "inbox_logged_total",
                      (int64_t)atomic_load(&g_inbox_logged_total));
    json_push_kv_int (out, "reorg_rewind_total",
                      (int64_t)atomic_load(&g_reorg_rewind_total));
    json_push_kv_int (out, "header_event_emit_total",
                      (int64_t)atomic_load(&g_header_event_emit_total));
    json_push_kv_int (out, "header_event_emit_fail_total",
                      (int64_t)atomic_load(&g_header_event_emit_fail_total));
    json_push_kv_int (out, "last_admit_height",
                      atomic_load(&g_last_admit_height));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_str (out, "mode",
                      header_admit_get_mode() ==
                          HEADER_ADMIT_MODE_AUTHORITATIVE
                              ? "authoritative"
                              : "shadow");
    if (g_stage) {
        json_push_kv_int(out, "advanced_count",
                         (int64_t)stage_advanced_count(g_stage));
        json_push_kv_int(out, "blocked_count",
                         (int64_t)stage_blocked_count(g_stage));
        json_push_kv_int(out, "idle_count",
                         (int64_t)stage_idle_count(g_stage));
        json_push_kv_int(out, "error_count",
                         (int64_t)stage_error_count(g_stage));
    }
    return true;
}

/* ── S-11 mini-diff harness ─────────────────────────────────────────── */

/* SELECT MAX(height) FROM header_admit_log. Returns -1 if empty. */
static int32_t log_max_height(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT MAX(height) FROM header_admit_log", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("header_admit", "[header_admit] diff: prepare MAX(height) failed: %s", sqlite3_errmsg(db));
        return -1;  /* raw-return-ok:diagnostic-treats-as-empty */
    }
    int32_t out = -1;
    if (sqlite3_step(st) == SQLITE_ROW &&  // raw-sql-ok:kernel-primitive
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
        out = (int32_t)sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

static void diff_sample_record(struct header_admit_diff_report *r,
                                int32_t h,
                                const uint8_t *log_hash, bool log_present,
                                const uint8_t *chain_hash, bool chain_present)
{
    if (r->sample_count >= HEADER_ADMIT_DIFF_MAX_SAMPLES) return;
    struct header_admit_diff_sample *s = &r->samples[r->sample_count++];
    s->height        = h;
    s->log_present   = log_present;
    s->chain_present = chain_present;
    if (log_present && log_hash)   memcpy(s->log_hash,   log_hash,   32);
    else                           memset(s->log_hash,   0, 32);
    if (chain_present && chain_hash) memcpy(s->chain_hash, chain_hash, 32);
    else                             memset(s->chain_hash, 0, 32);
}

bool header_admit_stage_diff(int32_t start_h, int32_t end_h,
                              struct header_admit_diff_report *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->first_divergent_height = -1;
    out->log_max_height         = -1;
    out->chain_tip_height       = -1;

    /* Not-ready guard: stage uninit or progress.kv closed. The report
     * is still populated with sensible defaults so callers can render. */
    struct main_state *ms = g_ms;
    sqlite3 *db = progress_store_db();
    if (!g_stage || !ms || !db) {
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        out->start_height = (start_h < 0) ? 0 : start_h;
        out->end_height   = (end_h   < 0) ? 0 : end_h;
        return true;
    }

    out->cursor             = (int32_t)stage_cursor(g_stage);
    out->log_max_height     = log_max_height(db);
    out->chain_tip_height   = active_chain_height(&ms->chain_active);

    /* Resolve auto-bounds. A fully automatic diff should answer the
     * operator question "does the recent stage path match the active
     * chain?" rather than burning the capped range on genesis. */
    int32_t s = (start_h < 0) ? 0 : start_h;
    int32_t e = end_h;
    if (e < 0) {
        int32_t a = out->log_max_height;
        int32_t b = out->chain_tip_height;
        if (a < 0 && b < 0)      e = -1;
        else if (a < 0)          e = b;
        else if (b < 0)          e = a;
        else                     e = (a < b) ? a : b;
    }
    if (start_h < 0 && e >= HEADER_ADMIT_DIFF_MAX_RANGE)
        s = e - HEADER_ADMIT_DIFF_MAX_RANGE + 1;

    if (e < s) {
        out->status       = HEADER_ADMIT_DIFF_EMPTY;
        out->start_height = s;
        out->end_height   = e;
        return true;
    }

    /* Hard cap the range. */
    int64_t span = (int64_t)e - (int64_t)s + 1;
    if (span > HEADER_ADMIT_DIFF_MAX_RANGE) {
        e = s + HEADER_ADMIT_DIFF_MAX_RANGE - 1;
        span = HEADER_ADMIT_DIFF_MAX_RANGE;
    }
    out->start_height = s;
    out->end_height   = e;

    /* Load all log rows in the range into a packed array indexed by
     * (height - s). Bounded: span <= HEADER_ADMIT_DIFF_MAX_RANGE → ≤ 330 KB. */
    struct row {
        uint8_t hash[32];
        bool    present;
    };
    struct row *rows = zcl_calloc((size_t)span, sizeof(struct row),
                                   "header_admit_diff_rows");
    if (!rows) {
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        return true;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT height, hash FROM header_admit_log "
        "WHERE height BETWEEN ? AND ? ORDER BY height",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        free(rows);
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        return true;
    }
    sqlite3_bind_int(st, 1, s);
    sqlite3_bind_int(st, 2, e);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        int32_t h = sqlite3_column_int(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int nb = sqlite3_column_bytes(st, 1);
        if (h < s || h > e || !blob || nb != 32) continue;
        struct row *r = &rows[h - s];
        memcpy(r->hash, blob, 32);
        r->present = true;
    }
    sqlite3_finalize(st);

    /* Walk the range. For each height, compare log row vs in-memory chain. */
    for (int32_t h = s; h <= e; h++) {
        const struct row *r = &rows[h - s];
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        bool chain_present = (bi != NULL && bi->phashBlock != NULL);
        const uint8_t *chain_hash =
            chain_present ? bi->phashBlock->data : NULL;

        if (!r->present && !chain_present) continue;  /* both missing → skip */

        out->checked_count++;
        if (r->present && chain_present) {
            if (memcmp(r->hash, chain_hash, 32) == 0) {
                out->match_count++;
            } else {
                out->mismatch_count++;
                if (out->first_divergent_height < 0)
                    out->first_divergent_height = h;
                diff_sample_record(out, h, r->hash, true, chain_hash, true);
            }
        } else if (r->present && !chain_present) {
            /* log has it, chain doesn't — chain shrank or reorged through. */
            out->missing_in_chain_count++;
            if (out->first_divergent_height < 0)
                out->first_divergent_height = h;
            diff_sample_record(out, h, r->hash, true, NULL, false);
        } else {
            /* chain has it, log doesn't — S-2 cursor lag. */
            out->missing_in_log_count++;
            if (out->first_divergent_height < 0)
                out->first_divergent_height = h;
            diff_sample_record(out, h, NULL, false, chain_hash, true);
        }
    }

    free(rows);

    /* Status: hash mismatches dominate; reorg-style log-ahead next;
     * normal cursor-lag third; otherwise converged or empty. */
    if (out->mismatch_count > 0)             out->status = HEADER_ADMIT_DIFF_DIVERGENT;
    else if (out->missing_in_chain_count > 0) out->status = HEADER_ADMIT_DIFF_LOG_AHEAD;
    else if (out->missing_in_log_count > 0)   out->status = HEADER_ADMIT_DIFF_CHAIN_AHEAD;
    else if (out->checked_count > 0)          out->status = HEADER_ADMIT_DIFF_CONVERGED;
    else                                       out->status = HEADER_ADMIT_DIFF_EMPTY;

    return true;
}
