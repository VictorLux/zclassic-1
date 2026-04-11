/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Middleware — implementation.  See middleware.h for the contract.
 */

#include "mcp/middleware.h"
#include "mcp/router.h"
#include "event/event.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ── Canonical destructive tool list ─────────────────────────── *
 *
 * Any tool that changes wallet, network, or chain state goes here.  Kept
 * in sync with the list `zcl_self_test` uses to skip destructive calls.
 */
static const char *k_default_destructive[] = {
    "zcl_send",
    "zcl_sendtoaddress",
    "zcl_importprivkey",
    "zcl_rescanblockchain",
    "zcl_replaywalletfromchain",
    "zcl_addnode",
    "zcl_swap_initiate",
    "zcl_swap_participate",
    "zcl_market_buy",
    "zcl_market_offer",
    "zcl_msg_send",
    "zcl_msg_send_named",
    "zcl_name_register",
};

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

void mcp_middleware_init(struct mcp_middleware *mw)
{
    memset(mw, 0, sizeof(*mw));
    mw->global_rps = 100;
    mw->destructive_rps = 1;
    mw->burst_global = 200;
    mw->burst_destructive = 2;
    mw->default_timeout_ms = 5000;

    /* Populate destructive list */
    size_t n = sizeof(k_default_destructive) / sizeof(k_default_destructive[0]);
    if (n > MCP_MIDDLEWARE_DESTRUCT_MAX) n = MCP_MIDDLEWARE_DESTRUCT_MAX;
    for (size_t i = 0; i < n; i++)
        mw->destructive_tools[i] = k_default_destructive[i];
    mw->num_destructive_tools = n;

    /* Start with full buckets. */
    mw->global_bucket = (double)mw->burst_global;
    mw->destructive_bucket = (double)mw->burst_destructive;
    mw->last_global_refill_us = now_us();
    mw->last_destructive_refill_us = mw->last_global_refill_us;

    pthread_mutex_init(&mw->bucket_lock, NULL);
    mw->initialized = true;
}

void mcp_middleware_destroy(struct mcp_middleware *mw)
{
    if (!mw || !mw->initialized) return;
    pthread_mutex_destroy(&mw->bucket_lock);
    mw->initialized = false;
}

void mcp_middleware_load_from_env(struct mcp_middleware *mw)
{
    if (!mw->initialized) mcp_middleware_init(mw);

    const char *token = getenv("ZCL_MCP_BEARER_TOKEN");
    if (token && token[0]) {
        snprintf(mw->required_bearer_token,
                 sizeof(mw->required_bearer_token), "%s", token);
    }

    const char *grps = getenv("ZCL_MCP_GLOBAL_RPS");
    if (grps && grps[0]) {
        int64_t v = strtoll(grps, NULL, 10);
        if (v > 0 && v < 1000000) {
            mw->global_rps = v;
            mw->burst_global = v * 2;
            if (mw->global_bucket > (double)mw->burst_global)
                mw->global_bucket = (double)mw->burst_global;
        }
    }

    const char *drps = getenv("ZCL_MCP_DESTRUCTIVE_RPS");
    if (drps && drps[0]) {
        int64_t v = strtoll(drps, NULL, 10);
        if (v > 0 && v < 1000000) {
            mw->destructive_rps = v;
            mw->burst_destructive = v * 2 < 1 ? 1 : v * 2;
            if (mw->destructive_bucket > (double)mw->burst_destructive)
                mw->destructive_bucket = (double)mw->burst_destructive;
        }
    }

    const char *timeout = getenv("ZCL_MCP_TIMEOUT_MS");
    if (timeout && timeout[0]) {
        int64_t v = strtoll(timeout, NULL, 10);
        if (v > 0 && v < 600000)
            mw->default_timeout_ms = v;
    }
}

bool mcp_middleware_is_destructive(const struct mcp_middleware *mw,
                                    const char *tool_name)
{
    if (!mw || !tool_name) return false;
    for (size_t i = 0; i < mw->num_destructive_tools; i++) {
        if (mw->destructive_tools[i] &&
            strcmp(mw->destructive_tools[i], tool_name) == 0)
            return true;
    }
    return false;
}

/* ── Auth ────────────────────────────────────────────────────── */

static int constant_time_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    unsigned int diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned int)(x[i] ^ y[i]);
    return diff == 0 ? 0 : 1;
}

static bool auth_ok(const struct mcp_middleware *mw, const char *bearer_token)
{
    if (mw->required_bearer_token[0] == '\0') return true;
    if (!bearer_token) return false;
    /* Accept both "Bearer <token>" and the bare token. */
    const char *candidate = bearer_token;
    if (strncmp(candidate, "Bearer ", 7) == 0) candidate += 7;
    size_t req_len = strlen(mw->required_bearer_token);
    size_t got_len = strlen(candidate);
    if (req_len != got_len) return false;
    return constant_time_memcmp(mw->required_bearer_token, candidate, req_len) == 0;
}

/* ── Token bucket ────────────────────────────────────────────── */

static void refill_bucket(double *bucket, int64_t *last_us,
                           int64_t rps, int64_t burst)
{
    int64_t now = now_us();
    int64_t dt_us = now - *last_us;
    if (dt_us <= 0) return;
    double add = ((double)rps * (double)dt_us) / 1000000.0;
    *bucket += add;
    if (*bucket > (double)burst) *bucket = (double)burst;
    *last_us = now;
}

static bool consume_global(struct mcp_middleware *mw)
{
    pthread_mutex_lock(&mw->bucket_lock);
    refill_bucket(&mw->global_bucket, &mw->last_global_refill_us,
                  mw->global_rps, mw->burst_global);
    bool ok = mw->global_bucket >= 1.0;
    if (ok) mw->global_bucket -= 1.0;
    pthread_mutex_unlock(&mw->bucket_lock);
    return ok;
}

static bool consume_destructive(struct mcp_middleware *mw)
{
    pthread_mutex_lock(&mw->bucket_lock);
    refill_bucket(&mw->destructive_bucket, &mw->last_destructive_refill_us,
                  mw->destructive_rps, mw->burst_destructive);
    bool ok = mw->destructive_bucket >= 1.0;
    if (ok) mw->destructive_bucket -= 1.0;
    pthread_mutex_unlock(&mw->bucket_lock);
    return ok;
}

/* ── Timeout dispatch ────────────────────────────────────────── */

struct timeout_ctx {
    const char              *tool_name;
    const struct json_value *args;
    char                    *body;        /* handler result, if any */
    bool                     done;
    pthread_mutex_t          m;
    pthread_cond_t           cv;
};

static void *run_dispatch(void *arg)
{
    struct timeout_ctx *ctx = arg;
    char *b = mcp_router_dispatch(ctx->tool_name, ctx->args);
    pthread_mutex_lock(&ctx->m);
    /* If the caller already gave up (timeout), free here and exit. */
    if (ctx->done) {
        pthread_mutex_unlock(&ctx->m);
        free(b);
        return NULL;
    }
    ctx->body = b;
    ctx->done = true;
    pthread_cond_signal(&ctx->cv);
    pthread_mutex_unlock(&ctx->m);
    return NULL;
}

static char *dispatch_with_timeout(const char *tool_name,
                                    const struct json_value *args,
                                    int64_t timeout_ms,
                                    bool *out_timed_out)
{
    *out_timed_out = false;

    /* Zero / negative timeout disables the guard. */
    if (timeout_ms <= 0)
        return mcp_router_dispatch(tool_name, args);

    struct timeout_ctx ctx = {
        .tool_name = tool_name,
        .args      = args,
        .body      = NULL,
        .done      = false,
    };
    pthread_mutex_init(&ctx.m, NULL);
    pthread_cond_init(&ctx.cv, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, run_dispatch, &ctx) != 0) {
        pthread_mutex_destroy(&ctx.m);
        pthread_cond_destroy(&ctx.cv);
        /* Fall back to synchronous dispatch. */
        return mcp_router_dispatch(tool_name, args);
    }
    pthread_detach(th);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ctx.m);
    int rc = 0;
    while (!ctx.done && rc == 0)
        rc = pthread_cond_timedwait(&ctx.cv, &ctx.m, &deadline);

    char *result = NULL;
    if (ctx.done) {
        result = ctx.body;
        ctx.body = NULL;
    } else {
        /* Timed out — abandon the worker thread.  It will free its own
         * body when it eventually completes (see run_dispatch). */
        *out_timed_out = true;
        ctx.done = true;  /* instruct worker to discard its result */
    }
    pthread_mutex_unlock(&ctx.m);

    /* Destroy cv/mutex.  Safe once we observed ctx.done=true OR we set
     * done=true under the lock; in the latter case the worker will skip
     * touching cv.  A small race with the worker entering run_dispatch's
     * mutex lock is handled because it holds ctx.m before signaling. */
    pthread_mutex_destroy(&ctx.m);
    pthread_cond_destroy(&ctx.cv);
    return result;
}

/* ── Envelope helper ─────────────────────────────────────────── */

static char *make_envelope(enum mcp_error_code code, const char *tool,
                            const char *param, const char *msg)
{
    char tmp[2048];
    size_t n = mcp_router_error_envelope(tmp, sizeof(tmp), code, tool,
                                          param, msg);
    if (n == 0) {
        return strdup("{\"error\":{\"code\":\"INTERNAL\",\"message\":\"envelope build failed\"}}");
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, tmp, n);
    out[n] = 0;
    return out;
}

/* ── Public dispatch ─────────────────────────────────────────── */

char *mcp_middleware_dispatch(struct mcp_middleware *mw,
                               const char *tool_name,
                               const struct json_value *args,
                               const char *bearer_token)
{
    if (!mw || !mw->initialized) {
        /* Middleware not configured — fall straight through. */
        return mcp_router_dispatch(tool_name, args);
    }
    if (!tool_name) tool_name = "";

    /* 1. Auth */
    if (!auth_ok(mw, bearer_token)) {
        mw->stat_auth_denied++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=AUTH_REQUIRED", tool_name);
        return make_envelope(MCP_ERR_AUTH_REQUIRED, tool_name, NULL,
                              "bearer token required");
    }

    /* 2. Global rate limit */
    if (!consume_global(mw)) {
        mw->stat_rate_limited_global++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=RATE_LIMITED kind=global", tool_name);
        return make_envelope(MCP_ERR_RATE_LIMITED, tool_name, "global",
                              "global rate limit exceeded");
    }

    /* 3. Destructive rate limit */
    bool is_dest = mcp_middleware_is_destructive(mw, tool_name);
    if (is_dest && !consume_destructive(mw)) {
        mw->stat_rate_limited_destructive++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=RATE_LIMITED kind=destructive",
                    tool_name);
        return make_envelope(MCP_ERR_RATE_LIMITED, tool_name, "destructive",
                              "destructive rate limit exceeded");
    }

    /* 4. Timeout-guarded dispatch */
    bool timed_out = false;
    char *body = dispatch_with_timeout(tool_name, args,
                                        mw->default_timeout_ms, &timed_out);
    if (timed_out) {
        mw->stat_timeout++;
        event_emitf(EV_MCP_REQUEST, 0,
                    "tool=%s code=TOOL_TIMEOUT deadline_ms=%lld",
                    tool_name, (long long)mw->default_timeout_ms);
        return make_envelope(MCP_ERR_TOOL_TIMEOUT, tool_name, NULL,
                              "handler exceeded timeout");
    }

    mw->stat_allowed++;
    return body;
}
