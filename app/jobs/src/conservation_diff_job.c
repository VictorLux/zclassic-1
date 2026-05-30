/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * conservation_diff_job — implementation. See jobs/conservation_diff_job.h
 * for the contract and the honesty guardrail.
 *
 * Single-process singleton. One step = one shadow-log read + one
 * zclassicd RPC getblock + one byte-for-byte compare + (on match) one
 * cursor bump. The F-2 stage primitive owns the cursor + crash-replay;
 * this module is the step body and the glue.
 *
 * HONESTY: the process-global `diffed` counter is bumped by the public
 * step wrapper ONLY when stage_run_once durably committed the cursor
 * advance (JOB_ADVANCED). The step body sets a per-step "matched" flag
 * but does NOT touch the counter — so a commit failure after a match
 * (which the stage primitive surfaces as JOB_FATAL with the cursor
 * unchanged) leaves diffed unchanged too. diffed advances iff the cursor
 * durably advanced iff a fed block reconciled byte-for-byte with legacy.
 *
 * RPC-PORT LIFECYCLE (owner-delegated default)
 * --------------------------------------------
 * CHOSEN DEFAULT: open the legacy RPC read port ONCE per boot (the
 * module-static `g_legacy_h` / `g_legacy_port` opened in init, closed in
 * shutdown). Lower overhead for a long-lived Job than open/close per
 * step. The block_log_legacy_rpc adapter does not connect at open — each
 * read does its own short-lived getblock — so a transiently-down daemon
 * is surfaced per-read as a BLOCKED reason, not at open. The ALTERNATIVE
 * (open/close per step) exists and is trivial to switch to (move the
 * open/close into step_diff), but is not the default. */

#include "platform/time_compat.h"
#include "jobs/conservation_diff_job.h"

#include "adapters/inbound/shadow_conservation.h"
#include "adapters/outbound/persistence/block_log_file.h"
#include "adapters/outbound/persistence/block_log_legacy_rpc.h"
#include "json/json.h"
#include "ports/block_log_port.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STAGE_NAME "conservation_diff"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static stage_t        *g_stage = NULL;

/* Shadow ("fed") side: the on-disk shadow log opened at init. */
static struct block_log_file *g_shadow_h = NULL;
static struct block_log_port  g_shadow_port = {0};

/* Legacy ("canonical") side: RPC read port opened once per boot. */
static struct block_log_legacy_rpc *g_legacy_h = NULL;
static struct block_log_port        g_legacy_port = {0};

/* Test seams: when set, these override the on-disk shadow log / real RPC
 * port. NULL means "use the default opened by init". */
static const struct block_log_port *g_shadow_override = NULL;
static const struct block_log_port *g_legacy_override = NULL;

/* Observability (per-init). */
static _Atomic uint64_t g_diffed_total = 0;
static _Atomic int64_t  g_last_advance_height = -1;
static _Atomic int64_t  g_last_blocked_height = -1;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;

/* Per-step out-of-band signal from the step body to the public wrapper:
 * set to 1 iff this step produced a GENUINE byte-for-byte match (and thus
 * advanced the cursor). The wrapper bumps the process-global diffed
 * counter iff this is 1 AND stage_run_once returned JOB_ADVANCED. */
static int g_step_matched = 0;

static const struct block_log_port *shadow_port(void)
{
    return g_shadow_override ? g_shadow_override : &g_shadow_port;
}

static const struct block_log_port *legacy_port(void)
{
    return g_legacy_override ? g_legacy_override : &g_legacy_port;
}

/* ── Step body ─────────────────────────────────────────────────────── */

static job_result_t step_conservation_diff(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());
    g_step_matched = 0;

    if (c->cursor_in > (uint64_t)UINT32_MAX) return JOB_FATAL;
    uint32_t h = (uint32_t)c->cursor_in;

    const struct block_log_port *sp = shadow_port();
    const struct block_log_port *lp = legacy_port();
    if (!sp || !sp->read_at_height || !lp || !lp->read_at_height)
        return JOB_FATAL;

    /* 1. The FED side: the next shadow-log block to diff. NOT_FOUND means
     *    nothing has been fed at this height yet — IDLE (the feeder will
     *    catch up); cursor stays put. */
    const uint8_t *sbytes = NULL;
    size_t slen = 0;
    struct zcl_result rs = sp->read_at_height((void *)sp->self, h,
                                              &sbytes, &slen);
    if (!rs.ok) {
        if (rs.code == BLOCK_LOG_ERR_NOT_FOUND) {
            return JOB_IDLE;  /* no fed block here yet */
        }
        LOG_WARN("conservation_diff", "[conservation_diff] shadow read h=%u code=%d %s", h, rs.code, rs.message);
        return JOB_FATAL;
    }

    /* 2. The CANONICAL side: the legacy block at the same height, read
     *    from zclassicd over RPC. Two distinct failure classes:
     *      - NOT_FOUND  -> legacy hasn't reached this height yet: IDLE.
     *      - any other  -> zclassicd unreachable / oversize / parse:
     *                      BLOCKED with a clear reason. NEVER fake
     *                      progress; a watchdog/condition can alert. */
    const uint8_t *lbytes = NULL;
    size_t llen = 0;
    struct zcl_result rl = lp->read_at_height((void *)lp->self, h,
                                              &lbytes, &llen);
    if (!rl.ok) {
        if (rl.code == BLOCK_LOG_ERR_NOT_FOUND) {
            return JOB_IDLE;  /* legacy not yet at this height */
        }
        atomic_store(&g_last_blocked_height, (int64_t)h);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        char reason[BLOCKER_REASON_MAX];
        snprintf(reason, sizeof reason,
                 "legacy RPC read failed at h=%u code=%d %s",
                 h, rl.code, rl.message);
        /* TRANSIENT: zclassicd down/oversize is recoverable — the
         * supervisor keeps ticking and the step retries the same cursor
         * once the daemon is back. No fake progress in the meantime. */
        blocker_init(&c->blocker, "conservation_diff.legacy_unreachable",
                     STAGE_NAME, BLOCKER_TRANSIENT, reason);
        LOG_WARN("conservation_diff", "[conservation_diff] legacy RPC read h=%u code=%d %s — BLOCKED (no fake progress)", h, rl.code, rl.message);
        return JOB_BLOCKED;
    }

    /* 3. THE GENUINE-MATCH TEST. Equal serialized bytes ⟹ equal block
     *    hash ⟹ identical consensus content. This is the ONLY thing that
     *    licenses a diffed increment. A length or byte mismatch is a
     *    divergence: do NOT increment, do NOT advance — return BLOCKED so
     *    a reorg/reconsider can re-drive this same cursor once both sides
     *    settle (transient mid-reorg disagreement is expected, NOT a
     *    permanent wedge, so we never JOB_FATAL on a content mismatch). */
    if (slen != llen || memcmp(sbytes, lbytes, slen) != 0) {
        atomic_store(&g_last_blocked_height, (int64_t)h);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        char reason[BLOCKER_REASON_MAX];
        snprintf(reason, sizeof reason,
                 "fed/legacy block mismatch at h=%u (fed_len=%zu legacy_len=%zu)",
                 h, slen, llen);
        /* TRANSIENT, not PERMANENT: a mid-reorg disagreement (one side
         * settled to a new block before the other) is expected and
         * self-heals once both sides converge; on the next step the
         * shadow log returns the re-observed block and the diff retries
         * this same cursor. We must NOT permanently wedge the gate. */
        blocker_init(&c->blocker, "conservation_diff.divergent",
                     STAGE_NAME, BLOCKER_TRANSIENT, reason);
        LOG_WARN("conservation_diff", "[conservation_diff] DIVERGENCE at h=%u (fed_len=%zu legacy_len=%zu) — BLOCKED, diffed NOT incremented (reorg can re-drive)", h, slen, llen);
        return JOB_BLOCKED;
    }

    /* Genuine match. Signal the wrapper to bump diffed ON durable commit,
     * advance the cursor. The diffed counter is NOT touched here — see
     * the honesty note at the top of the file. */
    g_step_matched = 1;
    atomic_store(&g_last_advance_height, (int64_t)h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool conservation_diff_job_init(const char *datadir)
{
    if (!datadir || !datadir[0])
        LOG_FAIL("conservation_diff", "init: NULL/empty datadir");

    sqlite3 *db = progress_store_db();
    if (!db)
        LOG_FAIL("conservation_diff", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);

    if (g_stage != NULL) {
        /* Idempotent — already initialised. */
        pthread_mutex_unlock(&g_lock);
        return true;
    }

    /* Open the on-disk shadow log (the FED side). Same path the shadow
     * feeder writes to: <datadir>/blocks.shadow. */
    char shadow_dir[1100];
    snprintf(shadow_dir, sizeof shadow_dir, "%s/blocks.shadow", datadir);
    struct zcl_result rs = block_log_file_open(shadow_dir, &g_shadow_h,
                                               &g_shadow_port);
    if (!rs.ok) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN("conservation_diff", "[conservation_diff] init: shadow log open(%s) failed code=%d %s — Job not running this boot", shadow_dir, rs.code, rs.message);
        return false;
    }

    /* Open the legacy RPC read port ONCE per boot (chosen lifecycle —
     * see the file header). This does not connect; reads do their own
     * short getblock round trips. */
    struct zcl_result rl = block_log_legacy_rpc_open(&g_legacy_h,
                                                     &g_legacy_port);
    if (!rl.ok) {
        block_log_file_close(g_shadow_h);
        g_shadow_h = NULL;
        memset(&g_shadow_port, 0, sizeof g_shadow_port);
        pthread_mutex_unlock(&g_lock);
        LOG_WARN("conservation_diff", "[conservation_diff] init: legacy RPC port open failed code=%d %s — Job not running this boot", rl.code, rl.message);
        return false;
    }

    stage_t *s = stage_create(STAGE_NAME, step_conservation_diff, NULL);
    if (!s) {
        block_log_legacy_rpc_close(g_legacy_h);
        g_legacy_h = NULL;
        memset(&g_legacy_port, 0, sizeof g_legacy_port);
        block_log_file_close(g_shadow_h);
        g_shadow_h = NULL;
        memset(&g_shadow_port, 0, sizeof g_shadow_port);
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("conservation_diff", "init: stage_create failed");
    }

    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("conservation_diff", "[conservation_diff] Job initialised (shadow=%s, legacy=rpc)", shadow_dir);
    return true;
}

void conservation_diff_job_set_legacy_port(const struct block_log_port *port)
{
    pthread_mutex_lock(&g_lock);
    g_legacy_override = port;
    pthread_mutex_unlock(&g_lock);
}

void conservation_diff_job_set_shadow_port(const struct block_log_port *port)
{
    pthread_mutex_lock(&g_lock);
    g_shadow_override = port;
    pthread_mutex_unlock(&g_lock);
}

job_result_t conservation_diff_job_step_once(void)
{
    if (!g_stage) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    pthread_mutex_lock(&g_lock);
    g_step_matched = 0;
    job_result_t r = stage_run_once(g_stage, db);
    /* HONESTY GATE: bump the process-global diffed counter iff the step
     * produced a genuine byte-for-byte match AND the cursor advance was
     * durably committed. If the commit failed (JOB_FATAL) the cursor did
     * not move, so diffed must not move either. */
    if (r == JOB_ADVANCED && g_step_matched) {
        shadow_conservation_record_diffed(1);
        atomic_fetch_add(&g_diffed_total, 1);
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

int conservation_diff_job_drain(int max_steps)
{
    if (max_steps <= 0) return 0;
    int advanced = 0;
    for (int i = 0; i < max_steps; i++) {
        job_result_t r = conservation_diff_job_step_once();
        if (r != JOB_ADVANCED) break;
        advanced++;
    }
    return advanced;
}

void conservation_diff_job_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    if (g_legacy_h) {
        block_log_legacy_rpc_close(g_legacy_h);
        g_legacy_h = NULL;
    }
    memset(&g_legacy_port, 0, sizeof g_legacy_port);
    if (g_shadow_h) {
        block_log_file_close(g_shadow_h);
        g_shadow_h = NULL;
    }
    memset(&g_shadow_port, 0, sizeof g_shadow_port);
    g_legacy_override = NULL;
    g_shadow_override = NULL;
    /* Reset per-init observability. Persisted cursor is preserved (saga
     * contract); the process-global diffed counter is owned by
     * shadow_conservation, not reset here. */
    atomic_store(&g_diffed_total, (uint64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    atomic_store(&g_last_blocked_height, (int64_t)-1);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    g_step_matched = 0;
    pthread_mutex_unlock(&g_lock);
}

uint64_t conservation_diff_job_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t conservation_diff_job_diffed_total(void)
{
    return atomic_load(&g_diffed_total);
}

int64_t conservation_diff_job_last_blocked_height(void)
{
    return atomic_load(&g_last_blocked_height);
}

bool conservation_diff_job_dump_state_json(struct json_value *out,
                                           const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "diffed_total",
                      (int64_t)atomic_load(&g_diffed_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_blocked_height",
                      atomic_load(&g_last_blocked_height));
    json_push_kv_int (out, "last_step_unix",
                      atomic_load(&g_last_step_unix));
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
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
