/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet view shared helpers — extracted from wallet_view_controller.c.
 * Global state, DB access, RPC, QR codes, HTML chrome, form parsing. */

#include "controllers/wallet_view_internal.h"
#include "views/wallet_css.h"
#include "crypto/sha256.h"
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
    /* Try cookie first */
    snprintf(path, sizeof(path), "%s/.zclassic/.cookie", home);
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(auth, 1, sizeof(auth) - 1, f);
        fclose(f);
        auth[n] = '\0';
        char *nl = strchr(auth, '\n'); if (nl) *nl = '\0';
        if (auth[0]) return auth;
    }
    /* Fall back to zclassic.conf */
    snprintf(path, sizeof(path), "%s/.zclassic/zclassic.conf", home);
    f = fopen(path, "r");
    if (!f) { snprintf(auth, sizeof(auth), "zcluser:zclpass"); return auth; }
    char user[64] = "", pass[64] = "", line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "rpcuser=", 8) == 0) {
            char *e = strchr(line + 8, '\n'); if (e) *e = '\0';
            snprintf(user, sizeof(user), "%s", line + 8);
        }
        if (strncmp(line, "rpcpassword=", 12) == 0) {
            char *e = strchr(line + 12, '\n'); if (e) *e = '\0';
            snprintf(pass, sizeof(pass), "%s", line + 12);
        }
    }
    fclose(f);
    if (user[0] && pass[0])
        snprintf(auth, sizeof(auth), "%s:%s", user, pass);
    else
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
        if (!g_wv_datadir) return -1;

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
            if (!f) return -1;
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
            if (!user[0] || !pass[0]) return -1;
            snprintf(cookie, sizeof(cookie), "%s:%s", user, pass);
        } else {
            size_t n = fread(cookie, 1, sizeof(cookie) - 1, f);
            fclose(f);
            cookie[n] = '\0';
            char *nl = strchr(cookie, '\n'); if (nl) *nl = '\0';
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

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
        return -1;
    }

    char body[1024];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
        method, params_json);
    if (blen < 0 || (size_t)blen >= sizeof(body)) { close(fd); return -1; }

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

    if (write(fd, req, (size_t)rlen) != rlen) { close(fd); return -1; }

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

void wv_get_funded_taddr(char *out, size_t max) {
    out[0] = '\0';
    char lu[8192] = "";
    if (wv_rpc_call("listunspent", "[]", lu, sizeof(lu)) <= 0)
        return;
    const char *best = NULL;
    size_t blen = 0;
    double bval = 0;
    const char *p = lu;
    while ((p = strstr(p, "\"address\"")) != NULL) {
        p += 9;
        while (*p == ' ' || *p == ':' || *p == '"') p++;
        const char *a = p;
        while (*p && *p != '"') p++;
        size_t al = (size_t)(p - a);
        const char *am = strstr(p, "\"amount\"");
        if (am) {
            am += 8;
            while (*am == ' ' || *am == ':') am++;
            double v = strtod(am, NULL);
            if (v > bval && al > 20 && al < 64) {
                best = a; blen = al; bval = v;
            }
        }
    }
    if (best && blen + 1 <= max) {
        memcpy(out, best, blen);
        out[blen] = '\0';
    }
}

/* ── DB helpers ────────────────────────────────────────────── */

sqlite3 *wv_open_db(void) {
    if (!g_wv_datadir) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_wv_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 3000);
    return db;
}

sqlite3 *wv_open_db_rw(void) {
    if (!g_wv_datadir) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_wv_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

/* ── Contacts (address book) ───────────────────────────────── */

static void ensure_contacts_table(sqlite3 *db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS contacts ("
        "  address TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  last_used INTEGER DEFAULT 0"
        ")", NULL, NULL, NULL);
}

void wv_save_contact(const char *address, const char *name) {
    sqlite3 *db = wv_open_db_rw();
    if (!db) return;
    ensure_contacts_table(db);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO contacts (address, name, last_used) "
            "VALUES (?, ?, strftime('%s','now'))",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, address, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
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

/* ── Sync wallet from zclassicd ────────────────────────────── */

void wv_sync_wallet_from_zclassicd(void) {
    if (!g_sync_enabled) return;
    sqlite3 *db = wv_open_db_rw();
    if (!db) return;

    /* Fetch transparent UTXOs from zclassicd */
    char lu[65536] = "";
    int lu_rc = wv_rpc_call("listunspent", "[0]", lu, sizeof(lu));
    if (lu_rc <= 0) { sqlite3_close(db); return; }

    /* Fetch shielded notes from zclassicd */
    char zlu[65536] = "";
    int zlu_rc = wv_rpc_call("z_listunspent", "[0]", zlu, sizeof(zlu));
    if (zlu_rc <= 0) { sqlite3_close(db); return; }

    /* Sanity: response must contain "result" and at least one entry.
     * If response is an error or empty, don't wipe the DB. */
    if (!strstr(lu, "\"result\"") || !strstr(lu, "\"txid\"")) {
        sqlite3_close(db); return;
    }

    /* Both RPCs succeeded — parse into temp table first, then swap.
     * If parsing produces 0 results, don't touch the real tables. */
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TEMP TABLE new_utxos AS SELECT * FROM wallet_utxos WHERE 0",
        NULL, NULL, NULL);

    /* Get chain tip for height computation: height = tip - confirmations + 1 */
    int chain_tip = 0;
    { sqlite3_stmt *tip_s = NULL;
      if (sqlite3_prepare_v2(db, "SELECT MAX(height) FROM blocks",
                              -1, &tip_s, NULL) == SQLITE_OK) {
          if (sqlite3_step(tip_s) == SQLITE_ROW)
              chain_tip = sqlite3_column_int(tip_s, 0);
          sqlite3_finalize(tip_s);
      }
    }

    sqlite3_stmt *ins_utxo = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO new_utxos "
        "(txid,vout,value,address_hash,script,height,is_coinbase,spent_txid) "
        "VALUES (?,?,?,?,?,?,0,NULL)", -1, &ins_utxo, NULL);

    if (ins_utxo) {
        const char *p = lu;
        const char *txid_s; size_t txid_l;
        while (json_next_str(&p, "txid", &txid_s, &txid_l)) {
            if (txid_l != 64) continue;
            uint8_t txid_bin[32];
            if (hex_to_bin(txid_s, 64, txid_bin, 32) != 32) continue;

            int vout = 0, confs = 0;
            double amt = 0;
            const char *script_s; size_t script_l;
            const char *scan = p;
            json_next_int(&scan, "vout", &vout);
            scan = p;
            json_next_num(&scan, "amount", &amt);
            scan = p;
            json_next_int(&scan, "confirmations", &confs);
            int64_t val = (int64_t)(amt * 1e8 + 0.5);
            int height = (chain_tip > 0 && confs > 0)
                ? (chain_tip - confs + 1) : 0;

            /* Get scriptPubKey for address_hash extraction */
            uint8_t addr_hash[20] = {0};
            uint8_t script_bin[64] = {0};
            size_t script_bin_len = 0;
            scan = p;
            if (json_next_str(&scan, "scriptPubKey", &script_s, &script_l)) {
                script_bin_len = hex_to_bin(script_s, script_l,
                                             script_bin, sizeof(script_bin));
                /* P2PKH: 76a914{20}88ac — extract 20-byte hash */
                if (script_bin_len == 25 && script_bin[0] == 0x76 &&
                    script_bin[1] == 0xa9 && script_bin[2] == 0x14) {
                    memcpy(addr_hash, script_bin + 3, 20);
                }
            }

            sqlite3_bind_blob(ins_utxo, 1, txid_bin, 32, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins_utxo, 2, vout);
            sqlite3_bind_int64(ins_utxo, 3, val);
            sqlite3_bind_blob(ins_utxo, 4, addr_hash, 20, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins_utxo, 5, script_bin,
                              (int)script_bin_len, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins_utxo, 6, height);
            sqlite3_step(ins_utxo);
            sqlite3_reset(ins_utxo);
        }
        sqlite3_finalize(ins_utxo);
    }

    /* Only replace wallet_utxos if parsing produced results */
    int new_count = 0;
    { sqlite3_stmt *cnt = NULL;
      if (sqlite3_prepare_v2(db, "SELECT count(*) FROM new_utxos",
                              -1, &cnt, NULL) == SQLITE_OK) {
          if (sqlite3_step(cnt) == SQLITE_ROW)
              new_count = sqlite3_column_int(cnt, 0);
          sqlite3_finalize(cnt);
      }
    }
    if (new_count > 0) {
        sqlite3_exec(db, "DELETE FROM wallet_utxos", NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO wallet_utxos SELECT * FROM new_utxos",
            NULL, NULL, NULL);
    }
    sqlite3_exec(db, "DROP TABLE IF EXISTS new_utxos", NULL, NULL, NULL);

    /* 2. Rebuild shielded notes (same safe pattern) */
    sqlite3_exec(db,
        "CREATE TEMP TABLE new_notes AS SELECT * FROM wallet_sapling_notes WHERE 0",
        NULL, NULL, NULL);

    sqlite3_stmt *ins_note = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO new_notes "
        "(txid,output_index,value,rcm,ivk,diversifier,pk_d,"
        "cm,nullifier,block_height,address) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?)", -1, &ins_note, NULL);

    if (ins_note) {
        const char *p = zlu;
        const char *txid_s; size_t txid_l;
        while (json_next_str(&p, "txid", &txid_s, &txid_l)) {
            if (txid_l != 64) continue;
            uint8_t txid_bin[32];
            if (hex_to_bin(txid_s, 64, txid_bin, 32) != 32) continue;

            int outindex = 0, confs = 0;
            double amt = 0;
            const char *addr_s; size_t addr_l;
            const char *scan = p;
            json_next_int(&scan, "outindex", &outindex);
            scan = p;
            json_next_num(&scan, "amount", &amt);
            scan = p;
            json_next_int(&scan, "confirmations", &confs);
            int64_t val = (int64_t)(amt * 1e8 + 0.5);
            int note_height = (chain_tip > 0 && confs > 0)
                ? (chain_tip - confs + 1) : 0;

            char addr_str[128] = "";
            scan = p;
            if (json_next_str(&scan, "address", &addr_s, &addr_l) &&
                addr_l < sizeof(addr_str)) {
                memcpy(addr_str, addr_s, addr_l);
                addr_str[addr_l] = '\0';
            }

            /* Deterministic unique placeholder crypto fields.
             * These are not real Sapling keys — just unique identifiers
             * so SQLite UNIQUE constraints don't collide. */
            uint8_t seed[36];
            memcpy(seed, txid_bin, 32);
            seed[32] = (uint8_t)(outindex & 0xFF);
            seed[33] = (uint8_t)((outindex >> 8) & 0xFF);
            seed[34] = 0; seed[35] = 0;

            /* Use SHA-256 context API for each field */
            #define HASH_FIELD(tag, taglen, out) do { \
                struct sha256_ctx _hc; \
                sha256_init(&_hc); \
                sha256_write(&_hc, (const unsigned char *)(tag), (taglen)); \
                sha256_write(&_hc, seed, 36); \
                sha256_finalize(&_hc, (out)); \
            } while(0)

            uint8_t nf[32], cm[32], rcm[32], ivk[32], pkd[32], div_full[32];
            HASH_FIELD("nf", 2, nf);
            HASH_FIELD("cm", 2, cm);
            HASH_FIELD("rcm", 3, rcm);
            HASH_FIELD("ivk", 3, ivk);
            HASH_FIELD("pkd", 3, pkd);
            HASH_FIELD("div", 3, div_full);
            #undef HASH_FIELD

            sqlite3_bind_blob(ins_note, 1, txid_bin, 32, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins_note, 2, outindex);
            sqlite3_bind_int64(ins_note, 3, val);
            sqlite3_bind_blob(ins_note, 4, rcm, 32, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins_note, 5, ivk, 32, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins_note, 6, div_full, 11, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins_note, 7, pkd, 32, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins_note, 8, cm, 32, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins_note, 9, nf, 32, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins_note, 10, note_height);
            sqlite3_bind_text(ins_note, 11, addr_str, -1, SQLITE_TRANSIENT);
            sqlite3_step(ins_note);
            sqlite3_reset(ins_note);
        }
        sqlite3_finalize(ins_note);
    }

    /* Only replace notes if parsing produced results */
    int note_count = 0;
    { sqlite3_stmt *cnt = NULL;
      if (sqlite3_prepare_v2(db, "SELECT count(*) FROM new_notes",
                              -1, &cnt, NULL) == SQLITE_OK) {
          if (sqlite3_step(cnt) == SQLITE_ROW)
              note_count = sqlite3_column_int(cnt, 0);
          sqlite3_finalize(cnt);
      }
    }
    if (note_count > 0) {
        sqlite3_exec(db, "DELETE FROM wallet_sapling_notes", NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO wallet_sapling_notes SELECT * FROM new_notes",
            NULL, NULL, NULL);
    }
    sqlite3_exec(db, "DROP TABLE IF EXISTS new_notes", NULL, NULL, NULL);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(db);
    g_balance_dirty = 1;
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
    int t = query_int(db, "SELECT MAX(height) FROM blocks");
    int u = query_int(db,
        "SELECT MAX(height) FROM wallet_utxos WHERE spent_txid IS NULL");
    return u > t ? u : t;
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
    int64_t total = 0;
    int count = 0;
    sqlite3_stmt *s = NULL;

    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(value),0), COUNT(*) "
            "FROM wallet_utxos WHERE spent_txid IS NULL",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            total = sqlite3_column_int64(s, 0);
            count = sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }

    if (utxo_count) *utxo_count = count;
    return total;
}

int64_t wv_query_shielded_balance(sqlite3 *db, int *note_count) {
    int64_t total = 0;
    int count = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(n.value),0), COUNT(*) "
            "FROM wallet_sapling_notes n"
            " WHERE NOT EXISTS ("
            "   SELECT 1 FROM sapling_spends ss"
            "   WHERE ss.nullifier = n.nullifier)",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            total = sqlite3_column_int64(s, 0);
            count = sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    if (note_count) *note_count = count;
    return total;
}

int64_t wv_query_speed_balance(sqlite3 *db) {
    return query_int64(db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_utxos"
        " WHERE spent_txid IS NULL");
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
    };
    int n = snprintf((char *)buf, max, "<nav class='nav' role='navigation'>");
    if (n < 0 || (size_t)n >= max) return 0;
    size_t off = (size_t)n;
    for (int i = 0; i < 4 && off < max; i++) {
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
    if (len > 106) return -1;

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
        title, wallet_css);
    off += wv_emit_nav(buf + off, max - off, active_tab);
    APPEND(off, buf, max, "<main>");
    return off;
}

void wv_emit_footer(uint8_t *buf, size_t max, size_t *off) {
    APPEND(*off, buf, max, "</main>");
    APPEND(*off, buf, max,
        "<div id='sbar' class='status-bar'>"
        "<span style='color:#a78bfa;font-weight:700'>Power Node</span>"
        "<span id='sb-h'>Block --</span>"
        "<span id='sb-p'>0 peers</span>"
        "<span id='sb-m'>0 pending</span>"
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
        "if(m)m.textContent=d.mempool+' pending';"
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
    if (!body || !key || !out || outmax == 0) return false;
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
    return false;
}

/* ── Address validation ────────────────────────────────────── */

bool wv_validate_zcl_address(const char *addr) {
    if (!addr || !addr[0]) return false;
    size_t alen = strlen(addr);

    /* t1 or t3: Base58Check with 2-byte version prefix */
    if ((addr[0] == 't' && (addr[1] == '1' || addr[1] == '3')) &&
        alen >= 26 && alen <= 36) {
        unsigned char decoded[64];
        size_t decoded_len = 0;
        return base58check_decode(addr, decoded, sizeof(decoded), &decoded_len);
    }

    /* zs1: Bech32 with "zs" human-readable part */
    if (alen >= 3 && addr[0] == 'z' && addr[1] == 's' && addr[2] == '1' &&
        alen >= 70) {
        char hrp[8];
        uint8_t data[128];
        size_t data_len = 0;
        if (!bech32_decode(hrp, sizeof(hrp), data, sizeof(data),
                           &data_len, addr))
            return false;
        return (strcmp(hrp, "zs") == 0 && data_len > 0);
    }

    return false;
}
