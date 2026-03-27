/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * UTXO repair: scan ahead through blocks on zclassicd, find inputs whose
 * UTXOs are missing locally, fetch and insert them so connect_block succeeds.
 */

#include "controllers/repair_controller.h"
#include "controllers/strong_params.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "json/json.h"
#include "models/utxo.h"
#include "rpc/client.h"
#include "script/script.h"
#include "validation/main_state.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct main_state *g_main_state_r = NULL;
static struct coins_view_cache *g_coins_tip_r = NULL;
static struct node_db *g_node_db_r = NULL;

void rpc_repair_set_state(struct main_state *ms,
                           struct coins_view_cache *coins_tip,
                           struct node_db *ndb)
{
    g_main_state_r = ms;
    g_coins_tip_r = coins_tip;
    g_node_db_r = ndb;
}

/* ── JSON helpers for parsing zclassicd responses ──────────────── */

static int64_t repair_json_int(const char *json, const char *key)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return strtoll(p, NULL, 10);
}

/* ── UTXO fetchers ─────────────────────────────────────────────── */

/* Fetch via gettxout (fast, only works for currently-unspent UTXOs) */
static bool fetch_utxo_gettxout(int port, const char *creds,
                                 const char *txid_hex, uint32_t vout,
                                 int64_t *value, uint8_t *script,
                                 size_t *script_len, int *height,
                                 bool *coinbase)
{
    char params[256];
    snprintf(params, sizeof(params), "[\"%s\", %u, false]", txid_hex, vout);

    char resp[65536];
    int rc = rpc_call_local(port, creds, "gettxout", params,
                             resp, sizeof(resp));
    if (rc <= 0) return false;

    const char *body = rpc_http_body(resp);
    const char *res = strstr(body, "\"result\"");
    if (!res) return false;
    res += 8;
    while (*res == ' ' || *res == ':') res++;
    if (*res == 'n') return false; /* null = already spent */

    /* Extract value (ZCL → zatoshi) */
    const char *vp = strstr(res, "\"value\"");
    if (!vp) return false;
    vp += 7;
    while (*vp == ' ' || *vp == ':') vp++;
    *value = (int64_t)(strtod(vp, NULL) * 100000000.0 + 0.5);

    /* Extract scriptPubKey hex */
    const char *sph = strstr(res, "\"hex\"");
    if (!sph) return false;
    sph += 5;
    while (*sph == ' ' || *sph == ':' || *sph == '"') sph++;
    const char *sp = sph;
    size_t slen = 0;
    while (*sp && *sp != '"') { slen++; sp++; }
    slen /= 2;
    if (slen > 10000) return false;
    for (size_t i = 0; i < slen; i++) {
        char hx[3] = { sph[i*2], sph[i*2+1], '\0' };
        script[i] = (uint8_t)strtoul(hx, NULL, 16);
    }
    *script_len = slen;

    *coinbase = (strstr(res, "\"coinbase\":true") != NULL ||
                 strstr(res, "\"coinbase\": true") != NULL);

    /* Compute UTXO height from confirmations */
    int64_t confs = repair_json_int(res, "confirmations");
    char resp2[4096];
    int rc2 = rpc_call_local(port, creds, "getblockcount", "[]",
                              resp2, sizeof(resp2));
    if (rc2 > 0) {
        const char *b2 = rpc_http_body(resp2);
        const char *r2 = strstr(b2, "\"result\"");
        if (r2) {
            r2 += 8;
            while (*r2 == ' ' || *r2 == ':') r2++;
            *height = (int)(strtoll(r2, NULL, 10) - confs + 1);
        }
    }
    return true;
}

/* Fetch via getrawtransaction (works for spent UTXOs if txindex=1) */
static bool fetch_utxo_rawtx(int port, const char *creds,
                               const char *txid_hex, uint32_t vout,
                               int64_t *value, uint8_t *script,
                               size_t *script_len, int *height,
                               bool *coinbase)
{
    char params[256];
    snprintf(params, sizeof(params), "[\"%s\", 1]", txid_hex);

    char *resp = malloc(1024 * 1024);
    if (!resp) return false;

    int rc = rpc_call_local(port, creds, "getrawtransaction", params,
                             resp, 1024 * 1024);
    if (rc <= 0) { free(resp); return false; }

    const char *body = rpc_http_body(resp);
    const char *res = strstr(body, "\"result\"");
    if (!res) { free(resp); return false; }
    res += 8;
    while (*res == ' ' || *res == ':') res++;
    if (*res == 'n') { free(resp); return false; }

    /* Find matching vout entry */
    const char *vout_arr = strstr(res, "\"vout\"");
    if (!vout_arr) { free(resp); return false; }

    char n_pat[64];
    snprintf(n_pat, sizeof(n_pat), "\"n\": %u", vout);
    const char *ventry = strstr(vout_arr, n_pat);
    if (!ventry) {
        snprintf(n_pat, sizeof(n_pat), "\"n\":%u", vout);
        ventry = strstr(vout_arr, n_pat);
    }
    if (!ventry) { free(resp); return false; }

    /* Find opening { of this vout object */
    const char *obj = ventry;
    int depth = 0;
    while (obj > vout_arr) {
        obj--;
        if (*obj == '}') depth++;
        if (*obj == '{') { if (depth == 0) break; depth--; }
    }

    /* Extract value */
    const char *vp = strstr(obj, "\"value\"");
    if (!vp || vp > ventry + 200) { free(resp); return false; }
    vp += 7;
    while (*vp == ' ' || *vp == ':') vp++;
    *value = (int64_t)(strtod(vp, NULL) * 100000000.0 + 0.5);

    /* Extract scriptPubKey hex */
    const char *sph = strstr(obj, "\"hex\"");
    if (!sph || sph > ventry + 2000) { free(resp); return false; }
    sph += 5;
    while (*sph == ' ' || *sph == ':' || *sph == '"') sph++;
    const char *sp = sph;
    size_t slen = 0;
    while (*sp && *sp != '"') { slen++; sp++; }
    slen /= 2;
    if (slen > 10000) { free(resp); return false; }
    for (size_t i = 0; i < slen; i++) {
        char hx[3] = { sph[i*2], sph[i*2+1], '\0' };
        script[i] = (uint8_t)strtoul(hx, NULL, 16);
    }
    *script_len = slen;

    *height = 0;
    int64_t bh = repair_json_int(res, "height");
    if (bh > 0) *height = (int)bh;

    *coinbase = (strstr(res, "\"coinbase\"") != NULL &&
                 strstr(res, "\"vin\"") != NULL &&
                 strstr(strstr(res, "\"vin\""), "\"coinbase\"") != NULL);

    free(resp);
    return true;
}

/* Insert a repaired UTXO into coins cache + SQLite */
static void insert_repaired_utxo(const uint8_t txid_bytes[32], uint32_t vout,
                                  int64_t value, const uint8_t *script_data,
                                  size_t script_len, int height, bool is_coinbase)
{
    /* Insert into coins cache (for connect_block) */
    if (g_coins_tip_r) {
        struct uint256 ptxid;
        memcpy(ptxid.data, txid_bytes, 32);
        struct coins_cache_entry *entry =
            coins_view_cache_modify_new(g_coins_tip_r, &ptxid);
        if (entry) {
            if (entry->coins.num_vout <= vout) {
                size_t new_size = vout + 1;
                struct tx_out *nv = realloc(entry->coins.vout,
                    new_size * sizeof(struct tx_out));
                if (nv) {
                    for (size_t k = entry->coins.num_vout; k < new_size; k++)
                        tx_out_set_null(&nv[k]);
                    entry->coins.vout = nv;
                    entry->coins.num_vout = new_size;
                }
            }
            if (vout < entry->coins.num_vout) {
                entry->coins.vout[vout].value = value;
                script_init(&entry->coins.vout[vout].script_pub_key);
                script_set(&entry->coins.vout[vout].script_pub_key,
                           script_data, script_len);
            }
            if (entry->coins.height == 0) entry->coins.height = height;
            if (is_coinbase) entry->coins.is_coinbase = true;
            entry->flags |= COINS_CACHE_DIRTY;
        }
    }

    /* Insert into SQLite */
    uint8_t addr_hash[20];
    bool has_addr = false;
    enum script_type stype = utxo_classify_script(
        script_data, script_len, addr_hash, &has_addr);

    struct db_utxo u;
    memset(&u, 0, sizeof(u));
    memcpy(u.txid, txid_bytes, 32);
    u.vout = vout;
    u.value = value;
    u.script = malloc(script_len);
    if (u.script) memcpy(u.script, script_data, script_len);
    u.script_len = script_len;
    u.script_type = stype;
    u.has_address = has_addr;
    if (has_addr) memcpy(u.address_hash, addr_hash, 20);
    u.height = height;
    u.is_coinbase = is_coinbase;
    db_utxo_save(g_node_db_r, &u);
    free(u.script);
}

/* ── RPC handler ───────────────────────────────────────────────── */

static bool rpc_repairutxos(const struct json_value *params, bool help,
                              struct json_value *result)
{
    RPC_HELP(help, result,
        "repairutxos ( zclassicd_port zclassicd_creds num_blocks )\n"
        "\nScans forward through blocks on zclassicd, finds inputs whose\n"
        "UTXOs are missing locally, and fetches them via RPC.\n"
        "\nArguments:\n"
        "1. zclassicd_port   (number, optional, default=8232)\n"
        "2. zclassicd_creds  (string, optional, default=\"zcluser:zclpass\")\n"
        "3. num_blocks       (number, optional, default=10000)\n"
        "\nRequires zclassicd running on localhost with RPC enabled.\n"
        "For spent UTXOs, zclassicd needs txindex=1.\n");

    if (!g_main_state_r) {
        json_set_str(result, "Node not fully initialized");
        return false;
    }
    if (!g_node_db_r || !g_node_db_r->open) {
        json_set_str(result, "Database not available");
        return false;
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 3);

    int port = (int)rpc_permit_int(&p, 0, "port", 8232);
    const char *creds = rpc_permit_str(&p, 1, "creds", "zcluser:zclpass");
    int num_blocks = (int)rpc_permit_int(&p, 2, "num_blocks", 10000);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    int tip_height = active_chain_height(&g_main_state_r->chain_active);
    int scan_end = tip_height + num_blocks;

    /* Get zclassicd's chain height as scan limit */
    {
        char resp[4096];
        int rc = rpc_call_local(port, creds, "getblockcount", "[]",
                                 resp, sizeof(resp));
        if (rc > 0) {
            const char *body = rpc_http_body(resp);
            const char *rp = strstr(body, "\"result\"");
            if (rp) {
                rp += 8;
                while (*rp == ' ' || *rp == ':') rp++;
                int remote_tip = (int)strtol(rp, NULL, 10);
                if (remote_tip > 0 && scan_end > remote_tip)
                    scan_end = remote_tip;
            }
        } else {
            json_set_str(result, "Cannot connect to zclassicd — is it running?");
            return false;
        }
    }

    printf("repairutxos: scanning blocks %d → %d (%d blocks)\n",
           tip_height + 1, scan_end, scan_end - tip_height);
    printf("repairutxos: using zclassicd on port %d\n", port);
    fflush(stdout);

    int64_t t_start = (int64_t)time(NULL);
    int blocks_scanned = 0, inputs_checked = 0;
    int missing_found = 0, repaired_gettxout = 0, repaired_rawtx = 0;
    int repair_failed = 0;

    if (g_coins_tip_r)
        coins_view_cache_flush(g_coins_tip_r);

    sqlite3_exec(g_node_db_r->db, "BEGIN", NULL, NULL, NULL);

    size_t blk_buf_size = 4 * 1024 * 1024;
    char *blk_buf = malloc(blk_buf_size);
    if (!blk_buf) {
        json_set_str(result, "Out of memory");
        return false;
    }

    for (int h = tip_height + 1; h <= scan_end; h++) {
        /* getblockhash */
        char hash_params[64];
        snprintf(hash_params, sizeof(hash_params), "[%d]", h);
        char hash_resp[4096];
        int rc = rpc_call_local(port, creds, "getblockhash", hash_params,
                                 hash_resp, sizeof(hash_resp));
        if (rc <= 0) { printf("repairutxos: getblockhash(%d) failed\n", h); break; }

        const char *hbody = rpc_http_body(hash_resp);
        const char *hr = strstr(hbody, "\"result\"");
        if (!hr) break;
        hr += 8;
        while (*hr == ' ' || *hr == ':') hr++;
        if (*hr != '"') break;
        hr++;
        char block_hash[65];
        size_t bhi = 0;
        while (*hr && *hr != '"' && bhi < 64) block_hash[bhi++] = *hr++;
        block_hash[bhi] = '\0';

        /* getblock hash 2 */
        char blk_params[256];
        snprintf(blk_params, sizeof(blk_params), "[\"%s\", 2]", block_hash);
        rc = rpc_call_local(port, creds, "getblock", blk_params,
                             blk_buf, blk_buf_size);
        if (rc <= 0) { printf("repairutxos: getblock(%d) failed\n", h); break; }

        const char *bbody = rpc_http_body(blk_buf);

        /* Parse vin arrays for prevout references */
        const char *tx_start = bbody;
        while ((tx_start = strstr(tx_start, "\"vin\"")) != NULL) {
            const char *arr = strchr(tx_start, '[');
            if (!arr) break;
            tx_start = arr + 1;

            const char *vp = tx_start;
            while (*vp) {
                const char *tid = strstr(vp, "\"txid\"");
                if (!tid) break;
                const char *arr_end = strchr(vp, ']');
                if (arr_end && tid > arr_end) break;

                /* Extract prevout txid */
                tid += 6;
                while (*tid == ' ' || *tid == ':') tid++;
                if (*tid != '"') { vp = tid; continue; }
                tid++;
                char prev_txid_hex[65];
                size_t ti2 = 0;
                while (*tid && *tid != '"' && ti2 < 64)
                    prev_txid_hex[ti2++] = *tid++;
                prev_txid_hex[ti2] = '\0';

                /* Extract prevout vout */
                const char *vn = strstr(tid, "\"vout\"");
                if (!vn || (arr_end && vn > arr_end)) { vp = tid; continue; }
                vn += 6;
                while (*vn == ' ' || *vn == ':') vn++;
                uint32_t prev_vout = (uint32_t)strtoul(vn, NULL, 10);

                /* Skip coinbase inputs */
                if (ti2 == 0) { vp = tid; continue; }
                const char *cb = strstr(vp, "\"coinbase\"");
                if (cb && (!arr_end || cb < arr_end) && cb < tid) {
                    vp = tid;
                    continue;
                }

                inputs_checked++;

                /* Convert hex txid to internal byte order (reversed) */
                uint8_t prev_txid_bytes[32];
                for (int bi = 0; bi < 32; bi++) {
                    char hx[3] = { prev_txid_hex[bi*2],
                                   prev_txid_hex[bi*2+1], '\0' };
                    prev_txid_bytes[31 - bi] = (uint8_t)strtoul(hx, NULL, 16);
                }

                /* Check coins cache */
                bool exists = false;
                if (g_coins_tip_r) {
                    struct uint256 ptxid;
                    memcpy(ptxid.data, prev_txid_bytes, 32);
                    struct coins c;
                    coins_init(&c);
                    bool have = coins_view_cache_get_coins(
                        g_coins_tip_r, &ptxid, &c);
                    if (have && prev_vout < c.num_vout &&
                        !tx_out_is_null(&c.vout[prev_vout]))
                        exists = true;
                    coins_free(&c);
                }
                /* Check SQLite */
                if (!exists)
                    exists = db_utxo_exists(g_node_db_r,
                        prev_txid_bytes, prev_vout);

                if (exists) { vp = tid; continue; }

                /* Missing! Fetch from zclassicd */
                missing_found++;
                if (missing_found <= 20 || missing_found % 100 == 0)
                    printf("repairutxos: missing %s:%u (block %d)\n",
                           prev_txid_hex, prev_vout, h);

                int64_t value = 0;
                uint8_t script[10001];
                size_t script_len = 0;
                int utxo_height = 0;
                bool is_coinbase = false;

                bool fetched = fetch_utxo_gettxout(
                    port, creds, prev_txid_hex, prev_vout,
                    &value, script, &script_len, &utxo_height, &is_coinbase);

                if (!fetched) {
                    fetched = fetch_utxo_rawtx(
                        port, creds, prev_txid_hex, prev_vout,
                        &value, script, &script_len, &utxo_height,
                        &is_coinbase);
                    if (fetched) repaired_rawtx++;
                } else {
                    repaired_gettxout++;
                }

                if (!fetched) {
                    repair_failed++;
                    printf("repairutxos: FAILED to fetch %s:%u\n",
                           prev_txid_hex, prev_vout);
                    vp = tid;
                    continue;
                }

                insert_repaired_utxo(prev_txid_bytes, prev_vout, value,
                                      script, script_len, utxo_height,
                                      is_coinbase);
                vp = tid;
            }
        }

        blocks_scanned++;
        if (blocks_scanned % 100 == 0) {
            printf("repairutxos: scanned %d/%d blocks, %d missing, %d repaired\n",
                   blocks_scanned, scan_end - tip_height,
                   missing_found, repaired_gettxout + repaired_rawtx);
            fflush(stdout);
        }
    }

    free(blk_buf);
    sqlite3_exec(g_node_db_r->db, "COMMIT", NULL, NULL, NULL);

    if (g_coins_tip_r)
        coins_view_cache_flush(g_coins_tip_r);

    int64_t elapsed = (int64_t)time(NULL) - t_start;

    printf("repairutxos: done in %llds — %d blocks, %d inputs, "
           "%d missing, %d repaired (%d gettxout, %d rawtx), %d failed\n",
           (long long)elapsed, blocks_scanned, inputs_checked,
           missing_found, repaired_gettxout + repaired_rawtx,
           repaired_gettxout, repaired_rawtx, repair_failed);
    fflush(stdout);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", blocks_scanned);
    json_push_kv_int(result, "inputs_checked", inputs_checked);
    json_push_kv_int(result, "missing_found", missing_found);
    json_push_kv_int(result, "repaired_gettxout", repaired_gettxout);
    json_push_kv_int(result, "repaired_rawtx", repaired_rawtx);
    json_push_kv_int(result, "repair_failed", repair_failed);
    json_push_kv_int(result, "scan_start", tip_height + 1);
    json_push_kv_int(result, "scan_end", scan_end);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

/* ── repairheights: fix height=0 UTXOs from transaction index ──── */

static bool rpc_repairheights(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "repairheights\n"
        "\nFixes UTXOs with height=0 by looking up the creating transaction\n"
        "in the transaction index. This repairs HODL wave calculations that\n"
        "show incorrect age distributions.\n"
        "\nThe LevelDB import pipeline sometimes fails to decode the height\n"
        "varint from coins entries, leaving height=0. This command fixes\n"
        "those entries using the transaction → block_height mapping.\n");

    if (!g_node_db_r || !g_node_db_r->open) {
        json_set_str(result, "Database not available");
        return false;
    }

    int64_t t0 = (int64_t)time(NULL);

    /* Count before */
    sqlite3_stmt *s = NULL;
    int64_t before = 0;
    sqlite3_prepare_v2(g_node_db_r->db,
        "SELECT COUNT(*) FROM utxos WHERE height = 0 AND value > 0",
        -1, &s, NULL);
    if (s && sqlite3_step(s) == SQLITE_ROW)
        before = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    if (before == 0) {
        json_set_object(result);
        json_push_kv_int(result, "fixed", 0);
        json_push_kv_str(result, "status", "no height=0 UTXOs to fix");
        return true;
    }

    printf("repairheights: fixing %lld UTXOs with height=0...\n",
           (long long)before);
    fflush(stdout);

    /* Fix heights by joining with transactions table */
    sqlite3_exec(g_node_db_r->db,
        "UPDATE utxos SET height = ("
        "  SELECT t.block_height FROM transactions t"
        "  WHERE t.txid = utxos.txid"
        ") WHERE height = 0 AND EXISTS ("
        "  SELECT 1 FROM transactions t"
        "  WHERE t.txid = utxos.txid AND t.block_height IS NOT NULL"
        ")", NULL, NULL, NULL);

    int changes = sqlite3_changes(g_node_db_r->db);

    /* Count remaining */
    int64_t after = 0;
    s = NULL;
    sqlite3_prepare_v2(g_node_db_r->db,
        "SELECT COUNT(*) FROM utxos WHERE height = 0 AND value > 0",
        -1, &s, NULL);
    if (s && sqlite3_step(s) == SQLITE_ROW)
        after = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    int64_t elapsed = (int64_t)time(NULL) - t0;

    printf("repairheights: fixed %d heights in %llds (%lld remaining)\n",
           changes, (long long)elapsed, (long long)after);
    fflush(stdout);

    json_set_object(result);
    json_push_kv_int(result, "fixed", changes);
    json_push_kv_int(result, "remaining_height_zero", after);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

void register_repair_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "blockchain", "repairutxos", rpc_repairutxos, false },
        { "blockchain", "repairheights", rpc_repairheights, false },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
