/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Legacy body pull — fetch missing block bodies from a sibling
 * zclassicd via JSON-RPC.
 *
 * Plumbed into phase3_block_ingest as the durable fallback when the
 * local block_index has headers (via header_probe) but not bodies in
 * a height range past our active tip. Without this, phase3 used to
 * fall back to P2P sync — which works when peers are healthy but
 * stalls when (a) peers don't serve far-tip bodies, (b) some blocks
 * in the window carry a stale BLOCK_FAILED_VALID flag and the chain
 * selector skips paths through them.
 *
 * The pull is strictly forward, blocking, single-threaded. It uses
 * the shared lib/rpc/legacy_rpc_client transport (also used by the
 * header probe).
 */

#include "services/legacy_body_pull.h"

#include "rpc/legacy_rpc_client.h"

#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "consensus/validation.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "primitives/block.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LBP_DEFAULT_HOST       "127.0.0.1"
#define LBP_DEFAULT_PORT       8232
#define LBP_MAX_WINDOW         50000   /* hard cap on per-call collection */

/* Parse a getblock(..., 0) response → result string (the raw hex
 * block). Returns malloc'd NUL-terminated string on success; NULL on
 * failure. Caller frees. */
static char *lbp_parse_result_str(const char *raw, char *err, size_t err_sz)
{
    const char *body = legacy_rpc_http_body(raw);
    if (!body) {
        snprintf(err, err_sz, "no http body separator");
        return NULL;
    }
    struct json_value v = {0};
    if (!json_read(&v, body, strlen(body))) {
        snprintf(err, err_sz, "json parse failed");
        json_free(&v);
        return NULL;
    }
    const struct json_value *r = json_get(&v, "result");
    if (!r || r->type != JSON_STR) {
        const struct json_value *jerr = json_get(&v, "error");
        const char *msg = "no string result";
        if (jerr && jerr->type == JSON_OBJ) {
            const struct json_value *m = json_get(jerr, "message");
            if (m && m->type == JSON_STR) msg = json_get_str(m);
        }
        snprintf(err, err_sz, "rpc error: %s", msg);
        json_free(&v);
        return NULL;
    }
    const char *s = json_get_str(r);
    size_t slen = s ? strlen(s) : 0;
    char *out = zcl_malloc(slen + 1, "lbp_hex");
    if (!out) {
        snprintf(err, err_sz, "oom result");
        json_free(&v);
        return NULL;
    }
    memcpy(out, s ? s : "", slen);
    out[slen] = '\0';
    json_free(&v);
    return out;
}

/* RPC: getblock(hash_hex, 0) -> raw block hex. */
static char *lbp_rpc_getblock_hex(const char *host, int port,
                                  const char *user, const char *pass,
                                  const char *hash_hex,
                                  char *err, size_t err_sz)
{
    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-lbp\","
        "\"method\":\"getblock\",\"params\":[\"%s\",0]}",
        hash_hex);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        snprintf(err, err_sz, "rpc body overflow");
        return NULL;
    }
    char *resp = NULL;
    if (!legacy_rpc_call(host, port, user, pass, body, &resp,
                         err, err_sz)) {
        return NULL;
    }
    char *hex = lbp_parse_result_str(resp, err, err_sz);
    free(resp);
    return hex;
}

/* Walk pprev from `start` collecting block_index pointers with
 * nHeight in [from_h .. to_h] into `out` (ascending order). Returns
 * collected count, or -1 on corrupt walk / out_cap overflow.
 *
 * Caller must hold ms->cs_main. */
static int lbp_collect_window(struct block_index *start,
                              int from_h, int to_h,
                              struct block_index **out, int out_cap)
{
    if (!start || out_cap <= 0 || from_h > to_h) return 0;

    /* Collect descending first, then reverse. */
    int count = 0;
    struct block_index *cur = start;
    int last_h = cur->nHeight + 1;  /* sentinel */
    int steps = 0;
    while (cur && cur->nHeight >= from_h) {
        if (steps++ > LBP_MAX_WINDOW * 2) return -1; // raw-return-ok: corrupt-walk-caller-logs
        if (cur->nHeight >= last_h) return -1; // raw-return-ok: corrupt-walk-non-monotonic
        last_h = cur->nHeight;
        if (cur->nHeight <= to_h) {
            if (count >= out_cap) return -1; // raw-return-ok: walk-window-overflow-caller-logs
            out[count++] = cur;
        }
        cur = cur->pprev;
    }
    /* Reverse to ascending. */
    for (int i = 0, j = count - 1; i < j; i++, j--) {
        struct block_index *t = out[i];
        out[i] = out[j];
        out[j] = t;
    }
    return count;
}

bool legacy_body_pull_range_blocking(struct main_state *ms,
                                     struct coins_view_cache *coins_tip,
                                     const struct chain_params *params,
                                     const char *our_datadir,
                                     int from_height,
                                     int to_height,
                                     int *out_applied)
{
    if (out_applied) *out_applied = 0;
    if (!ms || !params || !our_datadir) {
        LOG_FAIL("legacy_body_pull",
                 "bad args ms=%p params=%p datadir=%p",
                 (void *)ms, (const void *)params,
                 (const void *)our_datadir);
    }
    if (from_height < 0) {
        LOG_FAIL("legacy_body_pull", "bad from_height=%d", from_height);
    }

    /* Resolve credentials. */
    char host[64];
    snprintf(host, sizeof(host), "%s", LBP_DEFAULT_HOST);
    int port = LBP_DEFAULT_PORT;
    char user[64] = {0}, pass[128] = {0};
    if (!legacy_rpc_parse_conf(user, sizeof(user),
                               pass, sizeof(pass), &port)) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_body_pull] no zclassic.conf credentials; "
                "cannot reach legacy node\n");
        return false;
    }

    /* Snapshot best_header under cs_main; resolve to_height. */
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *best = ms->pindex_best_header;
    int best_h = best ? best->nHeight : -1;
    zcl_mutex_unlock(&ms->cs_main);

    if (!best) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_body_pull] pindex_best_header NULL; "
                "header_probe must run first\n");
        return false;
    }
    if (to_height < 0 || to_height > best_h) to_height = best_h;
    if (from_height > to_height) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_body_pull] nothing to do (from=%d to=%d)\n",
                from_height, to_height);
        return true;
    }
    if (to_height - from_height + 1 > LBP_MAX_WINDOW) {
        /* Bound per-call window so we can resume across calls without
         * pinning RAM for the full pointer array. */
        to_height = from_height + LBP_MAX_WINDOW - 1;
    }

    int window = to_height - from_height + 1;
    struct block_index **bis =
        zcl_malloc((size_t)window * sizeof(*bis), "lbp_window");
    if (!bis) {
        LOG_FAIL("legacy_body_pull", "oom window=%d", window);
    }

    zcl_mutex_lock(&ms->cs_main);
    int collected = lbp_collect_window(best, from_height, to_height,
                                       bis, window);
    zcl_mutex_unlock(&ms->cs_main);
    if (collected < 0) {
        free(bis);
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_body_pull] corrupt pprev walk in [%d..%d]\n",
                from_height, to_height);
        return false;
    }
    if (collected == 0) {
        free(bis);
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_body_pull] no block_index entries in "
                "[%d..%d] from best_header h=%d\n",
                from_height, to_height, best_h);
        return false;
    }

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_body_pull] starting: window=[%d..%d] entries=%d "
            "best_header=%d\n",
            from_height, to_height, collected, best_h);

    int applied = 0;
    int rpc_errors = 0;
    int skipped_have_data = 0;
    int skipped_failed = 0;
    int last_log_h = -1;
    bool ok = true;

    for (int i = 0; i < collected; i++) {
        if (thread_registry_shutdown_requested()) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] shutdown requested at h=%d\n",
                    bis[i] ? bis[i]->nHeight : -1);
            ok = false;
            break;
        }

        struct block_index *bi = bis[i];
        if (!bi || !bi->phashBlock) continue;
        if (bi->nStatus & BLOCK_HAVE_DATA) {
            skipped_have_data++;
            continue;
        }
        if (bi->nStatus & BLOCK_FAILED_MASK) {
            /* Leave for P0.5 (chain_restore_service) to clear when
             * appropriate. */
            skipped_failed++;
            continue;
        }

        char hash_hex[65];
        uint256_get_hex(bi->phashBlock, hash_hex);

        char err[160] = {0};
        char *hex = lbp_rpc_getblock_hex(host, port, user, pass,
                                          hash_hex, err, sizeof(err));
        if (!hex) {
            rpc_errors++;
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] getblock h=%d hash=%.16s... "
                    "rpc failed: %s\n",
                    bi->nHeight, hash_hex, err);
            ok = false;
            break;
        }

        /* Decode hex → bytes. */
        size_t hex_len = strlen(hex);
        if (hex_len < 280 || (hex_len % 2) != 0) {
            free(hex);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] h=%d bad hex length %zu\n",
                    bi->nHeight, hex_len);
            ok = false;
            break;
        }
        unsigned char *bytes = zcl_malloc(hex_len / 2, "lbp_block_bytes");
        if (!bytes) {
            free(hex);
            LOG_FAIL("legacy_body_pull", "oom decode h=%d", bi->nHeight);
        }
        size_t nbytes = ParseHex(hex, bytes, hex_len / 2);
        free(hex);
        if (nbytes < 80) {
            free(bytes);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] h=%d ParseHex short (%zu)\n",
                    bi->nHeight, nbytes);
            ok = false;
            break;
        }

        struct byte_stream s;
        stream_init_from_data(&s, bytes, nbytes);
        struct block block;
        block_init(&block);
        bool deser_ok = block_deserialize(&block, &s);
        stream_free(&s);
        free(bytes);
        if (!deser_ok) {
            block_free(&block);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] h=%d block_deserialize failed\n",
                    bi->nHeight);
            ok = false;
            break;
        }

        struct validation_state vs;
        memset(&vs, 0, sizeof(vs));
        bool pn_ok = process_new_block(&vs, ms, coins_tip, params,
                                        &block, true, our_datadir);
        block_free(&block);
        if (!pn_ok) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] h=%d process_new_block FAILED: "
                    "%s\n",
                    bi->nHeight,
                    vs.reject_reason[0] ? vs.reject_reason : "(unknown)");
            ok = false;
            break;
        }
        applied++;
        /* Periodic heartbeat every 200 blocks. */
        if (last_log_h < 0 || bi->nHeight - last_log_h >= 200) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] applied=%d h=%d "
                    "(target=%d)\n",
                    applied, bi->nHeight, to_height);
            last_log_h = bi->nHeight;
        }
    }

    free(bis);

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_body_pull] done: applied=%d skipped_have=%d "
            "skipped_failed=%d rpc_errors=%d window=[%d..%d] ok=%s\n",
            applied, skipped_have_data, skipped_failed, rpc_errors,
            from_height, to_height, ok ? "yes" : "no");

    if (out_applied) *out_applied = applied;
    return ok;
}
