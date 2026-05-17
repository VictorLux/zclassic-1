/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Explorer block page: /explorer/block/{hash|height}.
 * See explorer_controller_internal.h for shared declarations and
 * controllers/explorer_internal.h for the EXPLORER_HEADER / APPEND
 * macros. */

#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "explorer_controller_internal.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "models/database.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/disk_block_io.h"
#include "util/template.h"
#include "validation/main_state.h"
#include "views/format_helpers.h"
#include "views/wallet_templates_gen.h"
#include "zslp/slp.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t serve_block_rpc(const char *param, uint8_t *r, size_t max)
{
    if (!param || !param[0]) return 0;
    size_t off = 0;
    char buf[262144]; /* 256KB for block JSON */

    /* Get block hash */
    char hash[65] = "";
    if (is_all_digits(param)) {
        char params[64];
        snprintf(params, sizeof(params), "[%s]", param);
        rpc_call("getblockhash", params, buf, sizeof(buf));
        json_extract_str(buf, "result", hash, sizeof(hash));
    } else if (zcl_is_hex_string(param, 64)) {
        snprintf(hash, sizeof(hash), "%s", param);
    }

    if (!hash[0]) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Block Not Found</h2>" EXPLORER_FOOTER);
    }

    /* Get full block */
    char params2[128];
    snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
    rpc_call("getblock", params2, buf, sizeof(buf));

    int height = (int)json_extract_int(buf, "height");
    int64_t blk_time = json_extract_int(buf, "time");
    double blk_diff = json_extract_real(buf, "difficulty");
    (void)json_extract_int(buf, "size");

    char merkle[65] = "", prev[65] = "", next_hash[65] = "";
    json_extract_str(buf, "merkleroot", merkle, sizeof(merkle));
    json_extract_str(buf, "previousblockhash", prev, sizeof(prev));
    json_extract_str(buf, "nextblockhash", next_hash, sizeof(next_hash));

    char ts[32];
    format_time(ts, sizeof(ts), (uint32_t)blk_time);

    /* Count txs */
    int tx_count = 0;
    const char *txarr = strstr(buf, "\"tx\":[");
    if (txarr) {
        const char *end = strchr(txarr, ']');
        tx_count = 1;
        if (end) for (const char *p = txarr; p < end; p++)
            if (*p == ',') tx_count++;
    }

    APPEND(off, r, max, EXPLORER_HEADER("Block"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    /* Pager */
    APPEND(off, r, max, "<div class='pager'>");
    if (height > 0)
        APPEND(off, r, max, "<a href='/explorer/block/%d'>&laquo; Block %d</a>", height - 1, height - 1);
    if (next_hash[0])
        APPEND(off, r, max, "<a href='/explorer/block/%d'>Block %d &raquo;</a>", height + 1, height + 1);
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max,
        "<h2>Block %d</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Hash</div><div class='val hash'>%s</div>"
        "<div class='label'>Height</div><div class='val'>%d</div>"
        "<div class='label'>Time</div><div class='val'>%s</div>"
        "<div class='label'>Transactions</div><div class='val'>%d</div>"
        "<div class='label'>Difficulty</div><div class='val'>%.6f</div>"
        "<div class='label'>Merkle Root</div><div class='val mono'>%s</div>",
        height, hash, height, ts, tx_count, blk_diff, merkle);
    if (prev[0])
        APPEND(off, r, max,
            "<div class='label'>Prev Block</div><div class='val hash'>"
            "<a href='/explorer/block/%s'>%s</a></div>", prev, prev);
    APPEND(off, r, max, "</div></div>");

    /* Transaction list */
    if (txarr) {
        APPEND(off, r, max,
            "<h2>Transactions (%d)</h2>"
            "<table><tr><th>#</th><th>TxID</th></tr>", tx_count);

        const char *p = txarr + 6; /* skip "tx":[ */
        int idx = 0;
        while (p && idx < 100 && off + 256 < max) {
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) break;
                char txid[65];
                size_t tlen = (size_t)(end - p);
                if (tlen > 64) tlen = 64;
                memcpy(txid, p, tlen);
                txid[tlen] = '\0';

                char short_txid[18];
                if (tlen >= 64)
                    snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s", txid, txid + 60);
                else
                    snprintf(short_txid, sizeof(short_txid), "%s", txid);

                APPEND(off, r, max,
                    "<tr><td>%d</td><td class='hash'><a href='/explorer/tx/%s'>%s</a></td></tr>",
                    idx, txid, short_txid);
                idx++;
                p = end + 1;
            } else if (*p == ']') {
                break;
            } else {
                p++;
            }
        }
        if (tx_count > 100)
            APPEND(off, r, max,
                "<tr><td colspan='2' style='color:#666;text-align:center'>"
                "...and %d more transactions</td></tr>", tx_count - 100);
        APPEND(off, r, max, "</table>");
    }

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Block Detail (native) ────────────────────────────────── */

size_t serve_block(const char *param, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    if (use_rpc_proxy())
        return serve_block_rpc(param, r, max);
    if (!ctx->main_state || !param || !param[0]) return 0;
    size_t off = 0;

    const struct block_index *bi = NULL;

    if (is_all_digits(param)) {
        int h = atoi(param);
        int tip = active_chain_height(&ctx->main_state->chain_active);
        if (h >= 0 && h <= tip)
            bi = active_chain_at(&ctx->main_state->chain_active, h);
    } else if (zcl_is_hex_string(param, 64)) {
        struct uint256 hash;
        uint256_set_hex(&hash, param);
        bi = (const struct block_index *)block_map_find(
            &ctx->main_state->map_block_index, &hash);
    }

    if (!bi) {
        char safe_param[256];
        html_escape(safe_param, sizeof(safe_param), param ? param : "");
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Block Not Found</h2>"
            "<p>No block found for: <code>%s</code></p>"
            EXPLORER_FOOTER, safe_param);
    }

    int height = bi->nHeight;
    int tip = active_chain_height(&ctx->main_state->chain_active);

    char hash[65] = "";
    if (bi->phashBlock) uint256_get_hex(bi->phashBlock, hash);
    /* Read block from disk early to get header fields (merkle root, etc.)
     * The block_index mmap doesn't store these — only the full block has them. */
    struct block blk;
    block_init(&blk);
    bool loaded = ctx->datadir && read_block_from_disk_index(&blk, bi, ctx->datadir);

    char merkle[65], sapling_root[65], nonce[65];
    if (loaded) {
        uint256_get_hex(&blk.header.hashMerkleRoot, merkle);
        uint256_get_hex(&blk.header.hashFinalSaplingRoot, sapling_root);
        uint256_get_hex(&blk.header.nNonce, nonce);
    } else {
        uint256_get_hex(&bi->hashMerkleRoot, merkle);
        uint256_get_hex(&bi->hashFinalSaplingRoot, sapling_root);
        uint256_get_hex(&bi->nNonce, nonce);
    }

    char ts[32];
    format_time(ts, sizeof(ts), bi->nTime);
    char sap_val[32] = "0";
    format_zcl(sap_val, sizeof(sap_val), bi->nSaplingValue);
    char sprout_val[32] = "0";
    format_zcl(sprout_val, sizeof(sprout_val), bi->nSproutValue);

    APPEND(off, r, max, EXPLORER_HEADER("Block"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    /* Navigation */
    {
        char prev_fmt[32], next_fmt[32], h_fmt[32], conf_fmt[32];
        format_with_commas(prev_fmt, sizeof(prev_fmt), height - 1);
        format_with_commas(next_fmt, sizeof(next_fmt), height + 1);
        format_with_commas(h_fmt, sizeof(h_fmt), height);
        format_with_commas(conf_fmt, sizeof(conf_fmt), tip - height + 1);

        APPEND(off, r, max, "<div class='pager'>");
        if (height > 0)
            APPEND(off, r, max, "<a href='/explorer/block/%d'>&laquo; Block %s</a>", height - 1, prev_fmt);
        if (height < tip)
            APPEND(off, r, max, "<a href='/explorer/block/%d'>Block %s &raquo;</a>", height + 1, next_fmt);
        APPEND(off, r, max, "</div>");

        APPEND(off, r, max,
            "<h2>Block %s</h2>"
            "<div class='card'><div class='grid'>"
            "<div class='label'>Hash</div><div class='val hash'>%s</div>"
            "<div class='label'>Height</div><div class='val'>%s</div>"
            "<div class='label'>Confirmations</div><div class='val'>%s</div>"
            "<div class='label'>Time</div><div class='val'>%s</div>"
            "<div class='label'>Transactions</div><div class='val'>%u</div>"
            "<div class='label'>Difficulty</div><div class='val'>%.6f</div>"
            "<div class='label'>Merkle Root</div><div class='val mono'>%s</div>"
            "<div class='label'>Sapling Root</div><div class='val mono'>%s</div>"
            "<div class='label'>Nonce</div><div class='val mono'>%s</div>"
            "<div class='label'>Bits</div><div class='val'>0x%08x</div>"
            "<div class='label'>Sapling &Delta;</div><div class='val amount'>%s ZCL</div>"
            "<div class='label'>Sprout &Delta;</div><div class='val amount'>%s ZCL</div>"
            "</div></div>",
            h_fmt, hash, h_fmt, conf_fmt, ts, bi->nTx,
            explorer_get_difficulty(bi), merkle, sapling_root, nonce,
            bi->nBits, sap_val, sprout_val);
    }

    /* Block already loaded above for header fields */

    if (loaded && blk.num_vtx > 0) {
        APPEND(off, r, max,
            "<h2>Transactions (%zu)</h2>"
            "<table><tr><th>#</th><th>TxID</th><th>Type</th>"
            "<th>Inputs</th><th>Outputs</th><th>Value Out</th></tr>",
            blk.num_vtx);

        size_t show_max = blk.num_vtx > 100 ? 100 : blk.num_vtx;
        for (size_t i = 0; i < show_max && off + 512 < max; i++) {
            const struct transaction *tx = &blk.vtx[i];
            char txid[65];
            uint256_get_hex(&tx->hash, txid);
            char short_txid[18];
            snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s", txid, txid + 60);

            char val[32];
            format_zcl(val, sizeof(val), transaction_get_value_out(tx));

            bool is_cb = transaction_is_coinbase(tx);
            bool has_shielded = (tx->num_shielded_spend > 0 || tx->num_shielded_output > 0 ||
                                 tx->num_joinsplit > 0);

            /* Check for ZSLP */
            bool is_slp = false;
            struct slp_message slp;
            if (tx->num_vout > 0 && tx->vout[0].script_pub_key.size > 0)
                is_slp = slp_parse(tx->vout[0].script_pub_key.data,
                                   tx->vout[0].script_pub_key.size, &slp);

            const char *type_tags = "";
            char tags_buf[256] = "";
            if (is_cb) snprintf(tags_buf, sizeof(tags_buf), "<span class='tag tag-cb'>Coinbase</span> ");
            if (has_shielded) {
                size_t tl = strlen(tags_buf);
                snprintf(tags_buf + tl, sizeof(tags_buf) - tl,
                    "<span class='tag tag-shielded'>Shielded</span> ");
            }
            if (is_slp) {
                size_t tl = strlen(tags_buf);
                snprintf(tags_buf + tl, sizeof(tags_buf) - tl,
                    "<span class='tag tag-slp'>ZSLP: %s</span> ", slp.ticker);
            }
            type_tags = tags_buf;

            /* Combined transparent + shielded counts */
            size_t total_in = tx->num_vin + tx->num_shielded_spend +
                              tx->num_joinsplit;
            size_t total_out = tx->num_vout + tx->num_shielded_output +
                               tx->num_joinsplit;

            char idx_s[16], in_s[16], out_s[16];
            snprintf(idx_s, sizeof(idx_s), "%zu", i);
            snprintf(in_s, sizeof(in_s), "%zu", total_in);
            snprintf(out_s, sizeof(out_s), "%zu", total_out);

            struct template_var vars[] = {
                { "index",      idx_s },
                { "txid",       txid },
                { "short_txid", short_txid },
                { "type_tags",  type_tags },
                { "inputs",     in_s },
                { "outputs",    out_s },
                { "value",      val },
            };
            off += template_render(TMPL_EXPLORER_TX_ROW,
                                   vars, sizeof(vars)/sizeof(vars[0]),
                                   (char *)r + off, max - off);
        }

        if (blk.num_vtx > 100)
            APPEND(off, r, max,
                "<tr><td colspan='6' style='color:#666;text-align:center'>"
                "...and %zu more transactions</td></tr>",
                blk.num_vtx - 100);

        APPEND(off, r, max, "</table>");
    } else if (!loaded) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#ff4444'>"
            "Block data not available on disk.</div>");
    }

    block_free(&blk);
    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}
