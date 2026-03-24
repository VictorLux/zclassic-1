/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet view controller — MVC HTML views for GTK browser.
 * Reads directly from SQLite. No RPC. No ports.
 *
 * Routes:
 *   /wallet           Dashboard (balance, stats, recent txs)
 *   /wallet/send      Send form with validation
 *   /wallet/receive   Receive addresses with visual encoding
 *   /wallet/history   Full transaction history
 *   /wallet/coins     UTXO and shielded note breakdown */

#include "controllers/wallet_view_controller.h"
#include "controllers/wallet_controller.h"
#include "views/format_helpers.h"
#include "views/wallet_css.h"
#include "event/event.h"
#include "encoding/base58.h"
#include "encoding/bech32.h"
#include "chain/chainparams.h"
#include "crypto/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sqlite3.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static const char *g_datadir = NULL;
static int g_balance_dirty = 0; /* set after wallet sync to bust pulse cache */

#define PRIMARY_ADDR "t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn"
#define FEE_ZCL 0.0001
#define ZCLASSICD_PORT 8232

/* Read zclassicd RPC auth from ~/.zclassic (cached) */
static const char *zclassicd_auth(void) {
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

/* ── RPC to running node for live balance ──────────────────── */

static int wallet_rpc_call_port(const char *method, const char *params_json,
                                char *out, size_t outmax,
                                uint16_t port, const char *auth_cookie)
{
    char cookie[256] = "";

    if (auth_cookie && auth_cookie[0]) {
        snprintf(cookie, sizeof(cookie), "%s", auth_cookie);
    } else {
        if (!g_datadir) return -1;

        /* Read auth cookie */
        char cookie_path[1024];
        snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", g_datadir);
        FILE *f = fopen(cookie_path, "r");
        if (!f) {
            /* Try config file credentials */
            char conf_path[1024];
            snprintf(conf_path, sizeof(conf_path), "%s/zclassic.conf", g_datadir);
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
    addr.sin_port = htons(port);

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

/* Get the best-funded transparent address from zclassicd listunspent.
 * Returns true if an address was found. */
static bool get_funded_taddr(char *out, size_t outmax) {
    out[0] = '\0';
    char lu[8192] = "";
    if (wallet_rpc_call_port("listunspent", "[]", lu, sizeof(lu),
                              ZCLASSICD_PORT, zclassicd_auth()) <= 0)
        return false;
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
    if (best && blen + 1 <= outmax) {
        memcpy(out, best, blen);
        out[blen] = '\0';
        return true;
    }
    return false;
}

/* Balance comes from SQLite only — no RPC. */

/* CSS loaded from views/wallet_css.h */

/* ── Navigation with active state ───────────────────────────── */

static size_t emit_nav(uint8_t *buf, size_t max, const char *active) {
    struct { const char *href; const char *label; } tabs[] = {
        { "/wallet",         "Dashboard" },
        { "/wallet/send",    "Send"      },
        { "/wallet/receive", "Receive"   },
        { "/wallet/history", "History"   },
        { "/wallet/coins",   "Coins"     },
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

/* ── APPEND macro — use shared from explorer_internal.h ─────── */
#include "controllers/explorer_internal.h"
#include "util/template.h"

/* APPEND macro provided by controllers/explorer_internal.h above */

/* html_escape provided by util/template.h (included above) */

/* ── DB helpers ─────────────────────────────────────────────── */

static sqlite3 *open_db(void) {
    if (!g_datadir) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 3000);
    return db;
}

static sqlite3 *open_db_rw(void) {
    if (!g_datadir) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

/* Convert hex char to nibble value, or -1 on error. */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Convert hex string to binary. Returns bytes written, or 0 on error. */
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

/* Extract next JSON string value after a key like "txid": "abc...".
 * Scans from *pos forward. On success, sets *val and *vlen, advances
 * *pos past the closing quote, returns true. */
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
    /* Find closing quote, skipping escaped quotes */
    const char *end = p;
    while (*end && !(*end == '"' && (end == p || *(end-1) != '\\'))) end++;
    if (!*end) return false;
    *val = p;
    *vlen = (size_t)(end - p);
    *pos = end + 1;
    return true;
}

/* Extract next JSON number value after a key. */
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

/* Sync wallet_utxos and wallet_sapling_notes from zclassicd.
 * Uses atomic transaction: if RPC fails, DB is not modified.
 * Called after shield/send operations to keep balance accurate. */
static bool g_sync_enabled = false; /* Only enable after GUI starts */

static void sync_wallet_from_zclassicd(void) {
    if (!g_sync_enabled) return;
    sqlite3 *db = open_db_rw();
    if (!db) return;

    /* Fetch transparent UTXOs from zclassicd */
    char lu[65536] = "";
    int lu_rc = wallet_rpc_call_port("listunspent", "[0]", lu, sizeof(lu),
                                      ZCLASSICD_PORT, zclassicd_auth());
    if (lu_rc <= 0) { sqlite3_close(db); return; }

    /* Fetch shielded notes from zclassicd */
    char zlu[65536] = "";
    int zlu_rc = wallet_rpc_call_port("z_listunspent", "[0]", zlu, sizeof(zlu),
                                       ZCLASSICD_PORT, zclassicd_auth());
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

    sqlite3_stmt *ins_utxo = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO new_utxos "
        "(txid,vout,value,address_hash,script,height,is_coinbase,spent_txid) "
        "VALUES (?,?,?,?,?,0,0,NULL)", -1, &ins_utxo, NULL);

    if (ins_utxo) {
        const char *p = lu;
        const char *txid_s; size_t txid_l;
        while (json_next_str(&p, "txid", &txid_s, &txid_l)) {
            if (txid_l != 64) continue;
            uint8_t txid_bin[32];
            if (hex_to_bin(txid_s, 64, txid_bin, 32) != 32) continue;

            int vout = 0;
            double amt = 0;
            const char *script_s; size_t script_l;
            const char *scan = p;
            json_next_int(&scan, "vout", &vout);
            scan = p;
            json_next_num(&scan, "amount", &amt);
            int64_t val = (int64_t)(amt * 1e8 + 0.5);

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
        "VALUES (?,?,?,?,?,?,?,?,?,0,?)", -1, &ins_note, NULL);

    if (ins_note) {
        const char *p = zlu;
        const char *txid_s; size_t txid_l;
        while (json_next_str(&p, "txid", &txid_s, &txid_l)) {
            if (txid_l != 64) continue;
            uint8_t txid_bin[32];
            if (hex_to_bin(txid_s, 64, txid_bin, 32) != 32) continue;

            int outindex = 0;
            double amt = 0;
            const char *addr_s; size_t addr_l;
            const char *scan = p;
            json_next_int(&scan, "outindex", &outindex);
            scan = p;
            json_next_num(&scan, "amount", &amt);
            int64_t val = (int64_t)(amt * 1e8 + 0.5);

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
            sqlite3_bind_text(ins_note, 10, addr_str, -1, SQLITE_TRANSIENT);
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

/* sql_query_int() and sql_query_i64() provided by controllers/explorer_internal.h */
#define query_int sql_query_int
#define query_int64 sql_query_i64

/* ── Txid formatting ────────────────────────────────────────── */

static void txid_short(const char *hex, char *out, size_t out_max) {
    if (!hex || !out || out_max < 18) { if (out && out_max > 0) out[0] = '\0'; return; }
    size_t len = strlen(hex);
    if (len < 8) { snprintf(out, out_max, "%s", hex); return; }
    snprintf(out, out_max, "%.8s...%.4s", hex, len >= 4 ? hex + len - 4 : hex);
}

static void txid_lower(const char *hex, char *out, size_t out_max) {
    if (!hex || !out || out_max == 0) return;
    size_t len = strlen(hex);
    if (len >= out_max) len = out_max - 1;
    for (size_t i = 0; i < len; i++)
        out[i] = (hex[i] >= 'A' && hex[i] <= 'F') ? (char)(hex[i] + 32) : hex[i];
    out[len] = '\0';
}

/* Time formatting — delegates to shared zcl_format_time */
static void format_time(int64_t timestamp, char *out, size_t out_max) {
    zcl_format_time(out, out_max, timestamp);
}

/* ── Relative time formatting ───────────────────────────────── */

static void format_relative_time(int64_t timestamp, char *out, size_t out_max) {
    if (!out || out_max == 0) return;
    out[0] = '\0';
    if (timestamp <= 0) { snprintf(out, out_max, "Unconfirmed"); return; }
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

/* ── QR Code Generator (Alphanumeric Mode, Version 2-L) ────── */

/* GF(256) arithmetic for Reed-Solomon over QR's field polynomial
 * x^8 + x^4 + x^3 + x^2 + 1 = 0x11d */

#define QR_N 29

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

/* Reed-Solomon: generate ECC codewords.
 * Version 2-L: 34 data codewords, 10 ECC codewords.
 * Generator polynomial for 10 ECC codewords:
 *   product of (x - alpha^i) for i = 0..9 */
static void rs_encode(const uint8_t *data, int data_len,
                      uint8_t *ecc, int ecc_len) {
    gf_init();

    /* Build generator polynomial coefficients */
    uint8_t gen[16];
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;
    for (int i = 0; i < ecc_len; i++) {
        uint8_t alpha_i = gf_exp_table[i];
        for (int j = ecc_len; j >= 1; j--) {
            gen[j] = gen[j - 1] ^ gf_mul(gen[j], alpha_i);
        }
        gen[0] = gf_mul(gen[0], alpha_i);
    }

    /* Polynomial division */
    uint8_t rem[16];
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

/* Alphanumeric mode character map */

/* Bit buffer for QR data encoding */
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

/* Build QR Version 3-L data codewords using BYTE MODE.
 * Version 3-L: 70 total codewords, 55 data, 15 ECC.
 * Byte mode capacity: 53 chars (enough for 34-char t-address).
 * Byte mode supports ALL characters including lowercase. */
#define QR_DATA_CW 55
#define QR_ECC_CW  15
#define QR_TOTAL_CW (QR_DATA_CW + QR_ECC_CW)

static int qr_encode_bytes(const char *str, uint8_t *codewords) {
    size_t len = strlen(str);
    if (len > 53) return -1;

    qr_bitbuf bb;
    memset(&bb, 0, sizeof(bb));

    /* Mode indicator: byte = 0100 */
    bb_append(&bb, 0x4, 4);
    /* Character count: 8 bits for version 3 byte mode */
    bb_append(&bb, (uint32_t)len, 8);

    /* Encode each byte directly */
    for (size_t i = 0; i < len; i++)
        bb_append(&bb, (uint32_t)(uint8_t)str[i], 8);

    /* Terminator (up to 4 bits) */
    int data_bits = QR_DATA_CW * 8;
    int pad_bits = data_bits - bb.count;
    if (pad_bits > 4) pad_bits = 4;
    if (pad_bits > 0) bb_append(&bb, 0, pad_bits);

    /* Byte-align */
    if (bb.count % 8 != 0)
        bb_append(&bb, 0, 8 - (bb.count % 8));

    /* Pad codewords */
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

/* Place modules in the QR matrix (Version 2, 25x25).
 * matrix[row][col]: 0=white, 1=black, 2=reserved (not yet set) */
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
    /* Version 2 alignment pattern at (18,18) */
    int cr = 22, cc = 22;
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

/* Place format information (error correction L, mask 0) */
static void qr_place_format(uint8_t m[QR_N][QR_N]) {
    /* Format string for ECC L, mask 0: 111011111000100 (after masking with 101010000010010) */
    uint16_t fmt = 0x77C4;  /* pre-computed: L + mask 0 */
    /* Horizontal: bits 14..0 placed around top-left finder */
    static const int hpos_r[] = {8,8,8,8,8,8,8,8,8};
    static const int hpos_c[] = {0,1,2,3,4,5,7,8,24};
    /* Bits 0-5 at (8, 0-5), bit 6 at (8, 7), bit 7 at (8, 8), bit 8 at (7, 8) */
    for (int i = 0; i <= 5; i++)
        m[8][i] = (fmt >> (14 - i)) & 1;
    m[8][7] = (fmt >> 8) & 1;
    m[8][8] = (fmt >> 7) & 1;
    m[7][8] = (fmt >> 6) & 1;
    for (int i = 0; i <= 4; i++)
        m[5 - i][8] = (fmt >> (5 - i)) & 1;

    /* Right side of top-left: bits 7-14 at (8, 25-8..25-1) */
    for (int i = 0; i < 7; i++)
        m[8][QR_N - 1 - i] = (fmt >> i) & 1;

    /* Bottom side of top-left: bits 7-14 at (25-7..25-1, 8) */
    for (int i = 0; i < 7; i++)
        m[QR_N - 7 + i][8] = (fmt >> (6 - i)) & 1;

    /* Dark module */
    m[QR_N - 8][8] = 1;

    (void)hpos_r; (void)hpos_c;
}

/* Check if a module is reserved (finder, timing, alignment, format, etc.) */
static bool qr_is_function(int r, int c) {
    /* Finder + separator: top-left, top-right, bottom-left */
    if (r <= 8 && c <= 8) return true;           /* top-left */
    if (r <= 8 && c >= QR_N - 8) return true;      /* top-right */
    if (r >= QR_N - 8 && c <= 8) return true;       /* bottom-left */
    /* Timing */
    if (r == 6 || c == 6) return true;
    /* Alignment at (18,18) */
    if (r >= 20 && r <= 24 && c >= 20 && c <= 24) return true;
    return false;
}

/* Place data bits in the QR matrix using the standard upward-rightward zigzag */
static void qr_place_data(uint8_t m[QR_N][QR_N], const uint8_t *bits, int nbits) {
    int bit_idx = 0;
    /* Column pairs from right to left, skip column 6 (timing) */
    for (int right = QR_N - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;  /* skip timing column */
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

/* Apply mask pattern 0: (row + col) % 2 == 0 */
static void qr_apply_mask(uint8_t m[QR_N][QR_N]) {
    for (int r = 0; r < QR_N; r++)
        for (int c = 0; c < QR_N; c++)
            if (!qr_is_function(r, c) && ((r + c) % 2 == 0))
                m[r][c] ^= 1;
}

static size_t emit_qr_svg(uint8_t *buf, size_t max, size_t off,
                           const char *data, int module_size) {
    if (!data || strlen(data) == 0 || strlen(data) > 53) return off;

    /* Encode data to codewords */
    uint8_t data_cw[QR_DATA_CW];
    if (qr_encode_bytes(data, data_cw) != 0) return off;

    /* Reed-Solomon: 10 ECC codewords for Version 2-L */
    uint8_t ecc_cw[QR_ECC_CW];
    rs_encode(data_cw, QR_DATA_CW, ecc_cw, QR_ECC_CW);

    /* Combine data + ECC into bit stream */
    uint8_t all_cw[QR_TOTAL_CW];
    memcpy(all_cw, data_cw, QR_DATA_CW);
    memcpy(all_cw + QR_DATA_CW, ecc_cw, QR_ECC_CW);

    /* Build the 25x25 matrix */
    uint8_t m[QR_N][QR_N];
    memset(m, 0, sizeof(m));

    qr_place_finder(m, 0, 0);       /* top-left */
    qr_place_finder(m, 0, QR_N - 7);  /* top-right */
    qr_place_finder(m, QR_N - 7, 0);  /* bottom-left */
    qr_place_alignment(m);
    qr_place_timing(m);
    qr_place_format(m);
    qr_place_data(m, all_cw, QR_TOTAL_CW * 8);
    qr_apply_mask(m);
    qr_place_format(m);  /* re-place format after mask (format is not masked) */

    /* Emit SVG with quiet zone of 4 modules */
    int quiet = 4;
    int total = QR_N + quiet * 2;
    int px = total * module_size;

    APPEND(off, buf, max,
        "<div class='qr-wrap'>"
        "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' "
        "viewBox='0 0 %d %d' style='margin:0 auto;display:block'>",
        px, px, total, total);

    /* White background */
    APPEND(off, buf, max,
        "<rect width='%d' height='%d' fill='white'/>", total, total);

    /* Dark modules */
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

/* ── Page header/footer ─────────────────────────────────────── */

static size_t emit_header(uint8_t *buf, size_t max, const char *title,
                          const char *active_tab) {
    size_t off = 0;
    APPEND(off, buf, max,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title><style>%s</style></head><body>",
        title, wallet_css);
    off += emit_nav(buf + off, max - off, active_tab);
    APPEND(off, buf, max, "<main>");
    return off;
}

static void emit_footer(uint8_t *buf, size_t max, size_t *off) {
    APPEND(*off, buf, max, "</main>");
    APPEND(*off, buf, max,
        "<div id='sbar' class='status-bar'>"
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

/* ── URL decoding + form parsing ────────────────────────────── */

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

static bool parse_form_field(const uint8_t *body, size_t body_len,
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

/* ── Address validation with checksum ───────────────────────── */
/* Returns true if address has a valid checksum (Base58Check for t1/t3,
 * Bech32 for zs1). This catches typos that prefix+length checks miss. */

static bool validate_zcl_address(const char *addr) {
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

/* ── Wallet balance: unspent wallet_utxos (maintained by wallet layer) ── */

static int64_t query_ground_truth_balance(sqlite3 *db, int *utxo_count) {
    int64_t total = 0;
    int count = 0;
    sqlite3_stmt *s = NULL;

    /* wallet_utxos tracks UTXOs belonging to this wallet with correct
     * spent/unspent state. This is authoritative — the global utxos table
     * may contain stale entries from incomplete UTXO pruning, and the
     * P2SH change heuristic was producing false positives. */
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

/* ── Shielded balance: verified notes minus spent nullifiers ──── */

static int64_t query_shielded_balance(sqlite3 *db, int *note_count) {
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

/* ── Speed-layer balance (wallet_utxos cache) ────────────────── */

static int64_t query_speed_balance(sqlite3 *db) {
    return query_int64(db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_utxos"
        " WHERE spent_txid IS NULL");
}

/* ── Dashboard (/wallet) ────────────────────────────────────── */

static size_t serve_dashboard(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) {
        size_t off = emit_header(r, max, "ZClassic23 Wallet", "/wallet");
        APPEND(off, r, max,
            "<div class='empty-state' style='padding:48px 0'>"
            "<div style='font-size:40px;margin-bottom:12px'>&#x23F3;</div>"
            "<div style='color:#e2e2e2;font-size:18px;font-weight:600'>"
            "Wallet Loading</div>"
            "<div style='margin-top:8px'>"
            "The database is not yet available.</div>"
            "</div>");
        emit_footer(r, max, &off);
        return off;
    }

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");

    /* Ground-truth transparent balance (P2PKH + P2SH change addresses) */
    int t_utxos = 0;
    int64_t transparent = query_ground_truth_balance(db, &t_utxos);

    /* Shielded: verified notes minus spent nullifiers */
    int z_notes = 0;
    int64_t shielded = query_shielded_balance(db, &z_notes);

    int64_t total_balance = transparent + shielded;

    size_t off = emit_header(r, max, "ZClassic23 Wallet", "/wallet");

    const char *sync_raw = sync_state_name(sync_get_state());
    bool synced = (sync_get_state() == SYNC_AT_TIP);
    /* Map internal state names to user-friendly labels */
    const char *sync = synced ? "Synced" : "Syncing...";
    if (!synced) {
        if (strstr(sync_raw, "download")) sync = "Syncing blocks...";
        else if (strstr(sync_raw, "header")) sync = "Syncing headers...";
        else if (strstr(sync_raw, "connect")) sync = "Connecting...";
        else if (strstr(sync_raw, "idle")) sync = "Ready";
        else if (strstr(sync_raw, "scan")) sync = "Scanning...";
    }

    /* Format balance — show minimal decimals */
    char bal_str[32];
    double bal_f = (double)total_balance / (double)ZATOSHI_PER_ZCL;
    if (total_balance == 0)
        snprintf(bal_str, sizeof(bal_str), "0.00");
    else if (total_balance % 1000000 == 0)
        snprintf(bal_str, sizeof(bal_str), "%.2f", bal_f);
    else if (total_balance % 10000 == 0)
        snprintf(bal_str, sizeof(bal_str), "%.4f", bal_f);
    else
        snprintf(bal_str, sizeof(bal_str), "%.8f", bal_f);

    /* 1. Sync badge (top center) */
    APPEND(off, r, max,
        "<div style='text-align:center;padding:24px 0 16px'>"
        "<span id='sync' class='pill %s sync-badge'>%s</span>"
        /* 2. Balance hero */
        "<div id='bal' class='balance' style='margin-top:8px'>"
        "%s ZCL</div>",
        synced ? "pill-synced" :
        (strstr(sync_raw, "idle") ? "pill-ready" : "pill-syncing"),
        sync,
        bal_str);

    /* 3. Breakdown line */
    APPEND(off, r, max,
        "<div id='breakdown' class='balance-sub'>"
        "%.8f transparent",
        (double)transparent / (double)ZATOSHI_PER_ZCL);
    APPEND(off, r, max, " + %.8f shielded",
        (double)shielded / (double)ZATOSHI_PER_ZCL);
    APPEND(off, r, max, "</div>");

    if (!synced && !strstr(sync_raw, "idle")) {
        APPEND(off, r, max,
            "<div class='sync-note'>"
            "Syncing &mdash; balance updating</div>");
    }
    APPEND(off, r, max, "</div>");

    /* 4. Action buttons (Send / Receive) */
    APPEND(off, r, max,
        "<div class='actions'>"
        "<a href='/wallet/send' class='btn-secondary'"
        " style='display:flex;align-items:center;justify-content:center'>Send</a>"
        "<a href='/wallet/receive' class='btn-primary'"
        " style='display:flex;align-items:center;justify-content:center'>Receive</a>"
        "</div>");

    /* 5. Privacy nudge — show when transparent balance > 0 */
    if (transparent > 0) {
        APPEND(off, r, max,
            "<div class='privacy-card'>"
            "<div class='title'>%.8f ZCL is traceable on-chain</div>"
            "<div class='desc'>Shield your transparent balance to remove "
            "the link between your address and your funds.</div>"
            "<div style='display:flex;gap:8px;justify-content:center'>"
            "<a class='btn' href='/wallet/shield?all=1'>"
            "Shield All</a>"
            "<a class='btn' style='background:#1a1428;color:#a78bfa;"
            "border:1px solid #a78bfa' href='/wallet/shield'>"
            "Shield Custom Amount</a>"
            "</div></div>",
            (double)transparent / (double)ZATOSHI_PER_ZCL);
    }

    /* 6. Recent transactions (5 items) */
    APPEND(off, r, max,
        "<div class='section-header'>"
        "<span>Recent</span>"
        "<a href='/wallet/history'>View all</a></div>");

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(wt.txid), wt.block_height, COALESCE(b.time,0), "
            "wt.from_me, "
            "COALESCE("
            "  (SELECT SUM(wu.value) FROM wallet_utxos wu WHERE wu.txid = wt.txid),"
            "  (SELECT SUM(o.value) FROM tx_outputs o "
            "    WHERE o.txid = wt.txid AND o.address_hash IN "
            "    (SELECT pubkey_hash FROM wallet_keys)),"
            "  0) "
            "FROM wallet_transactions wt "
            "LEFT JOIN blocks b ON wt.block_height = b.height "
            "ORDER BY wt.block_height DESC LIMIT 20",
            -1, &s, NULL) == SQLITE_OK) {
        int tx_shown = 0;
        while (sqlite3_step(s) == SQLITE_ROW && off + 400 < max && tx_shown < 5) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int height = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int from_me = sqlite3_column_int(s, 3);
            int64_t wallet_output = sqlite3_column_int64(s, 4);
            if (!txid) continue;
            /* Skip entries with no useful data */
            if (wallet_output == 0 && height == 0) continue;
            /* Skip zero-amount sends (shielded ops with no transparent delta) */
            if (wallet_output == 0 && from_me) continue;

            bool is_recv = (from_me == 0);

            char rel_time[48], esc_rel[96];
            format_relative_time(btime, rel_time, sizeof(rel_time));
            html_escape(esc_rel, sizeof(esc_rel), rel_time);

            char lower_tx[65];
            txid_lower(txid, lower_tx, sizeof(lower_tx));

            int confs = (tip > 0 && height > 0) ? (tip - height + 1) : 0;

            char amt[32];
            zcl_format_zcl(amt, sizeof(amt), wallet_output);

            APPEND(off, r, max,
                "<a href='/wallet/tx/%s' style='text-decoration:none;"
                "color:inherit;display:block'>"
                "<div class='tx-row'>"
                "<div>"
                "<span class='tx-amount %s'>%s%s</span>"
                "<span style='color:#888;font-size:12px;"
                "margin-left:6px'>ZCL</span></div>"
                "<div class='tx-meta'>"
                "<span class='tx-time'>%s</span>"
                "<span class='tx-conf'>%d conf%s</span>"
                "</div></div></a>",
                lower_tx,
                is_recv ? "recv" : "send",
                is_recv ? "+" : "-",
                amt, esc_rel,
                confs, confs == 1 ? "" : "s");
            tx_shown++;
        }
        sqlite3_finalize(s);
        if (tx_shown == 0) {
            if (total_balance > 0)
                APPEND(off, r, max,
                    "<div class='empty-state'>"
                    "Transaction history syncing..."
                    "</div>");
            else
                APPEND(off, r, max,
                    "<div class='empty-state'>"
                    "No transactions yet"
                    "</div>");
        }
    }

    /* Dashboard live-update JS — merged into footer poll (no duplicate fetch) */
    APPEND(off, r, max,
        "<script>"
        "function fmt(z){var v=z/1e8;if(z===0)return'0.00';"
        "if(z%%1000000===0)return v.toFixed(2);"
        "if(z%%10000===0)return v.toFixed(4);return v.toFixed(8);}"
        "window._dashUpdate=function(d){"
        "var b=document.getElementById('bal');"
        "if(b){var n=fmt(d.balance+d.shielded)+' ZCL';"
        "if(b.textContent!==n){b.textContent=n;}}"
        "var s=document.getElementById('sync');"
        "if(s){var st=d.sync==='at_tip'?'Synced':"
        "d.sync==='idle'?'Ready':"
        "d.sync.indexOf('download')>=0?'Syncing blocks...':"
        "d.sync.indexOf('header')>=0?'Syncing headers...':"
        "d.sync.indexOf('connect')>=0?'Connecting...':"
        "d.sync.indexOf('scan')>=0?'Scanning...':"
        "d.sync;"
        "s.textContent=st;"
        "s.className='pill sync-badge '+"
        "(d.sync==='at_tip'?'pill-synced':"
        "d.sync==='idle'?'pill-ready':'pill-syncing');}"
        "var sn=document.querySelector('.sync-note');"
        "if(sn){if(d.sync==='at_tip')sn.style.display='none';"
        "else sn.style.display='';}"
        "var bd=document.getElementById('breakdown');"
        "if(bd){var t=fmt(d.balance)+' transparent';"
        "if(d.shielded>0)t+=' + '+fmt(d.shielded)+' shielded';"
        "bd.textContent=t;}};"
        "</script>");

    /* Pre-fill status bar so it's not blank on load */
    APPEND(off, r, max,
        "<script>document.addEventListener('DOMContentLoaded',function(){"
        "var h=document.getElementById('sb-h');"
        "if(h)h.textContent='Block %d';});</script>", tip);

    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Send (/wallet/send) ────────────────────────────────────── */

static size_t serve_send(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();

    int64_t balance = 0;
    if (db) {
        balance = query_ground_truth_balance(db, NULL);
        sqlite3_close(db);
    }

    size_t off = emit_header(r, max, "Send — ZClassic23", "/wallet/send");

    char bal_fmt[32];
    zcl_format_zcl(bal_fmt, sizeof(bal_fmt), balance);

    {
        int64_t shielded_bal = query_shielded_balance(db, NULL);
        APPEND(off, r, max,
            "<div style='text-align:center;padding:16px 0'>"
            "<span class='balance-sub'>Transparent: "
            "<span style='color:#34d399;font-weight:600'>%s ZCL</span>"
            "</span>",
            bal_fmt);
        if (shielded_bal > 0) {
            char z_fmt[32];
            zcl_format_zcl(z_fmt, sizeof(z_fmt), shielded_bal);
            APPEND(off, r, max,
                "<div style='color:#a78bfa;font-size:12px;margin-top:4px'>"
                "+ %s ZCL shielded (send to z-address to use)</div>",
                z_fmt);
        }
        APPEND(off, r, max, "</div>");
    }

    APPEND(off, r, max,
        "<form id='send-form' method='POST' action='zcl://node/wallet/send/review' "
        "onsubmit='return validateSend()' autocomplete='off'>"
        "<div class='form-group'>"
        "<label class='form-label' for='addr'>To</label>"
        "<input class='form-input' type='text' id='addr' name='address' "
        "placeholder='Recipient address (t1... or zs1...)' required>"
        "<div id='addr-err' class='form-error'></div></div>"
        "<div class='form-group'>"
        "<label class='form-label' for='amt'>Amount</label>"
        "<div style='display:flex;gap:8px;align-items:center'>"
        "<input class='form-input' type='text' id='amt' name='amount' "
        "inputmode='decimal' style='flex:1' placeholder='0.00' required "
        "oninput='updateRemaining()'>"
        "<button type='button' class='send-max' "
        "onclick='document.getElementById(\"amt\").value="
        "(BAL-%.4f).toFixed(8);updateRemaining()'>Max</button></div>"
        "<div id='remaining' class='remaining' "
        "style='color:#888;font-size:12px;margin:4px 0'></div>"
        "<div id='amt-err' class='form-error'></div>"
        "<div style='color:#888;font-size:12px;margin:4px 0'>"
        "Network fee: <span style='color:#f59e0b'>%.8f ZCL</span>"
        "</div></div>"
        "<button type='submit' class='btn-primary' style='margin-top:16px' "
        "id='review-btn'>Review Send</button>"
        "</form>"
        "<script>"
        "var BAL=%.8f;"
        "function updateRemaining(){"
        "var a=parseFloat(document.getElementById('amt').value)||0;"
        "var r=document.getElementById('remaining');"
        "if(a>0&&a<=BAL){r.textContent='Remaining: '+(BAL-a-%.4f).toFixed(8)+' ZCL';"
        "r.style.color='#666';}"
        "else if(a>BAL){r.textContent='Insufficient funds';"
        "r.style.color='#f87171';}"
        "else{r.textContent='';}}"
        "function validateSend(){"
        "var a=document.getElementById('addr').value.trim();"
        "var m=document.getElementById('amt').value.trim();"
        "document.getElementById('addr-err').textContent='';"
        "document.getElementById('amt-err').textContent='';"
        "var minLen=a&&a.startsWith('zs1')?70:26;"
        "if(!a||a.length<minLen){"
        "document.getElementById('addr-err').textContent="
        "'Enter a valid address';return false;}"
        "if(!(/^(t[13]|zs1)/.test(a))){"
        "document.getElementById('addr-err').textContent="
        "'Must start with t1, t3, or zs1';return false;}"
        "if(!(/^[a-zA-Z0-9]+$/.test(a))){"
        "document.getElementById('addr-err').textContent="
        "'Invalid characters in address';return false;}"
        "var amt=parseFloat(m);"
        "if(isNaN(amt)||amt<=0){"
        "document.getElementById('amt-err').textContent="
        "'Enter an amount';return false;}"
        "if(amt+%.4f>BAL){"
        "document.getElementById('amt-err').textContent="
        "'Insufficient funds: need '+(amt+%.4f-BAL).toFixed(8)+' more ZCL';"
        "return false;}"
        "return true;}"
        /* Real-time address validation on blur */
        "document.getElementById('addr').addEventListener('blur',function(){"
        "var a=this.value.trim(),e=document.getElementById('addr-err');"
        "e.textContent='';"
        "if(!a)return;"
        "var ml=a.startsWith('zs1')?70:26;"
        "if(a.length<ml){e.textContent='Address too short';return;}"
        "if(!(/^(t[13]|zs1)/.test(a))){e.textContent="
        "'Must start with t1, t3, or zs1';return;}"
        "if(!(/^[a-zA-Z0-9]+$/.test(a))){e.textContent="
        "'Invalid characters';return;}"
        "this.style.borderColor='#34d399';"
        "setTimeout(function(){document.getElementById('addr')"
        ".style.borderColor='';},1500);});"
        /* Loading overlay on submit */
        "document.getElementById('review-btn').addEventListener('click',"
        "function(e){if(!validateSend()){e.preventDefault();return;}"
        "this.disabled=true;this.textContent='Reviewing...';"
        "});"
        "</script>",
        FEE_ZCL, FEE_ZCL,
        (double)balance / (double)ZATOSHI_PER_ZCL, FEE_ZCL, FEE_ZCL, FEE_ZCL);

    emit_footer(r, max, &off);
    return off;
}

/* ── Receive (/wallet/receive) ──────────────────────────────── */

static size_t serve_receive(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    size_t off = emit_header(r, max, "Receive — ZClassic23", "/wallet/receive");

    /* Address type tabs */
    APPEND(off, r, max,
        "<div class='tab-toggle'>"
        "<a id='tab-t' class='active' onclick='showTab(\"t\")'>Transparent</a>"
        "<a id='tab-z' onclick='showTab(\"z\")'>Shielded</a>"
        "</div>");

    /* Transparent address section */
    APPEND(off, r, max,
        "<div id='pane-t' style='text-align:center;padding:16px 0'>"
        "<div class='balance-sub' style='margin-bottom:12px'>"
        "Share this address to receive ZCL</div>");
    off = emit_qr_svg(r, max, off, PRIMARY_ADDR, 5);

    /* Address with 4-char chunking for visual verification */
    {
        const char *a = PRIMARY_ADDR;
        size_t alen = strlen(a);
        APPEND(off, r, max,
            "<div class='addr-display addr-chunked' "
            "style='margin-top:16px' id='t-addr'>");
        /* First 4 chars highlighted */
        APPEND(off, r, max, "<span class='hi'>%.4s</span>", a);
        for (size_t i = 4; i < alen; i += 4) {
            size_t left = alen - i;
            if (left > 4) left = 4;
            APPEND(off, r, max, "<span class='sep'> </span>");
            if (i + left >= alen) /* Last chunk highlighted */
                APPEND(off, r, max, "<span class='hi'>%.*s</span>",
                    (int)left, a + i);
            else
                APPEND(off, r, max, "%.*s", (int)left, a + i);
        }
        APPEND(off, r, max, "</div>");
    }
    APPEND(off, r, max,
        "<div id='copy-msg' style='color:#888;font-size:12px;"
        "margin-top:4px;height:16px'>Tap address to copy</div>"
        "<div style='color:#888;font-size:11px;margin-top:2px'>"
        "<span class='pill pill-t'>Transparent</span> "
        "Publicly visible on chain</div>"
        "</div>");

    /* Shielded address pane (hidden by default) */
    APPEND(off, r, max,
        "<div id='pane-z' style='display:none;text-align:center;padding:16px 0'>"
        "<div class='balance-sub' style='margin-bottom:12px'>"
        "Share a shielded address for private transactions</div>");

    /* Shielded addresses — try SQLite first, fall back to RPC */
    int z_shown = 0;
    if (db) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT address FROM wallet_sapling_keys "
                "WHERE address IS NOT NULL AND length(address) > 0 "
                "ORDER BY rowid",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW && off + 512 < max) {
                const char *raw = (const char *)sqlite3_column_text(s, 0);
                if (!raw || !raw[0]) continue;
                char escaped[1024];
                html_escape(escaped, sizeof(escaped), raw);
                if (z_shown == 0) {
                    APPEND(off, r, max,
                        "<div class='section-header' style='margin-top:24px'>"
                        "<span style='color:#a78bfa'>Your Shielded Address</span></div>");
                    APPEND(off, r, max,
                        "<div class='addr-display-sm' style='font-size:11px'>"
                        "%s</div>", escaped);
                } else if (z_shown == 1) {
                    APPEND(off, r, max,
                        "<details style='margin-top:8px'>"
                        "<summary style='color:#888;font-size:12px;cursor:pointer'>"
                        "Show all addresses</summary>");
                    APPEND(off, r, max,
                        "<div class='addr-display-sm' style='font-size:11px'>"
                        "%s</div>", escaped);
                } else {
                    APPEND(off, r, max,
                        "<div class='addr-display-sm' style='font-size:11px'>"
                        "%s</div>", escaped);
                }
                z_shown++;
            }
            sqlite3_finalize(s);
        }
        if (z_shown > 1)
            APPEND(off, r, max, "</details>");
        sqlite3_close(db);
    }

    if (z_shown == 0) {
        APPEND(off, r, max,
            "<div class='empty-state'>"
            "<div style='color:#a78bfa;font-size:13px'>"
            "No shielded addresses yet</div>"
            "<div style='color:#888;font-size:12px;margin-top:4px'>"
            "Generate one: <code>zcl-rpc z_getnewaddress</code></div>"
            "</div>");
    }
    APPEND(off, r, max,
        "<div id='copy-msg-z' style='color:#888;font-size:12px;"
        "margin-top:4px;height:16px'>Tap address to copy</div>");
    APPEND(off, r, max, "</div>"); /* close pane-z */

    /* Tab switching JS */
    APPEND(off, r, max,
        "<script>"
        "function showTab(t){"
        "document.getElementById('pane-t').style.display=t==='t'?'':'none';"
        "document.getElementById('pane-z').style.display=t==='z'?'':'none';"
        "document.getElementById('tab-t').className=t==='t'?'active':'';"
        "document.getElementById('tab-z').className=t==='z'?'active':'';}"
        "</script>");

    /* Click-to-copy with "Copied!" feedback */
    APPEND(off, r, max,
        "<script>"
        "document.querySelectorAll('.addr-display,.addr-display-sm,.addr-chunked')"
        ".forEach(function(el){"
        "el.style.cursor='pointer';"
        "el.addEventListener('click',function(){"
        "var txt=this.textContent.replace(/\\s+/g,'').trim();"
        "navigator.clipboard.writeText(txt).then(function(){"
        "el.style.borderColor='#34d399';"
        "var msg=document.getElementById('copy-msg')||"
        "document.getElementById('copy-msg-z');"
        "var pz=document.getElementById('pane-z');"
        "if(pz&&pz.style.display!=='none')"
        "msg=document.getElementById('copy-msg-z');"
        "if(msg)msg.textContent='Copied!';"
        "setTimeout(function(){el.style.borderColor='';"
        "if(msg)msg.textContent='Tap address to copy';},1500);}"
        ").catch(function(){"
        "var ta=document.createElement('textarea');"
        "ta.value=txt;ta.style.position='fixed';ta.style.left='-9999px';"
        "document.body.appendChild(ta);ta.select();"
        "document.execCommand('copy');document.body.removeChild(ta);"
        "var msg=document.getElementById('copy-msg')||"
        "document.getElementById('copy-msg-z');"
        "var pz=document.getElementById('pane-z');"
        "if(pz&&pz.style.display!=='none')"
        "msg=document.getElementById('copy-msg-z');"
        "if(msg)msg.textContent='Copied!';});"
        "});});"
        "</script>");

    emit_footer(r, max, &off);
    return off;
}

/* ── History (/wallet/history) ──────────────────────────────── */

static size_t serve_history(uint8_t *r, size_t max, int page,
                            const char *filter, const char *search) {
    sqlite3 *db = open_db();
    if (!db) {
        size_t off = emit_header(r, max, "History — ZClassic23", "/wallet/history");
        APPEND(off, r, max,
            "<div class='empty-state' style='padding:48px 0'>"
            "<div style='font-size:40px;margin-bottom:12px'>&#x23F3;</div>"
            "<div style='color:#e2e2e2;font-size:18px;font-weight:600'>"
            "Wallet Loading</div>"
            "<div style='margin-top:8px'>"
            "The database is not yet available.</div></div>");
        emit_footer(r, max, &off);
        return off;
    }

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");
    int per_page = 50;

    size_t off = emit_header(r, max, "History — ZClassic23", "/wallet/history");

    /* Filter: all, sent, received — always use WHERE 1=1 base */
    const char *filter_clause = "";
    if (filter && strcmp(filter, "sent") == 0)
        filter_clause = " AND wt.from_me = 1";
    else if (filter && strcmp(filter, "recv") == 0)
        filter_clause = " AND wt.from_me = 0";

    /* Search by txid prefix */
    char search_clause[256] = "";
    char safe_search[65] = "";
    if (search && search[0]) {
        size_t si = 0;
        for (size_t i = 0; search[i] && si < 64; i++) {
            char c = search[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))
                safe_search[si++] = c;
        }
        safe_search[si] = '\0';
        if (safe_search[0])
            snprintf(search_clause, sizeof(search_clause),
                " AND hex(wt.txid) LIKE '%%%s%%'", safe_search);
    }

    /* Exclude ghost entries: must have either a real block or wallet_utxos */
    const char *ghost_filter =
        " AND (wt.block_height > 0 OR EXISTS "
        "(SELECT 1 FROM wallet_utxos wu WHERE wu.txid = wt.txid))";
    char count_sql[1024];
    snprintf(count_sql, sizeof(count_sql),
        "SELECT count(*) FROM wallet_transactions wt"
        " WHERE 1=1%s%s%s",
        filter_clause, ghost_filter, search_clause);
    int tx_count = query_int(db, count_sql);

    int total_pages = (tx_count + per_page - 1) / per_page;
    if (page >= total_pages && total_pages > 0) page = total_pages - 1;

    /* Filter tabs with counts */
    const char *f = filter ? filter : "all";
    char all_sql[512], sent_sql[512], recv_sql[512];
    snprintf(all_sql, sizeof(all_sql),
        "SELECT count(*) FROM wallet_transactions wt WHERE 1=1%s", ghost_filter);
    snprintf(sent_sql, sizeof(sent_sql),
        "SELECT count(*) FROM wallet_transactions wt WHERE wt.from_me=1%s", ghost_filter);
    snprintf(recv_sql, sizeof(recv_sql),
        "SELECT count(*) FROM wallet_transactions wt WHERE wt.from_me=0%s", ghost_filter);
    int c_all = query_int(db, all_sql);
    int c_sent = query_int(db, sent_sql);
    int c_recv = query_int(db, recv_sql);
    APPEND(off, r, max,
        "<h2>Transaction History</h2>"
        "<div class='filter-tabs'>"
        "<a href='/wallet/history?filter=all' class='%s'>All (%d)</a>"
        "<a href='/wallet/history?filter=sent' class='%s'>Sent (%d)</a>"
        "<a href='/wallet/history?filter=recv' class='%s'>Received (%d)</a>"
        "</div>",
        strcmp(f, "all") == 0 || !filter ? "active" : "", c_all,
        strcmp(f, "sent") == 0 ? "active" : "", c_sent,
        strcmp(f, "recv") == 0 ? "active" : "", c_recv);

    /* Search bar */
    APPEND(off, r, max,
        "<input class='search-input' type='text' id='tx-search' "
        "placeholder='Search by txid...' value='%s' "
        "aria-label='Search transactions'"
        "onkeydown='if(event.key===\"Enter\"){"
        "var v=this.value.trim();"
        "window.location=\"/wallet/history?filter=%s\"+"
        "(v?\"&amp;q=\"+v:\"\");}'>"
        "<div class='sub'>%d transaction%s (page %d of %d)</div>",
        safe_search, f,
        tx_count, tx_count == 1 ? "" : "s",
        page + 1, total_pages > 0 ? total_pages : 1);

    /* Timeline view (tx-cards).
     * Use from_me to determine send vs receive.
     * Compute net value from wallet UTXOs for this txid. */
    sqlite3_stmt *s = NULL;
    char history_sql[1024];
    snprintf(history_sql, sizeof(history_sql),
        "SELECT hex(wt.txid), wt.block_height, COALESCE(b.time,0), "
        "wt.from_me, wt.fee, "
        /* Try wallet_utxos first, fall back to tx_outputs for wallet addrs */
        "COALESCE("
        "  (SELECT SUM(wu.value) FROM wallet_utxos wu WHERE wu.txid = wt.txid),"
        "  (SELECT SUM(o.value) FROM tx_outputs o "
        "    WHERE o.txid = wt.txid AND o.address_hash IN "
        "    (SELECT pubkey_hash FROM wallet_keys)),"
        "  0) "
        "FROM wallet_transactions wt "
        "LEFT JOIN blocks b ON wt.block_height = b.height "
        "WHERE 1=1%s%s%s"
        "ORDER BY wt.block_height DESC LIMIT ? OFFSET ?",
        filter_clause, ghost_filter, search_clause);
    if (sqlite3_prepare_v2(db, history_sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, per_page);
        sqlite3_bind_int(s, 2, page * per_page);
        while (sqlite3_step(s) == SQLITE_ROW && off + 600 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int from_me = sqlite3_column_int(s, 3);
            (void)sqlite3_column_int64(s, 4); /* fee — not displayed */
            int64_t wallet_output = sqlite3_column_int64(s, 5);
            if (!txid) continue;
            /* Skip entries with no useful data */
            if (wallet_output == 0 && h == 0) continue;
            /* Skip zero-amount sends (shielded ops with no transparent delta) */
            if (wallet_output == 0 && from_me) continue;

            bool is_recv = (from_me == 0);
            int64_t display_val = wallet_output;

            char short_tx[18], lower_tx[65], rel_time[48], ts[32];
            txid_short(txid, short_tx, sizeof(short_tx));
            txid_lower(txid, lower_tx, sizeof(lower_tx));
            format_relative_time(btime, rel_time, sizeof(rel_time));
            format_time(btime, ts, sizeof(ts));

            char esc_short[64], esc_lower[256], esc_rel[96], esc_ts[64];
            html_escape(esc_short, sizeof(esc_short), short_tx);
            html_escape(esc_lower, sizeof(esc_lower), lower_tx);
            html_escape(esc_rel, sizeof(esc_rel), rel_time);
            html_escape(esc_ts, sizeof(esc_ts), ts);

            int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
            if (confs < 0) confs = 0;

            /* Format height with commas */
            char h_fmt[20];
            {
                char tmp[20];
                int tl = snprintf(tmp, sizeof(tmp), "%d", h);
                int ci = 0, ti = 0;
                int digits_left = tl;
                for (int di = 0; di < tl && ci < (int)sizeof(h_fmt)-1; di++) {
                    h_fmt[ci++] = tmp[ti++];
                    digits_left--;
                    if (digits_left > 0 && digits_left % 3 == 0)
                        h_fmt[ci++] = ',';
                }
                h_fmt[ci] = '\0';
            }

            APPEND(off, r, max,
                "<a href='/wallet/tx/%s' style='text-decoration:none;color:inherit'>"
                "<div class='tx-card' style='border-left-color:%s'>"
                "<div style='display:flex;justify-content:space-between;"
                "align-items:baseline'>"
                "<span class='tx-amount %s'>%s%.8f ZCL</span>"
                "<span class='pill %s' style='font-size:9px'>%s</span></div>"
                "<div class='tx-meta'>"
                "<span class='tx-time' title='%s'>%s</span>"
                "<span class='tx-conf'>Block %s &middot; %d conf%s</span>"
                "</div></div></a>",
                esc_lower,
                is_recv ? "#34d399" : "#f87171",
                is_recv ? "recv" : "send",
                is_recv ? "+" : "-",
                (double)display_val / 1e8,
                is_recv ? "pill-t" : "pill-send",
                is_recv ? "Received" : "Sent",
                esc_ts,
                esc_rel,
                h_fmt, confs, confs == 1 ? "" : "s");
        }
        sqlite3_finalize(s);
    }

    /* Pagination (preserve filter + search) */
    if (total_pages > 1) {
        APPEND(off, r, max, "<div class='page-controls'>");
        if (page > 0)
            APPEND(off, r, max,
                "<a href='/wallet/history?page=%d&amp;filter=%s%s%s'>"
                "&larr; Newer</a>", page - 1, f,
                safe_search[0] ? "&amp;q=" : "",
                safe_search[0] ? safe_search : "");
        if (page < total_pages - 1)
            APPEND(off, r, max,
                "<a href='/wallet/history?page=%d&amp;filter=%s%s%s'>"
                "Older &rarr;</a>", page + 1, f,
                safe_search[0] ? "&amp;q=" : "",
                safe_search[0] ? safe_search : "");
        APPEND(off, r, max, "</div>");
    }

    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Coins (/wallet/coins) — Full UTXO audit view ──────────── */

static size_t serve_coins(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) {
        size_t off = emit_header(r, max, "Coins — ZClassic23", "/wallet/coins");
        APPEND(off, r, max,
            "<div class='empty-state' style='padding:48px 0'>"
            "<div style='font-size:40px;margin-bottom:12px'>&#x23F3;</div>"
            "<div style='color:#e2e2e2;font-size:18px;font-weight:600'>"
            "Wallet Loading</div>"
            "<div style='margin-top:8px'>"
            "The database is not yet available.</div></div>");
        emit_footer(r, max, &off);
        return off;
    }

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");

    size_t off = emit_header(r, max, "Coins — ZClassic23", "/wallet/coins");
    APPEND(off, r, max, "<h2>Coin Audit</h2>"
        "<div style='color:#6b7280;font-size:13px;margin-bottom:16px'>"
        "Every coin, verified against the chain UTXO set.</div>");

    /* Ground-truth transparent UTXOs (P2PKH + P2SH change) */
    APPEND(off, r, max,
        "<h3>Transparent UTXOs (Chain-Verified)</h3>"
        "<div class='overflow-x'>"
        "<table><tr><th>Outpoint</th><th>Type</th>"
        "<th>Amount</th><th>Height</th><th>Conf</th></tr>");

    int64_t t_total = 0;
    int t_count = 0;
    sqlite3_stmt *s = NULL;
    const char *coins_sql =
        "SELECT hex(wu.txid), wu.vout, wu.value, wu.height, "
        "  CASE WHEN wu.is_coinbase THEN 'Coinbase' ELSE 'Standard' END "
        "FROM wallet_utxos wu "
        "WHERE wu.spent_txid IS NULL "
        "ORDER BY wu.value DESC";
    if (sqlite3_prepare_v2(db, coins_sql, -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 500 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int vout = sqlite3_column_int(s, 1);
            int64_t val = sqlite3_column_int64(s, 2);
            int h = sqlite3_column_int(s, 3);
            const char *stype = (const char *)sqlite3_column_text(s, 4);
            if (!txid) continue;

            char short_tx[18], lower_tx[65];
            txid_short(txid, short_tx, sizeof(short_tx));
            txid_lower(txid, lower_tx, sizeof(lower_tx));

            int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
            if (confs < 0) confs = 0;

            APPEND(off, r, max,
                "<tr>"
                "<td><a href='/explorer/tx/%s' class='hash'>"
                "%s:%d</a></td>"
                "<td><span class='pill pill-%s' style='font-size:10px'>"
                "%s</span></td>"
                "<td class='zcl'>%.8f</td>",
                lower_tx, short_tx, vout,
                stype && stype[0] == 'C' ? "pending" : "t",
                stype ? stype : "Standard",
                (double)val / 1e8);
            if (h > 0)
                APPEND(off, r, max, "<td>%d</td><td>%d</td>", h, confs);
            else
                APPEND(off, r, max,
                    "<td><span class='pill pill-pending'>Pending</span></td>"
                    "<td><span class='pill pill-pending'>Pending</span></td>");
            APPEND(off, r, max, "</tr>");
            t_total += val;
            t_count++;
        }
        sqlite3_finalize(s);
    }

    APPEND(off, r, max,
        "<tr class='total-row'>"
        "<td colspan='2'>Total (%d UTXO%s)</td>"
        "<td class='zcl'>%.8f</td>"
        "<td></td><td></td></tr></table></div>",
        t_count, t_count == 1 ? "" : "s",
        (double)t_total / 1e8);

    /* Shielded notes: nullifier-verified unspent */
    int z_notes = 0;
    int64_t z_total = query_shielded_balance(db, &z_notes);

    APPEND(off, r, max,
        "<h3>Shielded Notes</h3>");

    if (z_notes > 0) {
        APPEND(off, r, max,
            "<div class='overflow-x'>"
            "<table><tr><th>Note ID</th>"
            "<th>Amount</th><th>Height</th></tr>");
        s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT hex(n.cm), n.value, n.height "
                "FROM wallet_sapling_notes n"
                " WHERE NOT EXISTS ("
                "   SELECT 1 FROM sapling_spends ss"
                "   WHERE ss.nullifier = n.nullifier)"
                " ORDER BY n.value DESC",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW && off + 400 < max) {
                const char *cm = (const char *)sqlite3_column_text(s, 0);
                int64_t val = sqlite3_column_int64(s, 1);
                int h = sqlite3_column_int(s, 2);
                if (!cm) continue;
                char short_cm[18];
                txid_short(cm, short_cm, sizeof(short_cm));
                APPEND(off, r, max,
                    "<tr><td class='hash'>%s</td>"
                    "<td class='zcl'>%.8f</td>"
                    "<td>%d</td></tr>",
                    short_cm, (double)val / 1e8, h);
            }
            sqlite3_finalize(s);
        }
        APPEND(off, r, max,
            "<tr class='total-row'>"
            "<td>Total (%d note%s)</td>"
            "<td class='zcl'>%.8f</td>"
            "<td></td></tr></table></div>",
            z_notes, z_notes == 1 ? "" : "s",
            (double)z_total / 1e8);
    } else {
        /* No notes found — explain why */
        int sapling_keys = query_int(db,
            "SELECT count(*) FROM wallet_sapling_keys");
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#f59e0b'>"
            "<div class='label' style='color:#f59e0b'>"
            "No shielded notes found</div>"
            "<div style='color:#888;font-size:13px'>"
            "%d Sapling keys in wallet. "
            "Run <code style='color:#f59e0b'>rescanwallet</code> "
            "to scan the chain for notes belonging to these keys."
            "</div></div>", sapling_keys);
    }

    /* Grand total */
    int64_t grand = t_total + z_total;
    APPEND(off, r, max,
        "<div class='stats' style='margin-top:20px'>"
        "<div class='stat'>"
        "<div class='n'>%.8f</div>"
        "<div class='l'>Transparent</div></div>"
        "<div class='stat' style='border-color:#a78bfa'>"
        "<div class='n' style='color:#a78bfa'>%.8f</div>"
        "<div class='l'>Shielded</div></div>"
        "<div class='stat' style='border-color:#f59e0b'>"
        "<div class='n' style='color:#f59e0b'>%.8f</div>"
        "<div class='l'>Total</div></div>"
        "</div>",
        (double)t_total / 1e8,
        (double)z_total / 1e8,
        (double)grand / 1e8);

    /* Data source comparison */
    int64_t speed_bal = query_speed_balance(db);
    int speed_utxos = query_int(db,
        "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL");
    APPEND(off, r, max,
        "<h3>Data Source Comparison</h3>"
        "<div class='overflow-x'>"
        "<table><tr><th>Source</th><th>Balance</th>"
        "<th>UTXOs</th><th>Status</th></tr>"
        "<tr><td>Chain UTXO set (chain-verified)</td>"
        "<td class='zcl'>%.8f</td><td>%d</td>"
        "<td><span class='pill pill-t'>verified</span></td></tr>"
        "<tr><td>Cached balance</td>"
        "<td class='zcl'>%.8f</td><td>%d</td>"
        "<td>%s</td></tr>"
        "</table></div>",
        (double)t_total / 1e8, t_count,
        (double)speed_bal / 1e8, speed_utxos,
        (speed_bal == t_total)
            ? "<span class='pill pill-t'>match</span>"
            : "<span class='pill pill-send'>stale</span>");

    /* Chain supply context */
    int64_t chain_supply = query_int64(db,
        "SELECT COALESCE(SUM(value),0) FROM utxos");
    int chain_utxos = query_int(db, "SELECT count(*) FROM utxos");
    APPEND(off, r, max,
        "<h3>Chain Supply</h3>"
        "<div class='stats'>"
        "<div class='stat'>"
        "<div class='n'>%.2f</div>"
        "<div class='l'>UTXO Supply (ZCL)</div></div>"
        "<div class='stat'>"
        "<div class='n'>%d</div>"
        "<div class='l'>Total UTXOs</div></div>"
        "</div>",
        (double)chain_supply / 1e8, chain_utxos);

    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Shield (/wallet/shield?amount=X) ──────────────────────── */
/* One-click shielding confirmation page.
 * The wallet auto-generates a z-address and builds the transaction.
 * User just confirms the amount. */

static size_t serve_shield(uint8_t *r, size_t max, const char *query) {
    /* Parse amount from query string */
    double amount = 0;
    if (query) {
        const char *amt = strstr(query, "amount=");
        if (amt) amount = strtod(amt + 7, NULL);
        /* ?all=1 — compute from balance (avoids leaking amount in URL) */
        if (strstr(query, "all=1")) {
            sqlite3 *sdb = open_db();
            if (sdb) {
                int64_t bal = query_ground_truth_balance(sdb, NULL);
                sqlite3_close(sdb);
                amount = (double)bal / (double)ZATOSHI_PER_ZCL - FEE_ZCL;
                if (amount < 0) amount = 0;
            }
        }
    }

    if (amount <= 0) {
        /* No amount specified — show amount input form */
        size_t off = emit_header(r, max, "Shield — ZClassic23", "/wallet/shield");
        int64_t avail = 0;
        {
            sqlite3 *sdb = open_db();
            if (sdb) {
                avail = query_ground_truth_balance(sdb, NULL);
                sqlite3_close(sdb);
            }
        }
        char avail_str[32];
        zcl_format_zcl(avail_str, sizeof(avail_str), avail);
        APPEND(off, r, max,
            "<div style='text-align:center;padding:16px 0'>"
            "<div style='color:#a78bfa;font-size:20px;font-weight:700;"
            "margin-bottom:4px'>Shield Funds</div>"
            "<div class='balance-sub'>Move ZCL to a shielded address "
            "for full privacy</div></div>"
            "<form method='GET' action='/wallet/shield'>"
            "<div class='form-group'>"
            "<label class='form-label' for='shield-amt'>Amount to Shield</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input class='form-input' type='text' id='shield-amt' "
            "inputmode='decimal' name='amount' placeholder='0.00' required>"
            "<button type='button' class='send-max' "
            "onclick='document.getElementById(\"shield-amt\").value="
            "\"%.8f\"'>Max</button>"
            "</div>"
            "<div style='color:#888;font-size:14px;margin-top:6px'>"
            "Available: <span style='color:#34d399'>%s ZCL</span>"
            "</div></div>"
            "<button type='submit' class='btn-primary' "
            "style='background:#a78bfa;color:#fff;margin-top:8px'>"
            "Review Shield</button>"
            "</form>"
            "<div style='text-align:center;margin-top:16px'>"
            "<a href='/wallet' style='color:#888'>Cancel</a></div>",
            (double)avail / (double)ZATOSHI_PER_ZCL - FEE_ZCL,
            avail_str);
        emit_footer(r, max, &off);
        return off;
    }

    double fee = FEE_ZCL;
    double total_cost = amount + fee;

    size_t off = emit_header(r, max, "Shield — ZClassic23", "/wallet/shield");

    APPEND(off, r, max,
        "<div class='card' style='border-left-color:#a78bfa;padding:20px;"
        "background:linear-gradient(135deg,#141414,#1a1a2a)'>"
        "<div style='text-align:center'>"
        "<div style='font-size:14px;color:#888;margin-bottom:8px'>"
        "Shielding</div>"
        "<div style='font-size:40px;color:#a78bfa;font-weight:800'>"
        "%.8f ZCL</div>"
        "<div style='color:#888;font-size:13px;margin-top:8px'>"
        "Fee: %.8f ZCL &middot; Total: %.8f ZCL</div>"
        "</div></div>",
        amount, fee, total_cost);

    APPEND(off, r, max,
        "<div class='card'>"
        "<div style='color:#888;font-size:13px;line-height:1.6'>"
        "<div style='margin-bottom:8px'>"
        "<span style='color:#34d399;font-weight:700'>Step 1:</span> "
        "Your transparent ZCL moves to a shielded address (1 confirmation, ~2.5 min).</div>"
        "<div style='margin-bottom:8px'>"
        "<span style='color:#a78bfa;font-weight:700'>Step 2:</span> "
        "Funds are spendable immediately. For maximum privacy, wait ~6 hours "
        "before spending so timing analysis cannot link the transparent source.</div>"
        "<div>"
        "<span style='color:#60a5fa;font-weight:700'>Step 3:</span> "
        "Spend from your shielded balance with no on-chain link to the original address.</div>"
        "</div></div>");

    APPEND(off, r, max,
        "<div style='display:flex;gap:10px;margin:16px 0'>"
        "<a href='/wallet' class='btn-secondary' "
        "style='flex:1;text-align:center;text-decoration:none;"
        "display:flex;align-items:center;justify-content:center'>Cancel</a>"
        "<form method='POST' action='zcl://node/wallet/shield/confirm' "
        "style='flex:2;margin:0'>"
        "<input type='hidden' name='amount' value='%.8f'>"
        "<button type='submit' class='btn-primary' "
        "style='background:#a78bfa;color:#fff'"
        " id='shield-btn'>Confirm Shield</button></form></div>"
        "<div id='shield-loading' class='loading-overlay' style='display:none'>"
        "<div class='spinner'></div>"
        "<p>Shielding funds...</p></div>"
        "<script>"
        "document.getElementById('shield-btn').addEventListener('click',"
        "function(e){e.preventDefault();this.disabled=true;"
        "document.getElementById('shield-loading').style.display='flex';"
        "this.form.submit();});"
        "</script>",
        amount);

    emit_footer(r, max, &off);
    return off;
}

/* ── Shield Confirm (/wallet/shield/confirm POST) ──────────── */
/* Executes the shielding transaction via the node's z_sendmany. */

static size_t serve_shield_confirm(uint8_t *r, size_t max,
                                    const uint8_t *body, size_t body_len) {
    double amount = 0;
    char amount_str[32] = "";
    if (body && body_len > 0)
        parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));
    if (amount_str[0])
        amount = strtod(amount_str, NULL);

    size_t off = emit_header(r, max, "Shielding — ZClassic23", "/wallet/shield");

    if (amount <= 0) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#f87171'>"
            "<div class='label' style='color:#f87171'>Invalid amount</div>"
            "<a href='/wallet'>Back to Wallet</a></div>");
        emit_footer(r, max, &off);
        return off;
    }

    /* Look up a z-address from the wallet to use as destination */
    char z_dest[256] = "";
    {
        sqlite3 *sdb = open_db();
        if (sdb) {
            sqlite3_stmt *zs = NULL;
            if (sqlite3_prepare_v2(sdb,
                    "SELECT address FROM wallet_sapling_keys "
                    "WHERE address IS NOT NULL AND length(address) > 0 "
                    "ORDER BY rowid LIMIT 1",
                    -1, &zs, NULL) == SQLITE_OK && zs) {
                if (sqlite3_step(zs) == SQLITE_ROW) {
                    const char *a = (const char *)sqlite3_column_text(zs, 0);
                    if (a) snprintf(z_dest, sizeof(z_dest), "%s", a);
                }
                sqlite3_finalize(zs);
            }
            sqlite3_close(sdb);
        }
    }

    if (!z_dest[0]) {
        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x274C;</div>"
            "<h2>Could Not Shield</h2>"
            "<p>No shielded address available and the node could not "
            "generate one. Is the node running?</p>"
            "<a href='/wallet' style='color:#34d399'>Back to Wallet</a>"
            "</div>");
        emit_footer(r, max, &off);
        return off;
    }

    /* Get funded t-address for z_sendmany. One fast RPC to zclassicd
     * listunspent — required because our SQLite doesn't track change
     * addresses from zclassicd transactions. This is the only RPC
     * besides z_sendmany itself. */
    char t_addr[128] = "";
    get_funded_taddr(t_addr, sizeof(t_addr));

    if (!t_addr[0]) {
        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x26A0;</div>"
            "<h2>Could Not Shield</h2>"
            "<p>No transparent address found in wallet.</p>"
            "<a href='/wallet' style='color:#34d399'>Back to Wallet</a>"
            "</div>");
        emit_footer(r, max, &off);
        return off;
    }

    /* Call zclassicd z_sendmany via RPC (Groth16 runs there) */
    char z_params[1024];
    snprintf(z_params, sizeof(z_params),
        "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}],1,%.8f]",
        t_addr, z_dest, amount, FEE_ZCL);

    char rpc_buf[4096] = "";
    int rpc_rc = wallet_rpc_call_port("z_sendmany", z_params,
                                       rpc_buf, sizeof(rpc_buf),
                                       ZCLASSICD_PORT, zclassicd_auth());

    bool success = false;
    char opid_str[128] = "";
    char shield_err[256] = "";

    if (rpc_rc > 0) {
        /* Check for opid in result */
        char result_val[256] = "";
        zcl_json_extract_str(rpc_buf, "result", result_val, sizeof(result_val));
        if (strstr(result_val, "opid-")) {
            snprintf(opid_str, sizeof(opid_str), "%s", result_val);
            success = true;
        } else if (strstr(rpc_buf, "opid-")) {
            /* Extract opid from raw response */
            const char *op = strstr(rpc_buf, "opid-");
            size_t i = 0;
            while (op[i] && op[i] != '"' && op[i] != '}' && i < 127) {
                opid_str[i] = op[i]; i++;
            }
            opid_str[i] = '\0';
            success = true;
        } else {
            /* Error from zclassicd */
            zcl_json_extract_str(rpc_buf, "message", shield_err,
                                  sizeof(shield_err));
            if (!shield_err[0])
                snprintf(shield_err, sizeof(shield_err),
                    "zclassicd returned an error");
        }
    } else {
        snprintf(shield_err, sizeof(shield_err),
            "Could not connect to zclassicd (port %d). "
            "Start it with: zclassicd -daemon", ZCLASSICD_PORT);
    }

    if (success) {
        /* Sync wallet tables immediately so balance is correct on return */
        sync_wallet_from_zclassicd();
        g_balance_dirty = 1; /* Also recompute on next pulse */
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#34d399;padding:20px'>"
            "<div style='text-align:center'>"
            "<div style='font-size:40px;margin-bottom:8px'>&#x2705;</div>"
            "<div style='font-size:20px;color:#34d399;font-weight:700'>"
            "Shielding Started</div>"
            "<div style='color:#888;font-size:14px;margin-top:8px'>"
            "%.8f ZCL is being moved to a shielded address.</div>"
            "<div style='color:#888;font-size:12px;margin-top:12px;"
            "font-family:monospace;word-break:break-all'>%s</div>"
            "<div style='color:#555;font-size:13px;margin-top:12px'>"
            "Shielded immediately. Wait ~6 hours to break timing correlation "
            "with the transparent source address.</div>",
            amount, opid_str);

        /* Show updated balance from the sync we just did */
        {
            sqlite3 *sdb = open_db();
            if (sdb) {
                int64_t new_t = query_ground_truth_balance(sdb, NULL);
                int64_t new_z = query_shielded_balance(sdb, NULL);
                int64_t new_total = new_t + new_z;
                APPEND(off, r, max,
                    "<div style='margin-top:16px;padding:12px;background:#0a1f14;"
                    "border-radius:8px'>"
                    "<div style='color:#34d399;font-size:18px;font-weight:700'>"
                    "%.8f ZCL</div>"
                    "<div style='color:#888;font-size:12px;margin-top:4px'>"
                    "%.8f transparent + %.8f shielded</div>"
                    "</div>",
                    (double)new_total / 1e8,
                    (double)new_t / 1e8,
                    (double)new_z / 1e8);
                sqlite3_close(sdb);
            }
        }

        APPEND(off, r, max, "</div></div>");
    } else {
        char safe_err[512];
        html_escape(safe_err, sizeof(safe_err), shield_err);

        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x26A0;</div>"
            "<h2>Could Not Shield</h2>"
            "<p>%s</p>"
            "<a href='/wallet' style='color:#34d399'>Back to Wallet</a>"
            "</div>", safe_err[0] ? safe_err : "Unknown error");
    }

    APPEND(off, r, max,
        "<div style='text-align:center;margin:16px'>"
        "<a href='/wallet' style='color:#60a5fa;font-size:16px'>"
        "Back to Wallet</a></div>");

    emit_footer(r, max, &off);
    return off;
}

/* ── Pulse endpoint (JSON) ──────────────────────────────────── */
/* Balance cache: recompute only when block height changes.
 * The ground-truth query has 3 JOINs — too heavy for 2-second polls. */

static struct {
    int height;
    int64_t balance, shielded, speed_bal;
    int t_utxos, z_notes;
} pulse_cache;

static size_t serve_pulse(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    int height = 0, peers = 0, mempool = 0;
    int64_t balance = 0, shielded = 0, speed_bal = 0;
    int t_utxos = 0, z_notes = 0;

    if (db) {
        height = query_int(db, "SELECT MAX(height) FROM blocks");
        peers = query_int(db,  "SELECT count(*) FROM peers");
        mempool = query_int(db, "SELECT count(*) FROM mempool_entries");

        if (g_balance_dirty) {
            /* Wallet changed — sync from zclassicd then recompute */
            sqlite3_close(db);
            sync_wallet_from_zclassicd();
            db = open_db();
            if (!db) return 0;
        }

        if (height != pulse_cache.height || pulse_cache.height == 0 ||
            g_balance_dirty) {
            g_balance_dirty = 0;
            /* Recompute balances */
            balance = query_ground_truth_balance(db, &t_utxos);
            shielded = query_shielded_balance(db, &z_notes);
            speed_bal = query_speed_balance(db);

            pulse_cache.height = height;
            pulse_cache.balance = balance;
            pulse_cache.shielded = shielded;
            pulse_cache.speed_bal = speed_bal;
            pulse_cache.t_utxos = t_utxos;
            pulse_cache.z_notes = z_notes;
        } else {
            /* Same height — serve cached balances, only refresh peers/mempool */
            balance = pulse_cache.balance;
            shielded = pulse_cache.shielded;
            speed_bal = pulse_cache.speed_bal;
            t_utxos = pulse_cache.t_utxos;
            z_notes = pulse_cache.z_notes;
        }
        sqlite3_close(db);
    }

    const char *sync = sync_state_name(sync_get_state());

    return (size_t)snprintf((char *)r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"height\":%d,\"balance\":%" PRId64 ",\"shielded\":%" PRId64
        ",\"speed_balance\":%" PRId64
        ",\"t_utxos\":%d,\"z_notes\":%d"
        ",\"peers\":%d,\"sync\":\"%s\",\"mempool\":%d}",
        height, balance, shielded, speed_bal,
        t_utxos, z_notes,
        peers, sync, mempool);
}

/* ── Send Review (/wallet/send/review POST) ─────────────────── */
/* Intermediate confirmation step: show details before executing. */

static size_t serve_send_review(uint8_t *r, size_t max,
                                 const uint8_t *body, size_t body_len) {
    size_t off = emit_header(r, max, "Review — ZClassic23", "/wallet/send");

    char address[128] = "", amount_str[32] = "";
    parse_form_field(body, body_len, "address", address, sizeof(address));
    parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));

    /* Validate address: checksum verification (Base58Check or Bech32) */
    bool addr_ok = validate_zcl_address(address);
    const char *addr_err = NULL;
    if (!addr_ok) {
        size_t alen = strlen(address);
        if (alen < 26)
            addr_err = "Address too short.";
        else if (!(address[0] == 't' || (alen >= 3 && address[0] == 'z')))
            addr_err = "ZClassic addresses start with t1, t3, or zs1.";
        else
            addr_err = "Invalid address checksum. Check for typos.";
    }

    double amount = strtod(amount_str, NULL);
    const char *err_reason = !addr_ok
        ? (addr_err ? addr_err : "Invalid address.")
        : "Invalid amount";
    if (!addr_ok || amount <= 0) {
        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x2717;</div>"
            "<h2>Invalid Transaction</h2>"
            "<p>%s</p>"
            "<div style='margin-top:16px;display:flex;gap:16px;justify-content:center'>"
            "<a href='/wallet' style='color:#999'>Back to Wallet</a>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div></div>",
            err_reason);
        emit_footer(r, max, &off);
        return off;
    }

    bool is_shielded = (strncmp(address, "zs1", 3) == 0);
    double fee = FEE_ZCL;
    double total_deducted = amount + fee;

    /* Query balance */
    int64_t balance = 0;
    {
        sqlite3 *db = open_db();
        if (db) {
            balance = query_ground_truth_balance(db, NULL);
            sqlite3_close(db);
        }
    }
    double remaining = (double)balance / (double)ZATOSHI_PER_ZCL - total_deducted;

    char safe_addr[256];
    html_escape(safe_addr, sizeof(safe_addr), address);

    APPEND(off, r, max,
        "<div style='text-align:center;margin-bottom:16px'>"
        "<span class='form-label'>Review Transaction</span></div>"
        "<table class='review-table'>"
        "<tr><td>To</td>"
        "<td style='color:#60a5fa;font-family:\"JetBrains Mono\",monospace;"
        "font-size:12px;word-break:break-all'>%s</td></tr>"
        "<tr><td>Amount</td>"
        "<td style='color:#34d399;font-size:18px;font-weight:700'>"
        "%.8f ZCL</td></tr>"
        "<tr><td>Fee</td>"
        "<td style='color:#999'>%.8f ZCL</td></tr>"
        "<tr><td style='font-weight:700'>Total</td>"
        "<td style='color:#e2e2e2;font-weight:700'>"
        "%.8f ZCL</td></tr>"
        "<tr><td>Remaining</td>"
        "<td style='color:#999'>%.8f ZCL</td></tr>"
        "<tr><td>Privacy</td>"
        "<td><span class='pill %s'>%s</span></td></tr>"
        "<tr><td>Est. Time</td>"
        "<td style='color:#999'>~2.5 min (1 confirmation)</td></tr>"
        "</table>",
        safe_addr, amount, fee, total_deducted, remaining,
        is_shielded ? "pill-private" : "pill-t",
        is_shielded ? "Private (shielded)" : "Public (transparent)");

    /* Cancel / Confirm buttons */
    APPEND(off, r, max,
        "<div style='display:flex;gap:10px;margin:20px 0'>"
        "<a href='/wallet/send' class='btn-secondary' "
        "style='flex:1;text-align:center;text-decoration:none;"
        "display:flex;align-items:center;justify-content:center'>Cancel</a>"
        "<form method='POST' action='zcl://node/wallet/send/confirm' "
        "style='flex:2;margin:0'>"
        "<input type='hidden' name='address' value='%s'>"
        "<input type='hidden' name='amount' value='%.8f'>"
        "<button type='submit' class='btn-primary'"
        " style='background:%s;color:%s'"
        " id='confirm-btn'>"
        "Confirm Send</button></form></div>"
        "<div id='send-loading' class='loading-overlay' style='display:none'>"
        "<div class='spinner'></div>"
        "<p>Sending transaction...</p></div>"
        "<script>"
        "document.getElementById('confirm-btn').addEventListener('click',"
        "function(e){e.preventDefault();this.disabled=true;"
        "document.getElementById('send-loading').style.display='flex';"
        "this.form.submit();});"
        "</script>",
        safe_addr, amount,
        is_shielded ? "#a78bfa" : "#34d399",
        is_shielded ? "#fff" : "#0c0c0c");

    emit_footer(r, max, &off);
    return off;
}

/* ── Send Confirm (/wallet/send/confirm POST) ──────────────── */

static size_t serve_send_confirm(uint8_t *r, size_t max,
                                  const uint8_t *body, size_t body_len) {
    size_t off = emit_header(r, max, "Sending — ZClassic23", "/wallet/send");

    char address[128] = "", amount_str[32] = "";
    parse_form_field(body, body_len, "address", address, sizeof(address));
    parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));

    /* Validate address: checksum verification */
    bool addr_ok = validate_zcl_address(address);

    double amount = strtod(amount_str, NULL);
    if (!addr_ok || amount <= 0) {
        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x2717;</div>"
            "<h2>Invalid Transaction</h2>"
            "<p>%s</p>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div>",
            !addr_ok ? "Invalid address" : "Invalid amount");
        emit_footer(r, max, &off);
        return off;
    }

    /* Execute send */
    bool is_shielded = (strncmp(address, "zs1", 3) == 0);
    char txid_result[128] = "";
    char error_msg[256] = "";
    int64_t amount_sat = (int64_t)(amount * 1e8 + 0.5);
    bool send_ok = false;

    if (is_shielded) {
        /* Shielded send: delegate to zclassicd z_sendmany.
         * Get the funded t-address from zclassicd listunspent
         * (our SQLite may be stale after change-address txs). */
        char t_from[128] = "";
        get_funded_taddr(t_from, sizeof(t_from));
        if (t_from[0]) {
            char zp[1024];
            snprintf(zp, sizeof(zp),
                "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}],1,%.8f]",
                t_from, address, amount, FEE_ZCL);
            char rb[4096] = "";
            if (wallet_rpc_call_port("z_sendmany", zp, rb, sizeof(rb),
                                      ZCLASSICD_PORT, zclassicd_auth()) > 0) {
                char rv[256] = "";
                zcl_json_extract_str(rb, "result", rv, sizeof(rv));
                if (strstr(rv, "opid-") || strstr(rb, "opid-")) {
                    snprintf(txid_result, sizeof(txid_result), "%s",
                        rv[0] ? rv : "submitted");
                    send_ok = true;
                } else {
                    zcl_json_extract_str(rb, "message", error_msg,
                                          sizeof(error_msg));
                    if (!error_msg[0])
                        snprintf(error_msg, sizeof(error_msg),
                            "zclassicd returned an error");
                }
            } else {
                snprintf(error_msg, sizeof(error_msg),
                    "Could not connect to zclassicd (port %d)", ZCLASSICD_PORT);
            }
        } else {
            snprintf(error_msg, sizeof(error_msg),
                "No transparent address found in wallet");
        }
    } else {
        send_ok = wallet_direct_sendtoaddress(address, amount_sat,
            txid_result, sizeof(txid_result),
            error_msg, sizeof(error_msg));
    }

    if (send_ok) {
        /* Sync wallet tables immediately so balance is correct on return */
        sync_wallet_from_zclassicd();
        g_balance_dirty = 1;
        char safe_addr[256], safe_txid[256];
        html_escape(safe_addr, sizeof(safe_addr), address);
        html_escape(safe_txid, sizeof(safe_txid), txid_result);

        bool is_opid = (strncmp(txid_result, "opid-", 5) == 0);
        APPEND(off, r, max,
            "<div class='result-success'>"
            "<div class='icon'>&#x2713;</div>"
            "<h2>%s</h2>"
            "<p>%.8f ZCL to %s</p>",
            is_opid ? "Shielded Send Started" : "Transaction Sent",
            amount, safe_addr);
        if (is_opid)
            APPEND(off, r, max,
                "<div class='hash' style='word-break:break-all;"
                "color:#a78bfa;font-size:12px'>%s</div>"
                "<div style='color:#888;font-size:12px;margin-top:8px'>"
                "Funds will arrive after ~10 confirmations (~25 min)</div>",
                safe_txid);
        else
            APPEND(off, r, max,
                "<a href='/explorer/tx/%s' class='hash' "
                "style='word-break:break-all'>%s</a>",
                safe_txid, safe_txid);
        APPEND(off, r, max,
            "<div style='margin-top:24px'>"
            "<a href='/wallet' style='color:#34d399;font-size:16px'>"
            "Back to Wallet</a></div></div>");
    } else {
        char safe_err[512];
        html_escape(safe_err, sizeof(safe_err), error_msg);
        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x2717;</div>"
            "<h2>Send Failed</h2>"
            "<p>%s</p>"
            "<div style='margin-top:16px;display:flex;gap:16px;justify-content:center'>"
            "<a href='/wallet' style='color:#999'>Back to Wallet</a>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div></div>", safe_err);
    }

    emit_footer(r, max, &off);
    return off;
}

/* ── Transaction Detail (/wallet/tx/:txid) ──────────────────── */

static size_t serve_tx_detail(uint8_t *r, size_t max, const char *txid_hex) {
    sqlite3 *db = open_db();
    if (!db) {
        size_t off = emit_header(r, max, "Transaction — ZClassic23", "/wallet/history");
        APPEND(off, r, max,
            "<div class='empty-state' style='padding:48px 0'>"
            "<div style='font-size:40px;margin-bottom:12px'>&#x23F3;</div>"
            "<div style='color:#e2e2e2;font-size:18px;font-weight:600'>"
            "Wallet Loading</div>"
            "<div style='margin-top:8px'>"
            "The database is not yet available.</div></div>");
        emit_footer(r, max, &off);
        return off;
    }

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");
    size_t off = emit_header(r, max, "Transaction — ZClassic23", "/wallet/history");

    /* Sanitize txid: only hex chars, max 64 */
    char safe_txid[65] = "";
    {
        size_t si = 0;
        for (size_t i = 0; txid_hex && txid_hex[i] && si < 64; i++) {
            char c = txid_hex[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))
                safe_txid[si++] = c;
        }
        safe_txid[si] = '\0';
    }

    if (strlen(safe_txid) < 64) {
        APPEND(off, r, max,
            "<div class='result-error'>"
            "<div class='icon'>&#x2717;</div>"
            "<h2>Invalid Transaction ID</h2>"
            "<a href='/wallet/history' style='color:#34d399'>Back to History</a>"
            "</div>");
        emit_footer(r, max, &off);
        sqlite3_close(db);
        return off;
    }

    /* Convert to uppercase for BLOB comparison */
    char upper_txid[65];
    for (int i = 0; i < 64; i++)
        upper_txid[i] = (safe_txid[i] >= 'a' && safe_txid[i] <= 'f')
            ? (char)(safe_txid[i] - 32) : safe_txid[i];
    upper_txid[64] = '\0';

    /* Lookup wallet transaction */
    int block_height = 0, from_me = 0;
    int64_t fee = 0, btime = 0;

    sqlite3_stmt *s = NULL;
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT wt.block_height, wt.from_me, wt.fee, "
        "COALESCE(b.time, 0) "
        "FROM wallet_transactions wt "
        "LEFT JOIN blocks b ON wt.block_height = b.height "
        "WHERE hex(wt.txid) = '%s'", upper_txid);
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            block_height = sqlite3_column_int(s, 0);
            from_me = sqlite3_column_int(s, 1);
            fee = sqlite3_column_int64(s, 2);
            btime = sqlite3_column_int64(s, 3);
            found = true;
        }
        sqlite3_finalize(s);
    }

    if (!found) {
        APPEND(off, r, max,
            "<div class='result-warning'>"
            "<div class='icon'>&#x1F50D;</div>"
            "<h2>Transaction Not Found</h2>"
            "<p>This transaction is not in your wallet.</p>"
            "<a href='/wallet/history' style='color:#34d399'>Back to History</a>"
            "</div>");
        emit_footer(r, max, &off);
        sqlite3_close(db);
        return off;
    }

    int confs = (tip > 0 && block_height > 0) ? (tip - block_height + 1) : 0;
    if (confs < 0) confs = 0;
    bool is_recv = (from_me == 0);

    /* Confirmation progress (6 = fully confirmed) */
    int conf_pct = confs >= 6 ? 100 : (confs * 100 / 6);

    char rel_time[48], abs_time[32];
    format_relative_time(btime, rel_time, sizeof(rel_time));
    format_time(btime, abs_time, sizeof(abs_time));

    char esc_rel[96], esc_abs[64];
    html_escape(esc_rel, sizeof(esc_rel), rel_time);
    html_escape(esc_abs, sizeof(esc_abs), abs_time);

    /* Header with direction and status */
    APPEND(off, r, max,
        "<div style='text-align:center;padding:16px 0'>"
        "<span class='pill %s' style='font-size:13px;padding:4px 12px'>"
        "%s</span>"
        "<h2 style='margin:12px 0 4px;color:%s'>%s</h2>"
        "<div class='balance-sub'>%s &middot; %s</div>"
        "</div>",
        is_recv ? "pill-t" : "pill-pending",
        is_recv ? "Received" : "Sent",
        is_recv ? "#34d399" : "#f87171",
        is_recv ? "Incoming Transaction" : "Outgoing Transaction",
        esc_rel, esc_abs);

    /* Confirmation meter */
    APPEND(off, r, max,
        "<div style='margin:0 0 16px'>"
        "<div style='display:flex;justify-content:space-between;"
        "font-size:11px;color:#888;margin-bottom:4px'>"
        "<span>%d confirmation%s</span>"
        "<span>%s</span></div>"
        "<div class='conf-meter'>"
        "<div class='fill' style='width:%d%%;background:%s'></div>"
        "</div></div>",
        confs, confs == 1 ? "" : "s",
        confs >= 6 ? "Confirmed" : "Pending",
        conf_pct,
        confs >= 6 ? "#34d399" : confs >= 1 ? "#fbbf24" : "#f87171");

    /* Transaction details grid */
    APPEND(off, r, max,
        "<div class='detail-grid'>"
        "<div class='lbl'>TxID</div>"
        "<div class='val'><a href='/explorer/tx/%s' class='hash' "
        "style='font-size:13px'>%s</a></div>"
        "<div class='lbl'>Block</div>"
        "<div class='val'>%d</div>"
        "<div class='lbl'>Direction</div>"
        "<div class='val'>%s</div>",
        safe_txid, safe_txid,
        block_height,
        is_recv ? "Received" : "Sent");

    if (fee > 0 && !is_recv)
        APPEND(off, r, max,
            "<div class='lbl'>Fee</div>"
            "<div class='val zcl'>%.8f ZCL</div>",
            (double)fee / 1e8);

    APPEND(off, r, max, "</div>");

    /* Outputs belonging to wallet */
    s = NULL;
    snprintf(sql, sizeof(sql),
        "SELECT vout, value, hex(address_hash) "
        "FROM wallet_utxos WHERE hex(txid) = '%s' "
        "ORDER BY vout", upper_txid);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        bool header_shown = false;
        while (sqlite3_step(s) == SQLITE_ROW && off + 300 < max) {
            if (!header_shown) {
                APPEND(off, r, max,
                    "<h3>Wallet Outputs</h3>");
                header_shown = true;
            }
            int vout = sqlite3_column_int(s, 0);
            int64_t val = sqlite3_column_int64(s, 1);
            APPEND(off, r, max,
                "<div class='utxo-row'>"
                "<span class='mono' style='color:#888'>:%d</span>"
                "<span class='zcl'>%.8f ZCL</span>"
                "</div>",
                vout, (double)val / 1e8);
        }
        sqlite3_finalize(s);
    }

    /* Link to full explorer view */
    APPEND(off, r, max,
        "<div style='text-align:center;margin:24px 0'>"
        "<a href='/explorer/tx/%s' style='color:#60a5fa;font-size:13px'>"
        "View full details in Explorer &rarr;</a></div>"
        "<div style='text-align:center'>"
        "<a href='/wallet/history' style='color:#34d399;font-size:14px'>"
        "&larr; Back to History</a></div>",
        safe_txid);

    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Router ─────────────────────────────────────────────────── */

void wallet_view_init(const char *datadir) {
    g_datadir = datadir;
}

void wallet_view_enable_sync(void) {
    g_sync_enabled = true;
}

size_t wallet_view_handle_request(const char *method, const char *path,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *response, size_t response_max)
{
    (void)method;
    if (!path || !response || response_max == 0) return 0;

    /* JSON pulse endpoint — polled every 5s by dashboard JS */
    if (strcmp(path, "/api/wallet/pulse") == 0)
        return serve_pulse(response, response_max);

    if (strcmp(path, "/wallet") == 0 || strcmp(path, "/wallet/") == 0)
        return serve_dashboard(response, response_max);
    if (strcmp(path, "/wallet/send") == 0)
        return serve_send(response, response_max);
    if (strcmp(path, "/wallet/send/review") == 0)
        return serve_send_review(response, response_max, body, body_len);
    if (strcmp(path, "/wallet/send/confirm") == 0)
        return serve_send_confirm(response, response_max, body, body_len);
    if (strncmp(path, "/wallet/shield/confirm", 22) == 0)
        return serve_shield_confirm(response, response_max, body, body_len);
    if (strncmp(path, "/wallet/shield", 14) == 0) {
        const char *q = strchr(path, '?');
        return serve_shield(response, response_max, q);
    }
    if (strcmp(path, "/wallet/receive") == 0)
        return serve_receive(response, response_max);
    if (strncmp(path, "/wallet/history", 15) == 0) {
        int page = 0;
        const char *pq = strstr(path, "page=");
        if (pq) page = atoi(pq + 5);
        if (page < 0) page = 0;
        /* Parse filter param */
        const char *filt = NULL;
        char filt_buf[16] = "";
        const char *fp = strstr(path, "filter=");
        if (fp) {
            fp += 7;
            size_t fi = 0;
            while (fp[fi] && fp[fi] != '&' && fi < 15)
                { filt_buf[fi] = fp[fi]; fi++; }
            filt_buf[fi] = '\0';
            filt = filt_buf;
        }
        /* Parse search param */
        const char *srch = NULL;
        char srch_buf[65] = "";
        const char *sp = strstr(path, "q=");
        if (sp) {
            sp += 2;
            size_t si = 0;
            while (sp[si] && sp[si] != '&' && si < 64)
                { srch_buf[si] = sp[si]; si++; }
            srch_buf[si] = '\0';
            srch = srch_buf;
        }
        return serve_history(response, response_max, page, filt, srch);
    }
    if (strcmp(path, "/wallet/coins") == 0)
        return serve_coins(response, response_max);
    if (strncmp(path, "/wallet/tx/", 11) == 0) {
        const char *txid = path + 11;
        return serve_tx_detail(response, response_max, txid);
    }

    return 0;
}
