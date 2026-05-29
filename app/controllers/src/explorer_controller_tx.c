/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Explorer transaction page: /explorer/tx/{txid}.
 * See explorer_controller_internal.h for shared declarations and
 * controllers/explorer_internal.h for the EXPLORER_HEADER / APPEND
 * macros. */

#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "explorer_controller_internal.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/tx_index.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "util/ar_step_readonly.h"
#include "util/template.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "views/format_helpers.h"
#include "views/wallet_templates_gen.h"
#include "zslp/slp.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t serve_tx_rpc(const char *param, uint8_t *r, size_t max)
{
    if (!zcl_is_hex_string(param, 64))
        return 0;

    size_t off = 0;
    char buf[262144];

    char params[128];
    snprintf(params, sizeof(params), "[\"%s\", 1]", param);
    int n = rpc_call("getrawtransaction", params, buf, sizeof(buf));
    if (n <= 0 || strstr(buf, "\"error\":null") == NULL) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Transaction Not Found</h2>"
            "<p>TxID: <code>%s</code></p>" EXPLORER_FOOTER, param);
    }

    /* Extract the result object — find "result":{ */
    const char *result = strstr(buf, "\"result\":{");
    if (!result) result = buf;

    int64_t confirmations = json_extract_int(result, "confirmations");
    int64_t blk_height = json_extract_int(result, "height");
    int64_t tx_size = json_extract_int(result, "size");
    int64_t version = json_extract_int(result, "version");
    int64_t locktime = json_extract_int(result, "locktime");
    int64_t expiry = json_extract_int(result, "expiryheight");
    double value_balance = json_extract_real(result, "valuebalance");

    char blockhash[65] = "";
    json_extract_str(result, "blockhash", blockhash, sizeof(blockhash));

    APPEND(off, r, max, EXPLORER_HEADER("Transaction"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);

    APPEND(off, r, max,
        "<h2>Transaction</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>TxID</div><div class='val hash'>%s</div>"
        "<div class='label'>Confirmations</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Size</div><div class='val'>%" PRId64 " bytes</div>"
        "<div class='label'>Version</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Lock Time</div><div class='val'>%" PRId64 "</div>",
        param, confirmations, tx_size, version, locktime);

    if (blockhash[0])
        APPEND(off, r, max,
            "<div class='label'>Block</div><div class='val hash'>"
            "<a href='/explorer/block/%s'>%.16s...</a> (height %" PRId64 ")</div>",
            blockhash, blockhash, blk_height);
    if (expiry > 0)
        APPEND(off, r, max,
            "<div class='label'>Expiry Height</div><div class='val'>%" PRId64 "</div>", expiry);
    if (value_balance != 0.0) {
        char vb[32];
        zcl_format_zcl(vb, sizeof(vb), (int64_t)(value_balance * (double)ZATOSHI_PER_ZCL));
        APPEND(off, r, max,
            "<div class='label'>Value Balance</div><div class='val amount'>%s ZCL</div>", vb);
    }

    APPEND(off, r, max, "</div></div>");

    /* Parse vout array for outputs */
    const char *vout = strstr(result, "\"vout\":[");
    if (vout) {
        APPEND(off, r, max, "<h2>Outputs</h2><div class='io-box'>");

        /* Walk through vout entries — look for "n": and "value": and "addresses": */
        const char *p = vout;
        const char *vout_end = NULL;
        int brace_depth = 0;
        for (const char *q = vout + 7; *q; q++) {
            if (*q == '[') brace_depth++;
            if (*q == ']') { brace_depth--; if (brace_depth <= 0) { vout_end = q; break; } }
        }
        if (!vout_end) vout_end = buf + n;

        /* Find each {"value": entry */
        p = vout;
        int out_idx = 0;
        while (p < vout_end && off + 512 < max) {
            const char *val_str = strstr(p, "\"value\":");
            if (!val_str || val_str >= vout_end) break;

            double val = strtod(val_str + 8, NULL);
            char val_fmt[32];
            zcl_format_zcl(val_fmt, sizeof(val_fmt), (int64_t)(val * (double)ZATOSHI_PER_ZCL));

            /* Try to find address */
            char addr[64] = "";
            const char *addr_start = strstr(val_str, "\"addresses\":[\"");
            if (addr_start && addr_start < vout_end && addr_start - val_str < 500) {
                addr_start += 14;
                const char *addr_end = strchr(addr_start, '"');
                if (addr_end && (size_t)(addr_end - addr_start) < sizeof(addr)) {
                    memcpy(addr, addr_start, (size_t)(addr_end - addr_start));
                    addr[(size_t)(addr_end - addr_start)] = '\0';
                }
            }

            /* Check for OP_RETURN */
            bool is_opreturn = (strstr(val_str, "\"type\":\"nulldata\"") != NULL &&
                                strstr(val_str, "\"type\":\"nulldata\"") < vout_end &&
                                strstr(val_str, "\"type\":\"nulldata\"") - val_str < 500);

            if (is_opreturn) {
                APPEND(off, r, max,
                    "<div class='io-row'><div class='io-idx'>%d</div>"
                    "<div class='io-addr' style='color:#888'>OP_RETURN</div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    out_idx, val_fmt);
            } else if (addr[0]) {
                APPEND(off, r, max,
                    "<div class='io-row'><div class='io-idx'>%d</div>"
                    "<div class='io-addr'><a href='/explorer/address/%s'>%s</a></div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    out_idx, addr, addr, val_fmt);
            } else {
                APPEND(off, r, max,
                    "<div class='io-row'><div class='io-idx'>%d</div>"
                    "<div class='io-addr' style='color:#666'>Unknown</div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    out_idx, val_fmt);
            }

            out_idx++;
            p = val_str + 8;
        }
        APPEND(off, r, max, "</div>");
    }

    /* Shielded data */
    int64_t vShieldedSpend = 0, vShieldedOutput = 0, vJoinSplit = 0;
    const char *ss = strstr(result, "\"vShieldedSpend\":[");
    if (ss) { for (const char *q = ss; *q && *q != ']'; q++) if (*q == '{') vShieldedSpend++; }
    const char *so = strstr(result, "\"vShieldedOutput\":[");
    if (so) { for (const char *q = so; *q && *q != ']'; q++) if (*q == '{') vShieldedOutput++; }
    const char *js = strstr(result, "\"vjoinsplit\":[");
    if (js) { for (const char *q = js; *q && *q != ']'; q++) if (*q == '{') vJoinSplit++; }

    if (vShieldedSpend > 0 || vShieldedOutput > 0 || vJoinSplit > 0) {
        APPEND(off, r, max, "<h2>Shielded Data</h2><div class='card'><div class='grid'>");
        if (vShieldedSpend > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Spends</div><div class='val'>%" PRId64 "</div>",
                vShieldedSpend);
        if (vShieldedOutput > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Outputs</div><div class='val'>%" PRId64 "</div>",
                vShieldedOutput);
        if (vJoinSplit > 0)
            APPEND(off, r, max,
                "<div class='label'>JoinSplits</div><div class='val'>%" PRId64 "</div>",
                vJoinSplit);
        APPEND(off, r, max, "</div></div>");
    }

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Transaction Detail (native) ──────────────────────────── */

size_t serve_tx(const char *param, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    if (use_rpc_proxy())
        return serve_tx_rpc(param, r, max);
    if (!ctx->main_state || !zcl_is_hex_string(param, 64) ||
        !explorer_param_is_printable_ascii(param))
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Invalid Transaction ID</h2>"
            "<p>Expected 64 hex characters.</p>" EXPLORER_FOOTER);

    struct uint256 txhash;
    uint256_set_hex(&txhash, param);

    /* Try mempool */
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    bool in_mempool = ctx->mempool &&
                      tx_mempool_lookup(ctx->mempool, &txhash, &tx);

    /* Try tx index */
    int block_height = -1;
    char block_hash_hex[65] = "";
    struct block blk;
    block_init(&blk);
    bool from_block = false;

    if (!in_mempool && ctx->node_db) {
        struct db_tx_index txi;
        if (db_tx_find(ctx->node_db, txhash.data, &txi)) {
            block_height = txi.block_height;

            /* Load block from disk */
            const struct block_index *bi =
                active_chain_at(&ctx->main_state->chain_active, block_height);
            if (bi && ctx->datadir && read_block_from_disk_index(&blk, bi, ctx->datadir)) {
                /* Find the tx in the block */
                for (size_t i = 0; i < blk.num_vtx; i++) {
                    if (uint256_eq(&blk.vtx[i].hash, &txhash)) {
                        transaction_copy(&tx, &blk.vtx[i]);
                        from_block = true;
                        if (bi->phashBlock)
                            uint256_get_hex(bi->phashBlock, block_hash_hex);
                        break;
                    }
                }
            }
        }
    }

    if (!in_mempool && !from_block) {
        block_free(&blk);
        /* SQLite didn't have it — fall back to RPC (covers txindex gaps) */
        size_t rpc_result = serve_tx_rpc(param, r, max);
        if (rpc_result > 0) return rpc_result;
        char safe_param[256];
        html_escape(safe_param, sizeof(safe_param), param ? param : "");
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Transaction Not Found</h2>"
            "<p>TxID: <code>%s</code></p>"
            "<p style='color:#666'>Not in mempool or tx index.</p>" EXPLORER_FOOTER, safe_param);
    }

    size_t off = 0;
    int tip = active_chain_height(&ctx->main_state->chain_active);
    int confirmations = in_mempool ? 0 : (block_height >= 0 ? tip - block_height + 1 : 0);

    APPEND(off, r, max, EXPLORER_HEADER("Transaction"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);

    /* Header info */
    char txid_hex[65];
    uint256_get_hex(&tx.hash, txid_hex);

    /* Compute serialized size */
    struct byte_stream bs;
    stream_init(&bs, 512);
    transaction_serialize(&tx, &bs);
    size_t tx_size = bs.size;
    stream_free(&bs);

    APPEND(off, r, max,
        "<h2>Transaction</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>TxID</div><div class='val hash'>%s</div>"
        "<div class='label'>Status</div><div class='val'>%s</div>"
        "<div class='label'>Confirmations</div><div class='val'>%d</div>",
        txid_hex,
        in_mempool ? "<span class='tag tag-mempool'>Mempool</span>" : "Confirmed",
        confirmations);

    if (block_height >= 0) {
        char bh_fmt[32];
        format_with_commas(bh_fmt, sizeof(bh_fmt), block_height);
        APPEND(off, r, max,
            "<div class='label'>Block</div><div class='val'>"
            "<a href='/explorer/block/%d'>%s</a></div>",
            block_height, bh_fmt);
    }

    APPEND(off, r, max,
        "<div class='label'>Version</div><div class='val'>%d%s</div>"
        "<div class='label'>Size</div><div class='val'>%zu bytes</div>"
        "<div class='label'>Lock Time</div><div class='val'>%u</div>",
        tx.version, tx.overwintered ? " (Overwinter)" : "",
        tx_size, tx.lock_time);

    if (tx.overwintered && tx.expiry_height > 0)
        APPEND(off, r, max,
            "<div class='label'>Expiry Height</div><div class='val'>%u</div>",
            tx.expiry_height);

    /* Value balance for Sapling */
    if (tx.overwintered && tx.version >= 4) {
        char vb[32];
        zcl_format_zcl(vb, sizeof(vb), tx.value_balance);
        APPEND(off, r, max,
            "<div class='label'>Value Balance</div><div class='val amount'>%s ZCL</div>",
            vb);
    }

    APPEND(off, r, max, "</div></div>");

    /* Inputs */
    APPEND(off, r, max, "<h2>Inputs (%zu)</h2><div class='io-box'>", tx.num_vin);
    for (size_t i = 0; i < tx.num_vin && off + 512 < max; i++) {
        if (transaction_is_coinbase(&tx) && i == 0) {
            char subsidy[32];
            int64_t reward = block_height >= 0 ? get_block_subsidy(block_height, &chain_params_get()->consensus) : 0;
            zcl_format_zcl(subsidy, sizeof(subsidy), reward);
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr'><span class='tag tag-cb'>Coinbase</span> "
                "Block reward</div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, subsidy);
        } else {
            char prev_hash[65];
            uint256_get_hex(&tx.vin[i].prevout.hash, prev_hash);
            char prev_short[18];
            snprintf(prev_short, sizeof(prev_short), "%.8s...%.4s",
                     prev_hash, prev_hash + 60);

            /* Look up previous output value from tx_outputs table */
            char in_val[32] = "?";
            if (ctx->node_db && ctx->node_db->db) {
                sqlite3_stmt *vs = NULL;
                if (sqlite3_prepare_v2(ctx->node_db->db,
                        "SELECT value FROM tx_outputs WHERE txid=? AND vout=?",
                        -1, &vs, NULL) == SQLITE_OK && vs) {
                    sqlite3_bind_blob(vs, 1, tx.vin[i].prevout.hash.data, 32, SQLITE_STATIC);
                    sqlite3_bind_int(vs, 2, (int)tx.vin[i].prevout.n);
                    if (AR_STEP_ROW_READONLY(vs) == SQLITE_ROW) {
                        int64_t prev_val = sqlite3_column_int64(vs, 0);
                        zcl_format_zcl(in_val, sizeof(in_val), prev_val);
                    }
                    sqlite3_finalize(vs);
                }
            }

            if (in_val[0] != '?') {
                APPEND(off, r, max,
                    "<div class='io-row'>"
                    "<div class='io-idx'>%zu</div>"
                    "<div class='io-addr'><a href='/explorer/tx/%s'>%s</a>:%u</div>"
                    "<div class='io-val'>%s ZCL</div></div>",
                    i, prev_hash, prev_short, tx.vin[i].prevout.n, in_val);
            } else {
                APPEND(off, r, max,
                    "<div class='io-row'>"
                    "<div class='io-idx'>%zu</div>"
                    "<div class='io-addr'><a href='/explorer/tx/%s'>%s</a>:%u</div>"
                    "<div class='io-val' style='color:#666'>?</div></div>",
                    i, prev_hash, prev_short, tx.vin[i].prevout.n);
            }
        }
    }
    APPEND(off, r, max, "</div>");

    /* Outputs */
    int64_t total_out = 0;
    APPEND(off, r, max, "<h2>Outputs (%zu)</h2><div class='io-box'>", tx.num_vout);
    for (size_t i = 0; i < tx.num_vout && off + 512 < max; i++) {
        char val[32];
        zcl_format_zcl(val, sizeof(val), tx.vout[i].value);
        total_out += tx.vout[i].value;

        /* Try to extract destination address */
        char addr_str[64] = "";
        struct tx_destination dest;
        memset(&dest, 0, sizeof(dest));
        if (script_extract_destination(&tx.vout[i].script_pub_key, &dest)) {
            addr_encode(addr_str, sizeof(addr_str), &dest);
        }

        /* Check for OP_RETURN */
        bool is_op_return = (tx.vout[i].script_pub_key.size > 0 &&
                             tx.vout[i].script_pub_key.data[0] == 0x6a); /* OP_RETURN */

        if (is_op_return) {
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr' style='color:#888'>OP_RETURN (%zu bytes)</div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, tx.vout[i].script_pub_key.size, val);
        } else if (addr_str[0]) {
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr'><a href='/explorer/address/%s'>%s</a></div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, addr_str, addr_str, val);
        } else {
            APPEND(off, r, max,
                "<div class='io-row'>"
                "<div class='io-idx'>%zu</div>"
                "<div class='io-addr' style='color:#666'>Non-standard script (%zu bytes)</div>"
                "<div class='io-val'>%s ZCL</div></div>",
                i, tx.vout[i].script_pub_key.size, val);
        }
    }
    {
        char tot[32];
        zcl_format_zcl(tot, sizeof(tot), total_out);
        APPEND(off, r, max,
            "<div class='io-row' style='font-weight:bold;border-top:1px solid #333'>"
            "<div class='io-idx'></div><div class='io-addr'>Total</div>"
            "<div class='io-val'>%s ZCL</div></div>", tot);
    }
    APPEND(off, r, max, "</div>");

    /* Shielded data */
    if (tx.num_shielded_spend > 0 || tx.num_shielded_output > 0 || tx.num_joinsplit > 0) {
        APPEND(off, r, max, "<h2>Shielded Data</h2><div class='card'><div class='grid'>");
        if (tx.num_shielded_spend > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Spends</div><div class='val'>%zu</div>",
                tx.num_shielded_spend);
        if (tx.num_shielded_output > 0)
            APPEND(off, r, max,
                "<div class='label'>Sapling Outputs</div><div class='val'>%zu</div>",
                tx.num_shielded_output);
        if (tx.num_joinsplit > 0) {
            int64_t js_in = 0, js_out = 0;
            for (size_t j = 0; j < tx.num_joinsplit; j++) {
                js_in += tx.v_joinsplit[j].vpub_old;
                js_out += tx.v_joinsplit[j].vpub_new;
            }
            char jsi[32], jso[32];
            zcl_format_zcl(jsi, sizeof(jsi), js_in);
            zcl_format_zcl(jso, sizeof(jso), js_out);
            APPEND(off, r, max,
                "<div class='label'>JoinSplits</div><div class='val'>%zu</div>"
                "<div class='label'>vpub_old (t&rarr;z)</div><div class='val amount'>%s ZCL</div>"
                "<div class='label'>vpub_new (z&rarr;t)</div><div class='val amount'>%s ZCL</div>",
                tx.num_joinsplit, jsi, jso);
        }
        APPEND(off, r, max, "</div></div>");
    }

    /* ZSLP token data */
    if (tx.num_vout > 0) {
        struct slp_message slp;
        if (slp_parse(tx.vout[0].script_pub_key.data,
                      tx.vout[0].script_pub_key.size, &slp)) {
            APPEND(off, r, max,
                "<h2><span class='tag tag-slp'>ZSLP Token</span></h2>"
                "<div class='card'><div class='grid'>");

            if (slp.type == SLP_TX_GENESIS) {
                char qty[32];
                snprintf(qty, sizeof(qty), "%" PRIu64, slp.initial_quantity);
                char safe_ticker[128], safe_name[256];
                html_escape(safe_ticker, sizeof(safe_ticker), slp.ticker);
                html_escape(safe_name, sizeof(safe_name), slp.name);
                APPEND(off, r, max,
                    "<div class='label'>Type</div><div class='val'>GENESIS</div>"
                    "<div class='label'>Ticker</div><div class='val' style='color:#ff88ff'>%s</div>"
                    "<div class='label'>Name</div><div class='val'>%s</div>"
                    "<div class='label'>Decimals</div><div class='val'>%u</div>"
                    "<div class='label'>Initial Supply</div><div class='val'>%s</div>",
                    safe_ticker, safe_name, slp.decimals, qty);
                if (slp.document_url[0]) {
                    char safe_url[512];
                    html_escape(safe_url, sizeof(safe_url), slp.document_url);
                    APPEND(off, r, max,
                        "<div class='label'>Document URL</div><div class='val'>%s</div>",
                        safe_url);
                }
            } else if (slp.type == SLP_TX_SEND) {
                char token_id_hex[65];
                uint256_get_hex(&slp.token_id, token_id_hex);
                APPEND(off, r, max,
                    "<div class='label'>Type</div><div class='val'>SEND</div>"
                    "<div class='label'>Token ID</div><div class='val hash'>"
                    "<a href='/explorer/tx/%s'>%s</a></div>",
                    token_id_hex, token_id_hex);
                for (int q = 0; q < slp.num_outputs; q++) {
                    char qlbl[32];
                    snprintf(qlbl, sizeof(qlbl), "Output %d", q + 1);
                    APPEND(off, r, max,
                        "<div class='label'>%s</div><div class='val'>%" PRIu64 "</div>",
                        qlbl, slp.output_quantities[q]);
                }
            } else if (slp.type == SLP_TX_MINT) {
                char token_id_hex[65];
                uint256_get_hex(&slp.token_id, token_id_hex);
                char qty[32];
                snprintf(qty, sizeof(qty), "%" PRIu64, slp.additional_quantity);
                APPEND(off, r, max,
                    "<div class='label'>Type</div><div class='val'>MINT</div>"
                    "<div class='label'>Token ID</div><div class='val hash'>"
                    "<a href='/explorer/tx/%s'>%s</a></div>"
                    "<div class='label'>Quantity</div><div class='val'>%s</div>",
                    token_id_hex, token_id_hex, qty);
            }

            APPEND(off, r, max, "</div></div>");
        }
    }

    transaction_free(&tx);
    block_free(&blk);
    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}
