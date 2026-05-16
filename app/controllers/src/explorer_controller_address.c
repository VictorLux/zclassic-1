/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Explorer address + search pages. Split from explorer_controller.c
 * per wave 6c. See explorer_controller_internal.h for shared declarations
 * and controllers/explorer_internal.h for the EXPLORER_HEADER / APPEND
 * macros. */

#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "explorer_controller_internal.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "primitives/block.h"
#include "script/standard.h"
#include "util/ar_step_readonly.h"
#include "util/template.h"
#include "validation/main_state.h"
#include "views/format_helpers.h"
#include "views/wallet_templates_gen.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t serve_address(const char *param, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    size_t param_len = param ? strlen(param) : 0;
    if (!ctx->main_state || !param || !param[0] || param_len >= 128 ||
        !explorer_param_is_printable_ascii(param))
        return 0;

    size_t off = 0;
    char safe_addr[128];
    html_escape(safe_addr, sizeof(safe_addr), param);

    struct tx_destination dest;
    memset(&dest, 0, sizeof(dest));
    if (!addr_decode(param, &dest)) {
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Invalid Address</h2>"
            "<p><code>%s</code> is not a valid ZClassic address.</p>"
            EXPLORER_FOOTER, safe_addr);
    }

    /* Get the 20-byte hash */
    const uint8_t *addr_hash = NULL;
    if (dest.type == DEST_KEY_ID)
        addr_hash = dest.id.key.id.data;
    else if (dest.type == DEST_SCRIPT_ID)
        addr_hash = dest.id.script.hash.data;

    APPEND(off, r, max, EXPLORER_HEADER("Address"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);

    APPEND(off, r, max,
        "<h2>Address</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Address</div><div class='val hash'>%s</div>"
        "<div class='label'>Type</div><div class='val'>%s</div>",
        safe_addr,
        dest.type == DEST_KEY_ID ? "P2PKH (Pay-to-PubKey-Hash)" : "P2SH (Pay-to-Script-Hash)");

    if (ctx->node_db && addr_hash) {
        int64_t balance = db_utxo_balance_for_address(ctx->node_db, addr_hash);
        char bal[32];
        format_zcl(bal, sizeof(bal), balance);
        APPEND(off, r, max,
            "<div class='label'>Balance</div><div class='val amount'>%s ZCL</div>",
            bal);
    }
    APPEND(off, r, max, "</div></div>");

    /* UTXO list */
    if (ctx->node_db && addr_hash) {
        struct db_utxo utxos[100];
        int count = db_utxo_list_for_address(ctx->node_db, addr_hash, utxos, 100);

        APPEND(off, r, max,
            "<h2>Unspent Outputs (%d)</h2>"
            "<table><tr><th>TxID</th><th>Vout</th><th>Value</th>"
            "<th>Height</th><th>Type</th></tr>", count);

        for (int i = 0; i < count && off + 512 < max; i++) {
            char txid_hex[65];
            struct uint256 utxo_txid;
            memcpy(utxo_txid.data, utxos[i].txid, 32);
            uint256_get_hex(&utxo_txid, txid_hex);
            char short_txid[18];
            snprintf(short_txid, sizeof(short_txid), "%.8s...%.4s",
                     txid_hex, txid_hex + 60);

            char val[32];
            format_zcl(val, sizeof(val), utxos[i].value);

            APPEND(off, r, max,
                "<tr><td class='hash'><a href='/explorer/tx/%s'>%s</a></td>"
                "<td>%u</td><td class='amount'>%s ZCL</td>"
                "<td>%d</td><td>%s</td></tr>",
                txid_hex, short_txid, utxos[i].vout, val,
                utxos[i].height,
                utxos[i].is_coinbase ? "<span class='tag tag-cb'>CB</span>" : "");

            db_utxo_free(&utxos[i]);
        }

        APPEND(off, r, max, "</table>");
        if (count == 0) {
            APPEND(off, r, max,
                "<p style='color:#666'>No unspent outputs found for this address.</p>");
        }
    } else {
        APPEND(off, r, max,
            "<p style='color:#666'>UTXO index not available.</p>");
    }

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}

/* ── Search ───────────────────────────────────────────────── */

size_t serve_search(const char *query, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    if (!query) return 0;

    /* URL-decode the query ('+' → space, %XX → byte) */
    char decoded[256];
    {
        size_t di = 0;
        for (size_t si = 0; query[si] && di < sizeof(decoded) - 1; si++) {
            if (query[si] == '%' && query[si+1] && query[si+2]) {
                char hex[3] = { query[si+1], query[si+2], '\0' };
                char *endp = NULL;
                long v = strtol(hex, &endp, 16);
                if (!endp || *endp != '\0' || v < 0 || v > 255)
                    return (size_t)snprintf((char *)r, max,
                        "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                        "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
                        EXPLORER_NAV "<h2>Invalid Search Query</h2>"
                        "<p>Malformed percent-encoding in query.</p>" EXPLORER_FOOTER);
                decoded[di++] = (char)v;
                si += 2;
            } else if (query[si] == '+') {
                decoded[di++] = ' ';
            } else {
                decoded[di++] = query[si];
            }
        }
        decoded[di] = '\0';
    }

    /* Strip leading/trailing whitespace */
    const char *dq = decoded;
    while (*dq == ' ') dq++;
    size_t qlen = strlen(dq);
    char q[256];
    if (qlen >= sizeof(q)) qlen = sizeof(q) - 1;
    memcpy(q, dq, qlen);
    q[qlen] = '\0';
    while (qlen > 0 && q[qlen - 1] == ' ') q[--qlen] = '\0';

    if (!qlen) return explorer_serve_dashboard(r, max);
    if (!explorer_param_is_printable_ascii(q))
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Invalid Search Query</h2>"
            "<p>Search input contains unsupported characters.</p>" EXPLORER_FOOTER);

    /* Block height? Always try — serve_block handles RPC fallback */
    if (is_all_digits(q)) {
        int h = atoi(q);
        if (h >= 0 && h < 100000000)
            return serve_block(q, r, max);
    }

    /* 64-hex: try as block hash first, then txid */
    if (qlen == 64 && zcl_is_all_hex(q, 64)) {
        /* Try block hash via native index */
        if (ctx->main_state) {
            struct uint256 hash;
            uint256_set_hex(&hash, q);
            const struct block_index *bi = block_map_find(
                &ctx->main_state->map_block_index, &hash);
            if (bi)
                return serve_block(q, r, max);
        }

        /* Try txid via SQLite index */
        if (ctx->node_db) {
            struct uint256 hash;
            uint256_set_hex(&hash, q);
            struct db_tx_index txi;
            if (db_tx_find(ctx->node_db, hash.data, &txi))
                return serve_tx(q, r, max);
        }

        /* Try mempool */
        if (ctx->mempool) {
            struct uint256 hash;
            uint256_set_hex(&hash, q);
            if (tx_mempool_exists(ctx->mempool, &hash))
                return serve_tx(q, r, max);
        }

        /* Fallback: try as tx via RPC, then block hash via RPC */
        {
            char rpc_buf[1024];
            char rpc_params[128];
            snprintf(rpc_params, sizeof(rpc_params), "[\"%s\", 1]", q);
            int rn = rpc_call("getrawtransaction", rpc_params,
                              rpc_buf, sizeof(rpc_buf));
            if (rn > 0 && strstr(rpc_buf, "\"error\":null"))
                return serve_tx(q, r, max);
        }
        return serve_block(q, r, max);
    }

    /* Address? (starts with t1, t3, etc.) */
    if (qlen > 20 && (q[0] == 't' || q[0] == 'T')) {
        struct tx_destination dest;
        if (addr_decode(q, &dest))
            return serve_address(q, r, max);
    }

    /* Not found */
    char safe[512];
    html_escape(safe, sizeof(safe), q);
    size_t off = 0;
    APPEND(off, r, max, EXPLORER_HEADER("Search"));
    off += explorer_emit_nav((char *)r + off, max - off, NULL);
    APPEND(off, r, max,
        "<h2>Search Results</h2>"
        "<div class='card'>"
        "<p>No results for: <code>%s</code></p>"
        "<p style='color:#666'>Try a block height, block hash, transaction ID, or address.</p>"
        "</div>" EXPLORER_FOOTER, safe);
    return off;
}

/* ── Stats Page with SVG Charts ────────────────────────────── */

/* format_y_label and svg_line_chart moved to explorer_internal.h */

