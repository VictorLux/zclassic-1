/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Legacy body pull — fetch missing block bodies from a sibling
 * zclassicd via JSON-RPC.
 *
 * Plumbed into local_chain_ingest's phase3 prelude AND boot's
 * stand-alone -bodypull-from-legacy path as the durable backstop
 * for "tip behind zclassicd" conditions. Each fetched block is
 * handed to process_new_block(), so accept_block writes it to disk
 * and the activation controller's connect_tip path extends the
 * active chain.
 *
 * Traversal is height-based, not pprev-based:
 *   - pindex_best_header doesn't follow accept_block_header (only
 *     csr_commit_tip moves it), so a pprev walk from "best_header"
 *     misses the just-pulled headers entirely. Live evidence:
 *     header_probe pulled 10704 headers reaching remote_tip but
 *     pindex_best_header was still pointing at a stale fork ~28k
 *     blocks below the active tip.
 *   - The legacy node serves getblockhash(h) for every height we
 *     care about, so we don't need a local block_index walk at
 *     all. accept_block (inside process_new_block) creates the
 *     block_index entry on the fly if it doesn't exist yet.
 *
 * Strictly forward, blocking, single-threaded. Shares the
 * lib/rpc/legacy_rpc_client transport with the header probe.
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

#define LBP_DEFAULT_HOST    "127.0.0.1"
#define LBP_DEFAULT_PORT    8232

/* Parse a JSON-RPC `.result` string from a raw HTTP response.
 * Returns malloc'd NUL-terminated string on success; NULL on
 * failure with err populated. Caller frees. */
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
    char *out = zcl_malloc(slen + 1, "lbp_str");
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

/* RPC: getblockhash(height) -> hash hex (64 chars). */
static char *lbp_rpc_getblockhash(const char *host, int port,
                                  const char *user, const char *pass,
                                  int height,
                                  char *err, size_t err_sz)
{
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-lbp\","
        "\"method\":\"getblockhash\",\"params\":[%d]}",
        height);
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
    if (hex && strlen(hex) != 64) {
        snprintf(err, err_sz, "hash not 64 hex chars (got %zu)",
                 strlen(hex));
        free(hex);
        return NULL;
    }
    return hex;
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
    if (from_height < 1) {
        LOG_FAIL("legacy_body_pull", "bad from_height=%d", from_height);
    }
    if (to_height < from_height) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_body_pull] nothing to do (from=%d to=%d)\n",
                from_height, to_height);
        return true;
    }

    /* Resolve credentials from ~/.zclassic/zclassic.conf. */
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

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_body_pull] starting: window=[%d..%d] "
            "(%d blocks)\n",
            from_height, to_height,
            to_height - from_height + 1);

    int applied = 0;
    int rpc_errors = 0;
    int skipped_have_data = 0;
    int skipped_failed = 0;
    int last_log_h = -1;
    bool ok = true;

    for (int h = from_height; h <= to_height; h++) {
        if (thread_registry_shutdown_requested()) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] shutdown requested at h=%d\n", h);
            ok = false;
            break;
        }

        char err[160] = {0};
        char *hash_hex = lbp_rpc_getblockhash(host, port, user, pass,
                                              h, err, sizeof(err));
        if (!hash_hex) {
            rpc_errors++;
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] getblockhash h=%d failed: %s\n",
                    h, err);
            ok = false;
            break;
        }

        /* Skip if the block is already on disk (from prior P2P sync or
         * earlier body-pull). block_map lookup is cheap. */
        struct uint256 hash;
        memset(&hash, 0, sizeof(hash));
        uint256_set_hex(&hash, hash_hex);

        zcl_mutex_lock(&ms->cs_main);
        struct block_index *bi = block_map_find(&ms->map_block_index,
                                                 &hash);
        bool have_data = bi && (bi->nStatus & BLOCK_HAVE_DATA);
        bool failed = bi && (bi->nStatus & BLOCK_FAILED_MASK);
        zcl_mutex_unlock(&ms->cs_main);

        if (have_data) {
            skipped_have_data++;
            free(hash_hex);
            continue;
        }
        if (failed) {
            /* Stale BLOCK_FAILED_VALID — P0.5's job to clear. */
            skipped_failed++;
            free(hash_hex);
            continue;
        }

        char *block_hex = lbp_rpc_getblock_hex(host, port, user, pass,
                                                hash_hex, err, sizeof(err));
        free(hash_hex);
        if (!block_hex) {
            rpc_errors++;
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] getblock h=%d failed: %s\n",
                    h, err);
            ok = false;
            break;
        }

        size_t hex_len = strlen(block_hex);
        if (hex_len < 280 || (hex_len % 2) != 0) {
            free(block_hex);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] h=%d bad hex length %zu\n",
                    h, hex_len);
            ok = false;
            break;
        }

        unsigned char *bytes = zcl_malloc(hex_len / 2, "lbp_block_bytes");
        if (!bytes) {
            free(block_hex);
            LOG_FAIL("legacy_body_pull", "oom decode h=%d", h);
        }
        size_t nbytes = ParseHex(block_hex, bytes, hex_len / 2);
        free(block_hex);
        if (nbytes < 80) {
            free(bytes);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] h=%d ParseHex short (%zu)\n",
                    h, nbytes);
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
                    h);
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
                    h,
                    vs.reject_reason[0] ? vs.reject_reason : "(unknown)");
            ok = false;
            break;
        }
        applied++;
        /* Heartbeat every 200 blocks. */
        if (last_log_h < 0 || h - last_log_h >= 200) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_body_pull] applied=%d h=%d (target=%d)\n",
                    applied, h, to_height);
            last_log_h = h;
        }
    }

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_body_pull] done: applied=%d skipped_have=%d "
            "skipped_failed=%d rpc_errors=%d window=[%d..%d] ok=%s\n",
            applied, skipped_have_data, skipped_failed, rpc_errors,
            from_height, to_height, ok ? "yes" : "no");

    if (out_applied) *out_applied = applied;
    return ok;
}
