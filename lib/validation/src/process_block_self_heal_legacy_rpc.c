/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Legacy zclassicd RPC recovery source for missing-UTXO self-heal.
 *
 * This file owns the compatibility RPC transport/parsing needed to ask a
 * sibling zclassicd for a raw transaction, verify the decoded txid, and inject
 * the recovered outputs into the caller-owned coins cache. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/core_io.h"
#include "event/event.h"
#include "rpc/legacy_rpc_client.h"

#include "process_block_internal.h"

/* ── JSON-lite parse helpers for legacy RPC ──────────────────── */
static bool process_block_json_string(const char *json, const char *key,
                                      char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0)
        return false;
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < out_sz) {
        out[n] = p[n];
        n++;
    }
    if (p[n] != '"') return false;
    out[n] = '\0';
    return true;
}

static bool process_block_json_i64(const char *json, const char *key,
                                   int64_t *out)
{
    if (!json || !key || !out)
        return false;
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p)
        return false;
    *out = (int64_t)v;
    return true;
}

static bool process_block_legacy_rpc_body(const char *method,
                                          const char *params,
                                          char **out_raw,
                                          const char **out_body)
{
    if (out_raw) *out_raw = NULL;
    if (out_body) *out_body = NULL;
    if (!method || !params || !out_raw || !out_body)
        return false;

    char user[128], pass[128];
    int port = 8232;
    if (!legacy_rpc_parse_conf(user, sizeof(user), pass, sizeof(pass),
                               &port))
        return false;

    char req[512];
    int n = snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"1.0\",\"id\":\"selfheal\","
        "\"method\":\"%s\",\"params\":%s}",
        method, params);
    if (n <= 0 || (size_t)n >= sizeof(req))
        return false;

    char err[256];
    char *raw = NULL;
    if (!legacy_rpc_call("127.0.0.1", port, user, pass, req, &raw,
                         err, sizeof(err))) {
        fprintf(stderr, "[self-heal] legacy RPC %s failed: %s\n",
                method, err);
        return false;
    }
    const char *body = legacy_rpc_http_body(raw);
    if (!body) {
        free(raw);
        return false;
    }
    *out_raw = raw;
    *out_body = body;
    return true;
}

bool process_block_recover_missing_utxo_from_legacy_rpc(
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    int retry_no)
{
    if (!coins_tip || !txid)
        return false;

    char txhex[65];
    uint256_get_hex(txid, txhex);

    char *tip_raw = NULL;
    const char *tip_body = NULL;
    if (!process_block_legacy_rpc_body("getblockcount", "[]",
                                       &tip_raw, &tip_body))
        return false;
    int64_t remote_tip = 0;
    bool got_tip = process_block_json_i64(tip_body, "result", &remote_tip);
    free(tip_raw);
    if (!got_tip || remote_tip <= 0)
        return false;

    char params[96];
    snprintf(params, sizeof(params), "[\"%s\",1]", txhex);
    char *raw = NULL;
    const char *body = NULL;
    if (!process_block_legacy_rpc_body("getrawtransaction", params,
                                       &raw, &body)) {
        fprintf(stderr, "[self-heal] legacy RPC getrawtransaction failed "
                "for %s\n", txhex);
        return false;
    }
    if (strstr(body, "\"result\":null")) {
        free(raw);
        return false;
    }

    char rawtx_hex[200000];
    int64_t confirmations = 0;
    if (!process_block_json_string(body, "hex", rawtx_hex,
                                   sizeof(rawtx_hex)) ||
        !process_block_json_i64(body, "confirmations", &confirmations) ||
        confirmations <= 0) {
        fprintf(stderr, "[self-heal] legacy RPC response missing hex/"
                "confirmations for %s\n", txhex);
        free(raw);
        return false;
    }

    struct transaction tx;
    transaction_init(&tx);
    bool decoded = decode_hex_tx(&tx, rawtx_hex);
    if (!decoded) {
        fprintf(stderr, "[self-heal] legacy RPC raw tx decode failed "  // obs-ok:helper-context-logged
                "for %s\n", txhex);
        transaction_free(&tx);
        free(raw);
        return false;
    }
    transaction_compute_hash(&tx);
    if (!uint256_eq(&tx.hash, txid)) {
        transaction_free(&tx);
        free(raw);
        fprintf(stderr, "[self-heal] legacy RPC txid mismatch for %s\n",
                txhex);
        return false;
    }

    int height = (int)(remote_tip - confirmations + 1);
    bool recovered = process_block_inject_missing_utxo(
        coins_tip, txid, missing_vout, &tx, height,
        "verified legacy zclassicd RPC", retry_no);
    if (recovered) {
        event_emitf(EV_SELF_HEAL_SCAN_HIT, 0,
                    "tx=%s h=%d source=legacy_rpc", txhex, height);
    }
    transaction_free(&tx);
    free(raw);
    return recovered;
}
