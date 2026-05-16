/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet view shared helpers — extracted from wallet_view_controller.c.
 * Global state, DB access, RPC, QR codes, HTML chrome, form parsing. */

#include "controllers/wallet_view_internal.h"
/* CSS is now in app/views/css/wallet.ccss, compiled as CSS_WALLET */
#include "models/contact.h"
#include "models/shared_validators.h"
#include "models/wallet_tx.h"
#include "crypto/sha256.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ── Global state (non-static, declared extern in wallet_view_internal.h) ── */

const char *g_wv_datadir = NULL;
int g_balance_dirty = 0;
time_t g_shield_pending_since = 0;
char g_shield_opid[128] = "";
int64_t g_shield_pending_amount = 0;
bool g_sync_enabled = false;

/* ── Init / enable ─────────────────────────────────────────── */

void wallet_view_init(const char *datadir) {
    g_wv_datadir = datadir;
    g_balance_dirty = 0;
    g_shield_pending_since = 0;
    g_shield_opid[0] = '\0';
    g_shield_pending_amount = 0;
    tmpl_init_partials();
}

void wallet_view_enable_sync(void) {
    g_sync_enabled = true;
}

/* ── RPC auth ──────────────────────────────────────────────── */

const char *wv_zclassicd_auth(void) {
    static char auth[256] = "";
    if (auth[0]) return auth;
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    char path[512];
    /* Try zclassic.conf first (stable credentials survive restarts) */
    snprintf(path, sizeof(path), "%s/.zclassic/zclassic.conf", home);
    FILE *f = fopen(path, "r");
    if (f) {
        char user[64] = "", pass[64] = "", line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "rpcuser=", 8) == 0) {
                char *e = strchr(line + 8, '\n'); if (e) *e = '\0';
                char *r = strchr(line + 8, '\r'); if (r) *r = '\0';
                snprintf(user, sizeof(user), "%s", line + 8);
            }
            if (strncmp(line, "rpcpassword=", 12) == 0) {
                char *e = strchr(line + 12, '\n'); if (e) *e = '\0';
                char *r = strchr(line + 12, '\r'); if (r) *r = '\0';
                snprintf(pass, sizeof(pass), "%s", line + 12);
            }
        }
        fclose(f);
        if (user[0] && pass[0]) {
            snprintf(auth, sizeof(auth), "%s:%s", user, pass);
            return auth;
        }
    }
    /* Fall back to cookie file (ephemeral, changes on restart) */
    snprintf(path, sizeof(path), "%s/.zclassic/.cookie", home);
    f = fopen(path, "r");
    if (f) {
        size_t n = fread(auth, 1, sizeof(auth) - 1, f);
        fclose(f);
        auth[n] = '\0';
        char *nl = strchr(auth, '\n'); if (nl) *nl = '\0';
        if (auth[0]) return auth;
    }
    /* Last resort */
    snprintf(auth, sizeof(auth), "zcluser:zclpass");
    return auth;
}

/* ── RPC to running zclassicd node ─────────────────────────── */

int wv_rpc_call(const char *method, const char *params_json,
                char *out, size_t outmax)
{
    const char *auth_cookie = wv_zclassicd_auth();
    char cookie[256] = "";

    if (auth_cookie && auth_cookie[0]) {
        snprintf(cookie, sizeof(cookie), "%s", auth_cookie);
    } else {
        if (!g_wv_datadir)
            LOG_ERR("wallet_view", "rpc_call(%s): no datadir set", method);

        /* Read auth cookie */
        char cookie_path[1024];
        snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", g_wv_datadir);
        FILE *f = fopen(cookie_path, "r");
        if (!f) {
            /* Try config file credentials */
            char conf_path[1024];
            snprintf(conf_path, sizeof(conf_path), "%s/zclassic.conf",
                     g_wv_datadir);
            f = fopen(conf_path, "r");
            if (!f)
                LOG_ERR("wallet_view", "rpc_call(%s): cannot open cookie or conf at %s", method, g_wv_datadir);
            char user[64] = "", pass[64] = "";
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "rpcuser=", 8) == 0) {
                    char *nl = strchr(line + 8, '\n'); if (nl) *nl = '\0';
                    snprintf(user, sizeof(user), "%s", line + 8);
                }
                if (strncmp(line, "rpcpassword=", 12) == 0) {
                    char *nl = strchr(line + 12, '\n'); if (nl) *nl = '\0';
                    snprintf(pass, sizeof(pass), "%s", line + 12);
                }
            }
            fclose(f);
            if (!user[0] || !pass[0])
                LOG_ERR("wallet_view", "rpc_call(%s): missing rpcuser/rpcpassword in conf", method);
            snprintf(cookie, sizeof(cookie), "%s:%s", user, pass);
        } else {
            size_t n = fread(cookie, 1, sizeof(cookie) - 1, f);
            fclose(f);
            cookie[n] = '\0';
            char *nl = strchr(cookie, '\n'); if (nl) *nl = '\0';
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        LOG_ERR("wallet_view", "rpc_call(%s): socket() failed", method);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(ZCLASSICD_PORT);

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        LOG_ERR("wallet_view", "rpc_call(%s): connect to port %d failed", method, ZCLASSICD_PORT);
    }

    char body[1024];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
        method, params_json);
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        close(fd);
        LOG_ERR("wallet_view", "rpc_call(%s): request body too large (%d bytes)", method, blen);
    }

    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char auth_b64[512];
    size_t alen = strlen(cookie), bo = 0;
    for (size_t i = 0; i < alen; i += 3) {
        uint32_t n2 = ((uint32_t)(uint8_t)cookie[i]) << 16;
        if (i + 1 < alen) n2 |= ((uint32_t)(uint8_t)cookie[i+1]) << 8;
        if (i + 2 < alen) n2 |= (uint32_t)(uint8_t)cookie[i+2];
        auth_b64[bo++] = b64[(n2 >> 18) & 63];
        auth_b64[bo++] = b64[(n2 >> 12) & 63];
        auth_b64[bo++] = (i + 1 < alen) ? b64[(n2 >> 6) & 63] : '=';
        auth_b64[bo++] = (i + 2 < alen) ? b64[n2 & 63] : '=';
    }
    auth_b64[bo] = '\0';

    char req[4096];
    int rlen = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        auth_b64, blen, body);

    if (write(fd, req, (size_t)rlen) != rlen) {
        close(fd);
        LOG_ERR("wallet_view", "rpc_call(%s): write failed (expected %d bytes)", method, rlen);
    }

    size_t total = 0;
    while (total < outmax - 1) {
        ssize_t r = read(fd, out + total, outmax - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    out[total] = '\0';
    close(fd);

    char *body_start = strstr(out, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_len = total - (size_t)(body_start - out);
        memmove(out, body_start, body_len);
        out[body_len] = '\0';
        return (int)body_len;
    }
    return (int)total;
}

/* ── Get best-funded transparent address ───────────────────── */

/* Get ALL funded t-addresses with per-address balances.
 * Aggregates UTXOs by address. Returns count of addresses found.
 * Each entry: addrs[i].addr, addrs[i].amount (ZCL as double). */
int wv_get_all_funded_taddrs(struct wv_funded_addr *addrs, int max_addrs) {
    char lu[16384] = "";
    if (wv_rpc_call("listunspent", "[]", lu, sizeof(lu)) <= 0)
        return 0;
    int n = 0;
    /* Parse listunspent JSON: aggregate amounts per address */
    const char *p = lu;
    while ((p = strstr(p, "\"address\"")) != NULL) {
        p += 9;
        while (*p == ' ' || *p == ':' || *p == '"') p++;
        const char *a = p;
        while (*p && *p != '"') p++;
        size_t al = (size_t)(p - a);
        if (al < 20 || al >= 128) continue;
        const char *am = strstr(p, "\"amount\"");
        if (!am) continue;
        am += 8;
        while (*am == ' ' || *am == ':') am++;
        double v = strtod(am, NULL);
        if (v <= 0) continue;
        /* Find existing entry or add new */
        int found = -1;
        for (int i = 0; i < n; i++) {
            if (strlen(addrs[i].addr) == al &&
                memcmp(addrs[i].addr, a, al) == 0) {
                found = i; break;
            }
        }
        if (found >= 0) {
            addrs[found].amount += v;
        } else if (n < max_addrs) {
            memcpy(addrs[n].addr, a, al);
            addrs[n].addr[al] = '\0';
            addrs[n].amount = v;
            n++;
        }
    }
    return n;
}

void wv_get_funded_taddr(char *out, size_t max) {
    out[0] = '\0';
    struct wv_funded_addr addrs[16];
    int n = wv_get_all_funded_taddrs(addrs, 16);
    /* Return address with highest balance */
    double best = 0;
    for (int i = 0; i < n; i++) {
        if (addrs[i].amount > best) {
            best = addrs[i].amount;
            snprintf(out, max, "%s", addrs[i].addr);
        }
    }
}

/* ── DB helpers ────────────────────────────────────────────── */

sqlite3 *wv_open_db(void) {
    if (!g_wv_datadir)
        LOG_NULL("wallet_view", "open_db: no datadir set");
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_wv_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        LOG_NULL("wallet_view", "open_db: cannot open %s", path);
    }
    sqlite3_busy_timeout(db, 3000);
    return db;
}

sqlite3 *wv_open_db_rw(void) {
    if (!g_wv_datadir)
        LOG_NULL("wallet_view", "open_db_rw: no datadir set");
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_wv_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        LOG_NULL("wallet_view", "open_db_rw: cannot open %s", path);
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

/* ── Contacts (address book) ───────────────────────────────── */

void wv_save_contact(const char *address, const char *name) {
    sqlite3 *db = wv_open_db_rw();
    struct node_db ndb;
    struct db_contact contact;
    if (!db) return;
    memset(&ndb, 0, sizeof(ndb));
    ndb.db = db;
    ndb.open = true;
    memset(&contact, 0, sizeof(contact));
    snprintf(contact.address, sizeof(contact.address), "%s", address ? address : "");
    snprintf(contact.name, sizeof(contact.name), "%s", name ? name : "");
    (void)db_contact_save(&ndb, &contact);
    sqlite3_close(db);
}

int wv_recent_contacts(struct db_contact *out, size_t max)
{
    sqlite3 *db = wv_open_db();
    struct node_db ndb;
    int count = 0;

    if (!db || !out || max == 0)
        return 0;
    memset(&ndb, 0, sizeof(ndb));
    ndb.db = db;
    ndb.open = true;
    count = db_contact_recent(&ndb, out, max);
    sqlite3_close(db);
    return count;
}

/* ── Hex conversion (internal) ─────────────────────────────── */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_to_bin(const char *hex, size_t hexlen,
                          uint8_t *out, size_t outmax) {
    if (hexlen % 2 != 0 || hexlen / 2 > outmax) return 0;
    for (size_t i = 0; i < hexlen; i += 2) {
        int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i+1]);
        if (hi < 0 || lo < 0) return 0;
        out[i/2] = (uint8_t)((hi << 4) | lo);
    }
    return hexlen / 2;
}

/* ── JSON mini-parser (internal) ───────────────────────────── */

static bool json_next_str(const char **pos, const char *key,
                           const char **val, size_t *vlen) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(*pos, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    const char *end = p;
    while (*end && !(*end == '"' && (end == p || *(end-1) != '\\'))) end++;
    if (!*end) return false;
    *val = p;
    *vlen = (size_t)(end - p);
    *pos = end + 1;
    return true;
}

static bool json_next_num(const char **pos, const char *key, double *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(*pos, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    char *endptr = NULL;
    *out = strtod(p, &endptr);
    *pos = endptr ? endptr : p + 1;
    return true;
}

static bool json_next_int(const char **pos, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(*pos, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    char *endptr = NULL;
    *out = (int)strtol(p, &endptr, 10);
    *pos = endptr ? endptr : p + 1;
    return true;
}

static void wv_sapling_placeholder_fields(const uint8_t txid_bin[32],
                                          int outindex,
                                          uint8_t rcm[32],
                                          uint8_t ivk[32],
                                          uint8_t div_full[32],
                                          uint8_t pkd[32],
                                          uint8_t cm[32],
                                          uint8_t nf[32])
{
    uint8_t seed[36];

    memcpy(seed, txid_bin, 32);
    seed[32] = (uint8_t)(outindex & 0xFF);
    seed[33] = (uint8_t)((outindex >> 8) & 0xFF);
    seed[34] = 0;
    seed[35] = 0;

    #define HASH_FIELD(tag, taglen, out) do { \
        struct sha256_ctx _hc; \
        sha256_init(&_hc); \
        sha256_write(&_hc, (const unsigned char *)(tag), (taglen)); \
        sha256_write(&_hc, seed, 36); \
        sha256_finalize(&_hc, (out)); \
    } while (0)

    HASH_FIELD("nf", 2, nf);
    HASH_FIELD("cm", 2, cm);
    HASH_FIELD("rcm", 3, rcm);
    HASH_FIELD("ivk", 3, ivk);
    HASH_FIELD("pkd", 3, pkd);
    HASH_FIELD("div", 3, div_full);

    #undef HASH_FIELD
}

/* ── Sync wallet from zclassicd ────────────────────────────── */

void wv_sync_wallet_from_zclassicd(void) {
    if (!g_wv_datadir) return;
    char dbpath[1024];
    struct node_db ndb;
    struct db_wallet_utxo *utxos = NULL;
    struct db_sapling_note *notes = NULL;
    size_t utxo_count = 0;
    size_t utxo_cap = 0;
    size_t note_count = 0;
    size_t note_cap = 0;

    snprintf(dbpath, sizeof(dbpath), "%s/node.db", g_wv_datadir);
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, dbpath))
        return;

    /* Fetch transparent UTXOs from zclassicd */
    char lu[65536] = "";
    int lu_rc = wv_rpc_call("listunspent", "[0]", lu, sizeof(lu));
    if (lu_rc <= 0) { node_db_close(&ndb); return; }

    /* Fetch shielded notes from zclassicd.
     * z_listunspent can be large (~1.3KB per note with memo fields).
     * 256KB handles ~200 notes safely. */
    char zlu[262144] = "";
    int zlu_rc = wv_rpc_call("z_listunspent", "[0]", zlu, sizeof(zlu));
    if (zlu_rc <= 0) { node_db_close(&ndb); return; }

    /* Sanity: response must contain "result" and at least one entry.
     * If response is an error or empty, don't wipe the DB. */
    if (!strstr(lu, "\"result\"") || !strstr(lu, "\"txid\"")) {
        node_db_close(&ndb); return;
    }

    {
        int chain_tip = db_wallet_chain_tip_height(&ndb);
        const char *p = lu;
        const char *txid_s;
        size_t txid_l;

        while (json_next_str(&p, "txid", &txid_s, &txid_l)) {
            struct db_wallet_utxo row;
            int vout = 0;
            int confs = 0;
            int height = 0;
            double amt = 0;
            int64_t val = 0;
            const char *script_s;
            size_t script_l = 0;
            const char *scan = p;
            uint8_t script_bin[64];
            size_t script_bin_len = 0;

            if (txid_l != 64)
                continue;
            memset(&row, 0, sizeof(row));
            if (hex_to_bin(txid_s, 64, row.txid, 32) != 32)
                continue;
            json_next_int(&scan, "vout", &vout);
            scan = p;
            json_next_num(&scan, "amount", &amt);
            scan = p;
            json_next_int(&scan, "confirmations", &confs);
            val = (int64_t)(amt * 1e8 + 0.5);
            height = (chain_tip > 0 && confs > 0) ? (chain_tip - confs + 1) : 0;

            row.vout = (uint32_t)vout;
            row.value = val;
            row.height = height;

            scan = p;
            if (json_next_str(&scan, "scriptPubKey", &script_s, &script_l)) {
                script_bin_len = hex_to_bin(script_s, script_l,
                                            script_bin, sizeof(script_bin));
                if (script_bin_len == 25 && script_bin[0] == 0x76 &&
                    script_bin[1] == 0xa9 && script_bin[2] == 0x14) {
                    memcpy(row.address_hash, script_bin + 3, 20);
                }
            }
            if (script_bin_len > 0) {
                row.script = zcl_malloc(script_bin_len, "utxo script");
                if (!row.script)
                    goto cleanup;
                memcpy(row.script, script_bin, script_bin_len);
                row.script_len = script_bin_len;
            }

            if (utxo_count == utxo_cap) {
                size_t new_cap = utxo_cap == 0 ? 64 : utxo_cap * 2;
                struct db_wallet_utxo *new_rows =
                    zcl_realloc(utxos, new_cap * sizeof(*utxos), "utxo list grow");
                if (!new_rows) {
                    db_wallet_utxo_free(&row);
                    goto cleanup;
                }
                utxos = new_rows;
                utxo_cap = new_cap;
            }
            utxos[utxo_count++] = row;
        }
    }

    {
        int chain_tip = db_wallet_chain_tip_height(&ndb);
        const char *p = zlu;
        const char *txid_s;
        size_t txid_l;

        while (json_next_str(&p, "txid", &txid_s, &txid_l)) {
            struct db_sapling_note row;
            int outindex = 0;
            int confs = 0;
            int note_height = 0;
            double amt = 0;
            int64_t val = 0;
            const char *addr_s;
            size_t addr_l = 0;
            const char *scan = p;
            uint8_t div_full[32];

            if (txid_l != 64)
                continue;
            memset(&row, 0, sizeof(row));
            if (hex_to_bin(txid_s, 64, row.txid, 32) != 32)
                continue;
            json_next_int(&scan, "outindex", &outindex);
            scan = p;
            json_next_num(&scan, "amount", &amt);
            scan = p;
            json_next_int(&scan, "confirmations", &confs);
            val = (int64_t)(amt * 1e8 + 0.5);
            note_height = (chain_tip > 0 && confs > 0)
                ? (chain_tip - confs + 1) : 0;

            row.output_index = (uint32_t)outindex;
            row.value = val;
            row.block_height = note_height;
            scan = p;
            if (json_next_str(&scan, "address", &addr_s, &addr_l) &&
                addr_l < sizeof(row.address)) {
                memcpy(row.address, addr_s, addr_l);
                row.address[addr_l] = '\0';
            }

            wv_sapling_placeholder_fields(row.txid, outindex, row.rcm, row.ivk,
                                          div_full, row.pk_d, row.cm,
                                          row.nullifier);
            memcpy(row.diversifier, div_full, sizeof(row.diversifier));

            if (note_count == note_cap) {
                size_t new_cap = note_cap == 0 ? 64 : note_cap * 2;
                struct db_sapling_note *new_rows =
                    zcl_realloc(notes, new_cap * sizeof(*notes), "sapling notes grow");
                if (!new_rows)
                    goto cleanup;
                notes = new_rows;
                note_cap = new_cap;
            }
            notes[note_count++] = row;
        }
    }

    if (utxo_count > 0 && !db_wallet_utxo_replace_all(&ndb, utxos, utxo_count))
        goto cleanup;
    if (note_count > 0 && !db_sapling_note_replace_all(&ndb, notes, note_count))
        goto cleanup;

    /* Update wallet_transactions: fill in block_height for confirmed txs.
     * Query zclassicd gettransaction for each tx with height=0. */
    {
        struct db_wallet_txid_ref txids[50];
        int tx_count = db_wallet_tx_list_unconfirmed(&ndb, txids,
                                                     sizeof(txids) / sizeof(txids[0]));
        int updated = 0;
        int chain_tip = db_wallet_chain_tip_height(&ndb);
        for (int i = 0; i < tx_count; i++) {
            char txid_hex[65];
            for (int j = 0; j < 32; j++)
                snprintf(txid_hex + 2*j, 3, "%02x",
                         txids[i].txid[j]);

            char params[256];
            snprintf(params, sizeof(params), "[\"%s\"]", txid_hex);
            char gtx[4096] = "";
            if (wv_rpc_call("gettransaction", params, gtx, sizeof(gtx)) > 0) {
                int confs = 0;
                const char *scan = gtx;
                json_next_int(&scan, "confirmations", &confs);
                if (confs > 0 && chain_tip > 0) {
                    int tx_height = chain_tip - confs + 1;
                    if (db_wallet_tx_update_block_height(&ndb, txids[i].txid,
                                                         tx_height))
                        updated++;
                }
            }
            /* Limit RPC calls to avoid slowdown on first sync */
            if (updated >= 50)
                break;
        }
    }

    node_db_close(&ndb);
    for (size_t i = 0; i < utxo_count; i++)
        db_wallet_utxo_free(&utxos[i]);
    free(utxos);
    free(notes);
    g_balance_dirty = 1;
    return;

cleanup:
    node_db_close(&ndb);
    for (size_t i = 0; i < utxo_count; i++)
        db_wallet_utxo_free(&utxos[i]);
    free(utxos);
    free(notes);
}

/* ── Query helpers ─────────────────────────────────────────── */

/* sql_query_int() and sql_query_i64() provided by controllers/explorer_internal.h */
#define query_int sql_query_int
#define query_int64 sql_query_i64

int wv_query_int(sqlite3 *db, const char *sql) {
    return query_int(db, sql);
}

int64_t wv_query_int64(sqlite3 *db, const char *sql) {
    return query_int64(db, sql);
}

int wv_effective_tip(sqlite3 *db) {
    struct node_db ndb;

    if (!db)
        return 0;
    memset(&ndb, 0, sizeof(ndb));
    ndb.db = db;
    ndb.open = true;
    return db_wallet_effective_tip_height(&ndb);
}

/* ── Funded z-address lookup ───────────────────────────────── */

void wv_get_funded_zaddr(char *out, size_t max, double *out_balance) {
    out[0] = '\0';
    if (out_balance) *out_balance = 0;

    char buf[65536] = "";
    int rc = wv_rpc_call("z_listunspent", "[0]", buf, sizeof(buf));
    if (rc <= 0) return;

    /* Aggregate balances per z-address */
    struct { char addr[256]; double total; } addrs[16];
    int n_addrs = 0;

    const char *p = buf;
    while ((p = strstr(p, "\"address\"")) != NULL) {
        p += 9;
        const char *q = strchr(p, '"');
        if (!q) break;
        q++;
        const char *end = strchr(q, '"');
        if (!end || (size_t)(end - q) >= 256) { p = end ? end : q; continue; }

        char addr[256];
        size_t alen = (size_t)(end - q);
        memcpy(addr, q, alen);
        addr[alen] = '\0';

        /* Find amount for this entry */
        const char *amt_p = strstr(end, "\"amount\"");
        if (!amt_p) break;
        amt_p += 8;
        while (*amt_p && (*amt_p == ' ' || *amt_p == ':' || *amt_p == '\t'))
            amt_p++;
        double amt = strtod(amt_p, NULL);

        /* Aggregate into addrs[] */
        bool found = false;
        for (int i = 0; i < n_addrs; i++) {
            if (strcmp(addrs[i].addr, addr) == 0) {
                addrs[i].total += amt;
                found = true;
                break;
            }
        }
        if (!found && n_addrs < 16) {
            snprintf(addrs[n_addrs].addr, 256, "%s", addr);
            addrs[n_addrs].total = amt;
            n_addrs++;
        }
        p = amt_p;
    }

    /* Find the address with the highest balance */
    double best = 0;
    int best_idx = -1;
    for (int i = 0; i < n_addrs; i++) {
        if (addrs[i].total > best) {
            best = addrs[i].total;
            best_idx = i;
        }
    }
    if (best_idx >= 0) {
        snprintf(out, max, "%s", addrs[best_idx].addr);
        if (out_balance) *out_balance = best;
    }
}

/* ── Txid formatting ───────────────────────────────────────── */

void wv_txid_short(const char *hex, char *out, size_t out_max) {
    if (!hex || !out || out_max < 18) { if (out && out_max > 0) out[0] = '\0'; return; }
    size_t len = strlen(hex);
    if (len < 8) { snprintf(out, out_max, "%s", hex); return; }
    snprintf(out, out_max, "%.8s...%.4s", hex, len >= 4 ? hex + len - 4 : hex);
}

void wv_txid_lower(const char *hex, char *out, size_t out_max) {
    if (!hex || !out || out_max == 0) return;
    size_t len = strlen(hex);
    if (len >= out_max) len = out_max - 1;
    for (size_t i = 0; i < len; i++)
        out[i] = (hex[i] >= 'A' && hex[i] <= 'F') ? (char)(hex[i] + 32) : hex[i];
    out[len] = '\0';
}

/* ── Time formatting ───────────────────────────────────────── */

void wv_format_time(int64_t timestamp, char *out, size_t out_max) {
    zcl_format_time(out, out_max, timestamp);
}

void wv_format_relative_time(int64_t timestamp, char *out, size_t out_max) {
    if (!out || out_max == 0) return;
    out[0] = '\0';
    if (timestamp <= 0) { snprintf(out, out_max, "Just now"); return; }
    time_t now = time(NULL);
    int64_t diff = (int64_t)now - timestamp;
    if (diff < 0) { snprintf(out, out_max, "just now"); return; }
    if (diff < 60)    { snprintf(out, out_max, "%d second%s ago", (int)diff, diff == 1 ? "" : "s"); return; }
    if (diff < 3600)  { int m = (int)(diff / 60);  snprintf(out, out_max, "%d minute%s ago", m, m == 1 ? "" : "s"); return; }
    if (diff < 86400) { int h = (int)(diff / 3600); snprintf(out, out_max, "%d hour%s ago", h, h == 1 ? "" : "s"); return; }
    if (diff < 2592000)  { int d = (int)(diff / 86400);   snprintf(out, out_max, "%d day%s ago", d, d == 1 ? "" : "s"); return; }
    if (diff < 31536000) { int mo = (int)(diff / 2592000); snprintf(out, out_max, "%d month%s ago", mo, mo == 1 ? "" : "s"); return; }
    int y = (int)(diff / 31536000);
    snprintf(out, out_max, "%d year%s ago", y, y == 1 ? "" : "s");
}

/* ── Balance queries ───────────────────────────────────────── */

int64_t wv_query_ground_truth_balance(sqlite3 *db, int *utxo_count) {
    struct node_db ndb;

    if (!db)
        return 0;
    memset(&ndb, 0, sizeof(ndb));
    ndb.db = db;
    ndb.open = true;
    return db_wallet_utxo_balance_with_count(&ndb, utxo_count);
}

int64_t wv_query_shielded_balance(sqlite3 *db, int *note_count) {
    struct node_db ndb;

    if (!db)
        return 0;
    memset(&ndb, 0, sizeof(ndb));
    ndb.db = db;
    ndb.open = true;
    return db_sapling_note_balance_with_count(&ndb, note_count);
}

int64_t wv_query_speed_balance(sqlite3 *db) {
    struct node_db ndb;
    struct db_wallet_projection_summary summary;

    if (!db)
        return 0;
    memset(&ndb, 0, sizeof(ndb));
    memset(&summary, 0, sizeof(summary));
    ndb.db = db;
    ndb.open = true;
    if (!db_wallet_projection_summary(&ndb, &summary))
        return 0;
    return summary.speed_balance;
}

/* ── Shield status check ───────────────────────────────────── */

int wv_shield_check_status(void) {
    if (!g_shield_opid[0] || g_shield_pending_since == 0)
        return 0;
    if (time(NULL) - g_shield_pending_since > 600) {
        g_shield_opid[0] = '\0';
        g_shield_pending_since = 0;
        g_shield_pending_amount = 0;
        return 0;
    }
    char params[256];
    snprintf(params, sizeof(params), "[[\"%.120s\"]]", g_shield_opid);
    char buf[2048] = "";
    int rc = wv_rpc_call("z_getoperationstatus", params, buf, sizeof(buf));
    if (rc <= 0) return 1;
    if (strstr(buf, "\"success\"")) {
        g_shield_opid[0] = '\0';
        g_shield_pending_since = 0;
        g_shield_pending_amount = 0;
        g_balance_dirty = 1;
        return 2;
    }
    if (strstr(buf, "\"failed\"")) {
        g_shield_opid[0] = '\0';
        g_shield_pending_since = 0;
        g_shield_pending_amount = 0;
        return -1;
    }
    return 1;
}

/* ── Navigation ────────────────────────────────────────────── */

size_t wv_emit_nav(uint8_t *buf, size_t max, const char *active) {
    struct { const char *href; const char *label; } tabs[] = {
        { "/wallet",         "Home"    },
        { "/wallet/send",    "Send"    },
        { "/wallet/receive", "Receive" },
        { "/wallet/history", "History" },
        { "/wallet/node",    "Node"    },
    };
    int n = snprintf((char *)buf, max, "<nav class='nav' role='navigation'>");
    if (n < 0 || (size_t)n >= max) return 0;
    size_t off = (size_t)n;
    for (int i = 0; i < 5 && off < max; i++) {
        bool is_active = (strcmp(tabs[i].href, active) == 0);
        int w = snprintf((char *)buf + off, max - off,
            "<a href='%s'%s>%s</a>",
            tabs[i].href,
            is_active ? " class='active'" : "",
            tabs[i].label);
        if (w > 0 && (size_t)w < max - off) off += (size_t)w;
    }
    int w = snprintf((char *)buf + off, max - off, "</nav>");
    if (w > 0 && (size_t)w < max - off) off += (size_t)w;
    return off;
}

/* ── QR Code Generator (Byte Mode, Version 5-L) ───────────── */

#define QR_N 37

static uint8_t gf_exp_table[256];
static uint8_t gf_log_table[256];
static bool    gf_initialized = false;

static void gf_init(void) {
    if (gf_initialized) return;
    int v = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp_table[i] = (uint8_t)v;
        gf_log_table[v] = (uint8_t)i;
        v <<= 1;
        if (v >= 256) v ^= 0x11d;
    }
    gf_exp_table[255] = gf_exp_table[0];
    gf_initialized = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp_table[(gf_log_table[a] + gf_log_table[b]) % 255];
}

static void rs_encode(const uint8_t *data, int data_len,
                      uint8_t *ecc, int ecc_len) {
    gf_init();

    uint8_t gen[32];
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;
    for (int i = 0; i < ecc_len; i++) {
        uint8_t alpha_i = gf_exp_table[i];
        for (int j = ecc_len; j >= 1; j--) {
            gen[j] = gen[j - 1] ^ gf_mul(gen[j], alpha_i);
        }
        gen[0] = gf_mul(gen[0], alpha_i);
    }

    uint8_t rem[32];
    memset(rem, 0, sizeof(rem));
    for (int i = 0; i < data_len; i++) {
        uint8_t lead = data[i] ^ rem[ecc_len - 1];
        for (int j = ecc_len - 1; j >= 1; j--)
            rem[j] = rem[j - 1] ^ gf_mul(lead, gen[j]);
        rem[0] = gf_mul(lead, gen[0]);
    }
    for (int i = 0; i < ecc_len; i++)
        ecc[i] = rem[ecc_len - 1 - i];
}

typedef struct {
    uint8_t bits[256];
    int     count;
} qr_bitbuf;

static void bb_append(qr_bitbuf *bb, uint32_t val, int nbits) {
    for (int i = nbits - 1; i >= 0 && bb->count < (int)sizeof(bb->bits) * 8; i--) {
        int byte_idx = bb->count / 8;
        int bit_idx  = 7 - (bb->count % 8);
        if ((val >> i) & 1)
            bb->bits[byte_idx] |= (uint8_t)(1 << bit_idx);
        bb->count++;
    }
}

#define QR_DATA_CW 108
#define QR_ECC_CW  26
#define QR_TOTAL_CW (QR_DATA_CW + QR_ECC_CW)

static int qr_encode_bytes(const char *str, uint8_t *codewords) {
    size_t len = strlen(str);
    if (len > 106)
        LOG_ERR("wallet_view", "qr_encode_bytes: input too long (%zu > 106)", len);

    qr_bitbuf bb;
    memset(&bb, 0, sizeof(bb));

    bb_append(&bb, 0x4, 4);
    bb_append(&bb, (uint32_t)len, 8);

    for (size_t i = 0; i < len; i++)
        bb_append(&bb, (uint32_t)(uint8_t)str[i], 8);

    int data_bits = QR_DATA_CW * 8;
    int pad_bits = data_bits - bb.count;
    if (pad_bits > 4) pad_bits = 4;
    if (pad_bits > 0) bb_append(&bb, 0, pad_bits);

    if (bb.count % 8 != 0)
        bb_append(&bb, 0, 8 - (bb.count % 8));

    int bytes_filled = bb.count / 8;
    bool toggle = false;
    while (bytes_filled < QR_DATA_CW) {
        bb_append(&bb, toggle ? 0x11 : 0xEC, 8);
        bytes_filled++;
        toggle = !toggle;
    }

    memcpy(codewords, bb.bits, QR_DATA_CW);
    return 0;
}

static void qr_place_finder(uint8_t m[QR_N][QR_N], int row, int col) {
    for (int dr = -1; dr <= 7; dr++) {
        for (int dc = -1; dc <= 7; dc++) {
            int r = row + dr, c = col + dc;
            if (r < 0 || r >= QR_N || c < 0 || c >= QR_N) continue;
            bool is_border = (dr == -1 || dr == 7 || dc == -1 || dc == 7);
            bool is_outer  = (dr == 0 || dr == 6 || dc == 0 || dc == 6);
            bool is_inner  = (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4);
            m[r][c] = (is_border) ? 0 : (is_outer || is_inner) ? 1 : 0;
        }
    }
}

static void qr_place_alignment(uint8_t m[QR_N][QR_N]) {
    int cr = 30, cc = 30;
    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            bool is_edge = (dr == -2 || dr == 2 || dc == -2 || dc == 2);
            bool is_center = (dr == 0 && dc == 0);
            m[cr + dr][cc + dc] = (is_edge || is_center) ? 1 : 0;
        }
    }
}

static void qr_place_timing(uint8_t m[QR_N][QR_N]) {
    for (int i = 8; i < QR_N - 8; i++) {
        m[6][i] = (i % 2 == 0) ? 1 : 0;
        m[i][6] = (i % 2 == 0) ? 1 : 0;
    }
}

static void qr_place_format(uint8_t m[QR_N][QR_N]) {
    uint16_t fmt = 0x77C4;
    static const int hpos_r[] = {8,8,8,8,8,8,8,8,8};
    static const int hpos_c[] = {0,1,2,3,4,5,7,8,24};
    for (int i = 0; i <= 5; i++)
        m[8][i] = (fmt >> (14 - i)) & 1;
    m[8][7] = (fmt >> 8) & 1;
    m[8][8] = (fmt >> 7) & 1;
    m[7][8] = (fmt >> 6) & 1;
    for (int i = 0; i <= 4; i++)
        m[5 - i][8] = (fmt >> (5 - i)) & 1;

    for (int i = 0; i < 7; i++)
        m[8][QR_N - 1 - i] = (fmt >> i) & 1;

    for (int i = 0; i < 7; i++)
        m[QR_N - 7 + i][8] = (fmt >> (6 - i)) & 1;

    m[QR_N - 8][8] = 1;

    (void)hpos_r; (void)hpos_c;
}

static bool qr_is_function(int r, int c) {
    if (r <= 8 && c <= 8) return true;
    if (r <= 8 && c >= QR_N - 8) return true;
    if (r >= QR_N - 8 && c <= 8) return true;
    if (r == 6 || c == 6) return true;
    if (r >= 28 && r <= 32 && c >= 28 && c <= 32) return true;
    return false;
}

static void qr_place_data(uint8_t m[QR_N][QR_N], const uint8_t *bits, int nbits) {
    int bit_idx = 0;
    for (int right = QR_N - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        bool upward = ((QR_N - 1 - right) / 2) % 2 == 0;
        for (int cnt = 0; cnt < QR_N; cnt++) {
            int row = upward ? (QR_N - 1 - cnt) : cnt;
            for (int dx = 0; dx <= 1; dx++) {
                int col = right - dx;
                if (col < 0) continue;
                if (qr_is_function(row, col)) continue;
                if (bit_idx < nbits) {
                    int byte_i = bit_idx / 8;
                    int bit_i  = 7 - (bit_idx % 8);
                    m[row][col] = (bits[byte_i] >> bit_i) & 1;
                }
                bit_idx++;
            }
        }
    }
}

static void qr_apply_mask(uint8_t m[QR_N][QR_N]) {
    for (int r = 0; r < QR_N; r++)
        for (int c = 0; c < QR_N; c++)
            if (!qr_is_function(r, c) && ((r + c) % 2 == 0))
                m[r][c] ^= 1;
}

size_t wv_emit_qr_svg(uint8_t *buf, size_t max, size_t off,
                       const char *data, int module_size) {
    if (!data || strlen(data) == 0 || strlen(data) > 106) return off;

    uint8_t data_cw[QR_DATA_CW];
    if (qr_encode_bytes(data, data_cw) != 0) return off;

    uint8_t ecc_cw[QR_ECC_CW];
    rs_encode(data_cw, QR_DATA_CW, ecc_cw, QR_ECC_CW);

    uint8_t all_cw[QR_TOTAL_CW];
    memcpy(all_cw, data_cw, QR_DATA_CW);
    memcpy(all_cw + QR_DATA_CW, ecc_cw, QR_ECC_CW);

    uint8_t m[QR_N][QR_N];
    memset(m, 0, sizeof(m));

    qr_place_finder(m, 0, 0);
    qr_place_finder(m, 0, QR_N - 7);
    qr_place_finder(m, QR_N - 7, 0);
    qr_place_alignment(m);
    qr_place_timing(m);
    qr_place_format(m);
    qr_place_data(m, all_cw, QR_TOTAL_CW * 8);
    qr_apply_mask(m);
    qr_place_format(m);

    int quiet = 4;
    int total = QR_N + quiet * 2;
    int px = total * module_size;

    APPEND(off, buf, max,
        "<div class='qr-wrap'>"
        "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' "
        "viewBox='0 0 %d %d' style='margin:0 auto;display:block'>",
        px, px, total, total);

    APPEND(off, buf, max,
        "<rect width='%d' height='%d' fill='white'/>", total, total);

    for (int r = 0; r < QR_N; r++) {
        for (int c = 0; c < QR_N; c++) {
            if (m[r][c] && off + 80 < max) {
                APPEND(off, buf, max,
                    "<rect x='%d' y='%d' width='1' height='1' fill='black'/>",
                    c + quiet, r + quiet);
            }
        }
    }

    APPEND(off, buf, max, "</svg></div>");
    return off;
}

/* ── Page header / footer ──────────────────────────────────── */

size_t wv_emit_header(uint8_t *buf, size_t max, const char *title,
                      const char *active_tab) {
    size_t off = 0;
    APPEND(off, buf, max,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title><style>%s</style></head><body>",
        title, CSS_WALLET);
    off += wv_emit_nav(buf + off, max - off, active_tab);
    APPEND(off, buf, max, "<main>");
    return off;
}

void wv_emit_footer(uint8_t *buf, size_t max, size_t *off) {
    APPEND(*off, buf, max, "</main>");
    APPEND(*off, buf, max,
        "<div id='sbar' class='status-bar'>"
        "<span style='color:#34d399;font-weight:700'>ZCL23</span>"
        "<span id='sb-h'>Block --</span>"
        "<span id='sb-p'>0 peers</span>"
        "<span id='sb-m'>0 tx</span>"
        "</div>"
        "<script>"
        "(function(){"
        "var u='zcl://node/api/wallet/pulse';"
        "function up(){"
        "fetch(u).then(function(r){return r.json()}).then(function(d){"
        "var h=document.getElementById('sb-h');"
        "var p=document.getElementById('sb-p');"
        "var m=document.getElementById('sb-m');"
        "if(h)h.textContent='Block '+d.height;"
        "if(p)p.textContent=d.peers+' peers';"
        "if(m)m.textContent=d.mempool+' tx';"
        "if(window._dashUpdate)window._dashUpdate(d);"
        "}).catch(function(){});}"
        "up();setInterval(up,5000);"
        "})();"
        "</script>"
        "</body></html>");
}

/* ── URL decoding + form parsing ───────────────────────────── */

static void url_decode(char *dst, size_t dstmax, const char *src) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dstmax - 1; si++) {
        if (src[si] == '%' && src[si+1] && src[si+2]) {
            char hex[3] = { src[si+1], src[si+2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

bool wv_parse_form_field(const uint8_t *body, size_t body_len,
                          const char *key, char *out, size_t outmax) {
    if (!body || !key || !out || outmax == 0)
        LOG_FAIL("wallet_view", "parse_form_field: NULL arg (body=%p key=%p out=%p outmax=%zu)",
                 (const void *)body, (const void *)key, (void *)out, outmax);
    size_t klen = strlen(key);
    const char *p = (const char *)body;
    const char *end = p + body_len;
    while (p < end) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *ve = p;
            while (ve < end && *ve != '&') ve++;
            size_t vlen = (size_t)(ve - p);
            if (vlen >= outmax) vlen = outmax - 1;
            char encoded[512];
            if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
            memcpy(encoded, p, vlen);
            encoded[vlen] = '\0';
            url_decode(out, outmax, encoded);
            return true;
        }
        while (p < end && *p != '&') p++;
        if (p < end) p++;
    }
    out[0] = '\0';
    LOG_FAIL("wallet_view", "parse_form_field: key '%s' not found in body (%zu bytes)", key, body_len);
}

/* ── Address validation ────────────────────────────────────── */

bool wv_validate_zcl_address(const char *addr) {
    return zcl_validate_zcl_address(addr);
}
