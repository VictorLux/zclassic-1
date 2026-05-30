/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block explorer controller — comprehensive blockchain explorer served
 * over Tor .onion. Supports blocks, transactions (transparent + shielded),
 * ZSLP tokens, and address lookups. */

#include "platform/time_compat.h"
#include "controllers/explorer_controller.h"
#include "controllers/explorer_stats.h"
#include "controllers/explorer_factoids.h"
#include "controllers/api_controller.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "core/serialize.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/hodl_wave.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "zslp/slp.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "controllers/explorer_internal.h"
#include "util/template.h"
#include "views/wallet_templates_gen.h"
#include "views/format_helpers.h"
#include "explorer_controller_internal.h"

/* ── Dashboard (RPC proxy mode) ───────────────────────────── */

static size_t serve_dashboard_rpc(uint8_t *r, size_t max)
{
    size_t off = 0;
    char buf[65536];

    /* Get blockchain info */
    rpc_call("getblockchaininfo", "[]", buf, sizeof(buf));
    int tip = (int)json_extract_int(buf, "blocks");
    double diff = json_extract_real(buf, "difficulty");

    /* Get mempool info */
    rpc_call("getmempoolinfo", "[]", buf, sizeof(buf));
    int64_t mp_count = json_extract_int(buf, "size");
    int64_t mp_bytes = json_extract_int(buf, "bytes");

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    char ht_fmt[32];
    format_with_commas(ht_fmt, sizeof(ht_fmt), tip);
    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        ht_fmt, diff, mp_count, (double)mp_bytes / 1024.0);

    /* Latest blocks */
    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th></tr>");

    int show = 25;
    for (int h = tip; h > tip - show && h >= 0 && off + 600 < max; h--) {
        char params[64];
        snprintf(params, sizeof(params), "[%d]", h);
        rpc_call("getblockhash", params, buf, sizeof(buf));

        /* Extract hash from {"result":"<hash>",...} */
        char hash[65] = "";
        json_extract_str(buf, "result", hash, sizeof(hash));
        if (!hash[0]) continue;

        /* Get block details */
        char params2[128];
        snprintf(params2, sizeof(params2), "[\"%s\"]", hash);
        rpc_call("getblock", params2, buf, sizeof(buf));

        int64_t blk_time = json_extract_int(buf, "time");
        int64_t ntx = json_extract_int(buf, "tx");  /* this is actually array, use size */
        double blk_diff = json_extract_real(buf, "difficulty");

        /* Count txs by counting "tx":[ array elements */
        const char *txarr = strstr(buf, "\"tx\":[");
        int tx_count = 0;
        if (txarr) {
            const char *p = txarr;
            while ((p = strstr(p + 1, "\"")) != NULL && *p) {
                /* Count quoted strings in the tx array */
                tx_count++;
                p = strchr(p + 1, '"');
                if (!p) break;
                if (*(p + 1) == ']' || *(p + 1) == ',') continue;
                break;
            }
            tx_count /= 1; /* each tx has open+close quote */
        }
        /* Simpler: just count commas + 1 */
        if (txarr) {
            const char *end = strchr(txarr, ']');
            tx_count = 1;
            for (const char *p = txarr; p && p < end; p++)
                if (*p == ',') tx_count++;
        }
        (void)ntx;

        char ts[32];
        format_time(ts, sizeof(ts), (uint32_t)blk_time);

        char short_hash[18];
        snprintf(short_hash, sizeof(short_hash), "%.8s...%.4s", hash, hash + 60);

        char ago[32];
        format_time_ago(ago, sizeof(ago), (uint32_t)blk_time);

        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'><b>%d</b></a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s<br><small style='color:#666'>%s</small></td>"
            "<td>%d</td><td>%.2f</td></tr>",
            h, h, hash, short_hash, ago, ts, tx_count, blk_diff);
    }

    APPEND(off, r, max, "</table>" EXPLORER_FOOTER);
    return off;
}

/* ── Dashboard (native chain mode) ───────────────────────── */

static size_t serve_dashboard_native_page(uint8_t *r, size_t max, int page)
{
    struct explorer_context *ctx = explorer_ctx();
    size_t off = 0;

    int tip = active_chain_height(&ctx->main_state->chain_active);
    const struct block_index *tip_bi = active_chain_tip(&ctx->main_state->chain_active);

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    size_t mp_count = ctx->mempool ? tx_mempool_size(ctx->mempool) : 0;
    uint64_t mp_bytes = ctx->mempool ? tx_mempool_total_size(ctx->mempool) : 0;

    char ht_fmt[32];
    format_with_commas(ht_fmt, sizeof(ht_fmt), tip);
    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%zu</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        ht_fmt, explorer_get_difficulty(tip_bi), mp_count, (double)mp_bytes / 1024.0);

    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th><th>Shielded</th></tr>");

    int per_page = 25;
    if (page < 0) page = 0;
    int start_height = tip - page * per_page;
    int end_height = start_height - per_page + 1;
    if (end_height < 0) end_height = 0;

    for (int h = start_height; h >= end_height && h >= 0; h--) {
        const struct block_index *bi = active_chain_at(&ctx->main_state->chain_active, h);
        if (!bi) continue;

        char hash[65] = "";
        if (bi->phashBlock) uint256_get_hex(bi->phashBlock, hash);
        char ts[32];
        format_time(ts, sizeof(ts), bi->nTime);
        char short_hash[18];
        snprintf(short_hash, sizeof(short_hash), "%.8s...%.4s", hash, hash + 60);
        char sap_val[32] = "";
        if (bi->nSaplingValue != 0)
            zcl_format_zcl(sap_val, sizeof(sap_val), bi->nSaplingValue);

        char h_fmt[32];
        format_with_commas(h_fmt, sizeof(h_fmt), h);
        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'>%s</a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s</td><td>%u</td><td>%.4f</td><td class='amount'>%s</td></tr>",
            h, h_fmt, hash, short_hash, ts, bi->nTx, explorer_get_difficulty(bi), sap_val);

        if (off + 512 >= max) break;
    }

    APPEND(off, r, max, "</table>");

    /* Pagination */
    APPEND(off, r, max, "<div class='pager'>");
    if (page > 0)
        APPEND(off, r, max, "<a href='/explorer?page=%d'>&larr; Newer</a>", page - 1);
    if (end_height > 0)
        APPEND(off, r, max, "<a href='/explorer?page=%d'>Older &rarr;</a>", page + 1);
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Dashboard (SQLite-only, no RPC or main_state needed) ── */


size_t serve_dashboard_with_page(uint8_t *r, size_t max, int page)
{
    struct explorer_context *ctx = explorer_ctx();
    /* Use native if chain is loaded, otherwise fall back to RPC proxy */
    if (ctx->main_state && active_chain_height(&ctx->main_state->chain_active) > 0)
        return serve_dashboard_native_page(r, max, page);
    return serve_dashboard_rpc(r, max);
}

size_t explorer_serve_dashboard(uint8_t *r, size_t max)
{
    return serve_dashboard_with_page(r, max, 0);
}
