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
#include "views/format_helpers.h"
#include "event/event.h"
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

#define PRIMARY_ADDR "t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn"
#define FEE_ZCL 0.0001

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

/* Parse z_gettotalbalance JSON response into satoshi amounts.
 * Returns true if at least one balance field was found. */
static bool parse_balance_response(const char *buf,
                                   int64_t *transparent_out,
                                   int64_t *shielded_out)
{
    char tval[32] = "", pval[32] = "";
    zcl_json_extract_str(buf, "transparent", tval, sizeof(tval));
    zcl_json_extract_str(buf, "private", pval, sizeof(pval));

    if (tval[0]) *transparent_out = (int64_t)(strtod(tval, NULL) * 1e8 + 0.5);
    if (pval[0]) *shielded_out = (int64_t)(strtod(pval, NULL) * 1e8 + 0.5);
    return tval[0] || pval[0];
}

/* Query running node for wallet balance via z_gettotalbalance RPC.
 * Tries C23 node (port 18232) first, then falls back to legacy
 * zclassicd (port 8232) with zcluser:zclpass credentials. */
static bool query_node_balance(int64_t *transparent_out, int64_t *shielded_out)
{
    char buf[4096];

    /* Try C23 node first */
    if (wallet_rpc_call_port("z_gettotalbalance", "[]", buf, sizeof(buf),
                             18232, NULL) > 0) {
        if (parse_balance_response(buf, transparent_out, shielded_out)) {
            if (*transparent_out > 0 || *shielded_out > 0)
                return true;
        }
    }

    /* Fall back to legacy zclassicd on port 8232 */
    memset(buf, 0, sizeof(buf));
    if (wallet_rpc_call_port("z_gettotalbalance", "[]", buf, sizeof(buf),
                             8232, "zcluser:zclpass") > 0) {
        if (parse_balance_response(buf, transparent_out, shielded_out))
            return true;
    }

    return false;
}

/* ── Shared CSS ─────────────────────────────────────────────── */

#define WALLET_CSS_1 \
    "body{font-family:-apple-system,'Segoe UI',Roboto,monospace;" \
    "background:#0c0c0c;color:#e8e8e8;max-width:960px;margin:0 auto;" \
    "padding:16px 20px;font-size:16px;line-height:1.5}" \
    "*{box-sizing:border-box}" \
    "a{color:#4db8ff;text-decoration:none}" \
    "a:hover{color:#80ccff;text-decoration:underline}" \
    "h1{color:#33ff99;font-size:28px;margin:0 0 4px;font-weight:800}" \
    "h2{color:#33ff99;font-size:20px;border-bottom:1px solid #222;" \
    "padding-bottom:6px;margin:24px 0 12px}" \
    "h3{color:#aaa;font-size:16px;margin:20px 0 8px}" \
    ".subtitle{color:#888;font-size:13px;margin:0 0 16px}" \
    ".stats{display:flex;gap:10px;margin:12px 0;flex-wrap:wrap}" \
    ".stat{flex:1;min-width:130px;background:#141414;padding:14px;" \
    "border-radius:8px;text-align:center;border:1px solid #1e1e1e}" \
    ".stat .n{font-size:28px;color:#33ff99;font-weight:800;line-height:1.2}" \
    ".stat .l{font-size:11px;color:#888;text-transform:uppercase;" \
    "letter-spacing:1px;margin-top:2px}" \
    ".nav{display:flex;gap:8px;margin:14px 0;flex-wrap:wrap}" \
    ".nav a{background:#141414;padding:8px 16px;border-radius:6px;" \
    "border:1px solid #1e1e1e;font-size:14px;font-weight:600}" \
    ".nav a:hover{border-color:#33ff99;color:#33ff99}" \
    ".nav a.active{border-color:#33ff99;color:#33ff99;background:#0a1f0a}" \
    ".card{background:#141414;padding:14px 18px;border-radius:8px;" \
    "margin:8px 0;border:1px solid #1e1e1e;border-left:3px solid #33ff99}" \
    ".card .label{color:#888;font-size:12px}" \
    ".card .value{font-size:22px;color:#33ff99;font-weight:700}" \
    ".card .sub{color:#888;font-size:12px;margin-top:2px}" \
    "table{width:100%%;border-collapse:collapse;font-size:14px}" \
    "th{text-align:left;color:#888;padding:8px;border-bottom:1px solid #222;" \
    "font-size:12px;text-transform:uppercase;letter-spacing:0.5px}" \
    "td{padding:8px;border-bottom:1px solid #1a1a1a}" \
    "tr:hover{background:#161616}" \
    ".mono{font-family:'SF Mono','Fira Code',monospace;font-size:13px}" \
    ".hash{color:#4db8ff;font-family:'SF Mono',monospace;font-size:13px}" \
    ".zcl{color:#33ff99;font-weight:700}"

#define WALLET_CSS_2 \
    ".addr-box{background:#0a0a0a;padding:12px;border-radius:6px;" \
    "font-family:monospace;font-size:14px;color:#4db8ff;" \
    "word-break:break-all;text-align:center;margin:10px 0;" \
    "border:1px solid #222;user-select:all;cursor:pointer}" \
    ".addr-box-sm{background:#0a0a0a;padding:10px;border-radius:6px;" \
    "font-family:monospace;font-size:11px;color:#9999ff;" \
    "word-break:break-all;text-align:center;margin:8px 0;" \
    "border:1px solid #222;user-select:all;cursor:pointer}" \
    "input,select{background:#1a1a1a;color:#e8e8e8;border:1px solid #333;" \
    "padding:10px 14px;font-family:inherit;font-size:15px;width:100%%;" \
    "border-radius:6px;margin:4px 0}" \
    "input:focus{border-color:#33ff99;outline:none}" \
    "button{background:#33ff99;color:#0c0c0c;border:none;padding:12px 24px;" \
    "font-size:16px;font-weight:700;border-radius:6px;cursor:pointer;" \
    "font-family:inherit;width:100%%}" \
    "button:hover{background:#44ffaa}" \
    ".pill{display:inline-block;padding:2px 8px;border-radius:10px;" \
    "font-size:11px;font-weight:700}" \
    ".pill-t{background:#1a2a1a;color:#33ff99}" \
    ".pill-z{background:#1a1a2a;color:#9999ff}" \
    ".pill-recv{background:#0a1f0a;color:#33ff99}" \
    ".pill-send{background:#2a1a1a;color:#ff6666}" \
    ".err{color:#ff4444;font-size:13px;margin:4px 0}" \
    ".qr-wrap{text-align:center;margin:16px 0}" \
    ".total-row{font-weight:700;background:#0a1f0a}" \
    ".overflow-x{overflow-x:auto}" \
    ".tx-card{background:#141414;padding:14px 18px;border-radius:8px;" \
    "margin:8px 0;border:1px solid #1e1e1e;border-left:3px solid #333}" \
    ".tx-card:hover{background:#181818}" \
    ".tx-amount{font-size:22px;font-weight:800;font-family:'SF Mono',monospace}" \
    ".tx-amount.recv{color:#33ff99}" \
    ".tx-amount.send{color:#ff6666}" \
    ".tx-meta{display:flex;gap:12px;align-items:center;flex-wrap:wrap;" \
    "margin-top:4px;font-size:13px}" \
    ".tx-time{color:#888}" \
    ".tx-hash{color:#4db8ff;font-family:'SF Mono',monospace;font-size:12px}" \
    ".tx-conf{color:#555;font-size:11px}" \
    "footer{text-align:center;color:#333;font-size:11px;margin-top:32px}" \
    "@keyframes sync-pulse{0%%,100%%{opacity:1}50%%{opacity:.5}}" \
    ".pill-syncing{animation:sync-pulse 1.5s ease infinite}" \
    ".actions{display:flex;gap:12px;margin:16px 0}" \
    ".actions a{flex:1;text-align:center;background:#141414;padding:14px;" \
    "border-radius:8px;border:1px solid #1e1e1e;font-size:16px;font-weight:700;" \
    "color:#e8e8e8;text-decoration:none}" \
    ".actions a:hover{border-color:#33ff99;color:#33ff99}" \
    ".sync-note{color:#4db8ff;font-size:12px;margin-top:4px}" \
    "@media(max-width:600px){" \
    ".stat{min-width:100px;padding:10px}" \
    ".stat .n{font-size:20px}" \
    "table{font-size:12px}" \
    "td,th{padding:5px}" \
    "}"

/* ── Navigation with active state ───────────────────────────── */

static size_t emit_nav(uint8_t *buf, size_t max, const char *active) {
    struct { const char *href; const char *label; } tabs[] = {
        { "/wallet",         "Dashboard" },
        { "/wallet/send",    "Send"      },
        { "/wallet/receive", "Receive"   },
        { "/wallet/history", "History"   },
        { "/wallet/coins",   "Coins"     },
    };
    int n = snprintf((char *)buf, max, "<div class='nav'>");
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
    int w = snprintf((char *)buf + off, max - off, "</div>");
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
    if (timestamp <= 0) { snprintf(out, out_max, "unknown"); return; }
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
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title><style>",
        title);
    APPEND(off, buf, max, WALLET_CSS_1);
    APPEND(off, buf, max, WALLET_CSS_2);
    APPEND(off, buf, max,
        "</style></head><body>");
    off += emit_nav(buf + off, max - off, active_tab);
    return off;
}

static void emit_footer(uint8_t *buf, size_t max, size_t *off) {
    APPEND(*off, buf, max, "</body></html>");
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

/* ── Ground-truth balance: scan global UTXO set for wallet keys ── */

static int64_t query_ground_truth_balance(sqlite3 *db, int *utxo_count) {
    int64_t total = 0;
    int count = 0;
    sqlite3_stmt *s = NULL;

    /* P2PKH: match utxos.address_hash against wallet_keys.pubkey_hash */
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(u.value),0), COUNT(*) FROM utxos u "
            "WHERE u.address_hash IN "
            "(SELECT pubkey_hash FROM wallet_keys)",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            total += sqlite3_column_int64(s, 0);
            count += sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }

    /* P2SH: find UTXOs at addresses that received change from wallet txs.
     * Heuristic: any address that appears as an output of a transaction
     * that spent from a known wallet P2PKH address is a wallet address. */
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(u.value),0), COUNT(*) FROM utxos u "
            "WHERE u.address_hash NOT IN "
            "  (SELECT pubkey_hash FROM wallet_keys) "
            "AND u.address_hash IN ("
            "  SELECT DISTINCT to2.address_hash "
            "  FROM tx_inputs ti "
            "  JOIN tx_outputs to1 ON ti.prev_txid = to1.txid AND ti.prev_vout = to1.vout "
            "  JOIN tx_outputs to2 ON ti.txid = to2.txid "
            "  WHERE to1.address_hash IN (SELECT pubkey_hash FROM wallet_keys) "
            "  AND to2.address_hash != to1.address_hash"
            ")",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            total += sqlite3_column_int64(s, 0);
            count += sqlite3_column_int(s, 1);
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
        size_t off = emit_header(r, max, "ZClassic Wallet", "/wallet");
        APPEND(off, r, max,
            "<div style='text-align:center;padding:48px 0'>"
            "<div style='font-size:40px;margin-bottom:12px'>&#x23F3;</div>"
            "<div style='color:#e8e8e8;font-size:20px;font-weight:600'>"
            "Wallet Loading</div>"
            "<div style='color:#888;font-size:14px;margin-top:8px'>"
            "The database is not yet available. The node may still be starting.</div>"
            "</div>");
        emit_footer(r, max, &off);
        return off;
    }

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");

    /* Ground-truth transparent balance (P2PKH + P2SH change addresses) */
    int t_utxos = 0;
    int64_t transparent = query_ground_truth_balance(db, &t_utxos);

    /* Speed-layer balance (wallet_utxos cache — may differ) */
    int64_t speed_bal = query_speed_balance(db);

    /* Shielded: verified notes minus spent nullifiers */
    int z_notes = 0;
    int64_t shielded = query_shielded_balance(db, &z_notes);

    /* RPC fallback if SQLite has no balance */
    if (transparent == 0 && shielded == 0) {
        int64_t rpc_t = 0, rpc_z = 0;
        if (query_node_balance(&rpc_t, &rpc_z)) {
            transparent = rpc_t;
            shielded = rpc_z;
        }
    }

    int64_t total_balance = transparent + shielded;

    size_t off = emit_header(r, max, "ZClassic Wallet", "/wallet");

    const char *sync = sync_state_name(sync_get_state());
    bool synced = (sync_get_state() == SYNC_AT_TIP);

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

    /* Balance hero */
    APPEND(off, r, max,
        "<div style='text-align:center;padding:32px 0 20px'>"
        "<span id='sync' class='pill %s' style='font-size:10px'>%s</span>"
        "<div id='bal' style='font-size:40px;color:#34d399;"
        "font-weight:700;letter-spacing:-1px;margin-top:8px'>"
        "%s ZCL</div>",
        synced ? "pill-t" : "pill-syncing",
        synced ? "Synced" : sync,
        bal_str);

    /* Always show breakdown: transparent + shielded */
    APPEND(off, r, max,
        "<div id='breakdown' style='color:#6b7280;font-size:13px;"
        "margin-top:8px'>"
        "%.8f transparent",
        (double)transparent / (double)ZATOSHI_PER_ZCL);
    if (shielded > 0)
        APPEND(off, r, max, " + %.8f shielded",
            (double)shielded / (double)ZATOSHI_PER_ZCL);
    APPEND(off, r, max, "</div>");

    if (!synced) {
        APPEND(off, r, max,
            "<div style='color:#60a5fa;font-size:12px;margin-top:6px'>"
            "Syncing &mdash; balance updating</div>");
    }
    APPEND(off, r, max, "</div>");

    /* Primary actions */
    APPEND(off, r, max,
        "<div class='actions'>"
        "<a href='/wallet/send' style='border:2px solid #374151'>Send</a>"
        "<a href='/wallet/receive' style='background:#34d399;"
        "color:#0a0a0a;border:none'>Receive</a>"
        "</div>");

    /* Shielded address count + ZSLP tokens */
    {
        int z_count = 0, token_count = 0;
        sqlite3_stmt *zs = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT count(*) FROM wallet_sapling_keys "
                "WHERE address IS NOT NULL AND length(address) > 0",
                -1, &zs, NULL) == SQLITE_OK && zs) {
            if (sqlite3_step(zs) == SQLITE_ROW)
                z_count = sqlite3_column_int(zs, 0);
            sqlite3_finalize(zs);
        }
        zs = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT count(*) FROM zslp_tokens",
                -1, &zs, NULL) == SQLITE_OK && zs) {
            if (sqlite3_step(zs) == SQLITE_ROW)
                token_count = sqlite3_column_int(zs, 0);
            sqlite3_finalize(zs);
        }

        if (z_count > 0 || shielded > 0 || token_count > 0) {
            APPEND(off, r, max,
                "<div style='display:flex;gap:16px;margin:8px 0 0;"
                "font-size:12px;color:#6b7280'>");
            if (z_count > 0 || shielded > 0)
                APPEND(off, r, max,
                    "<a href='/wallet/receive' style='color:#a78bfa'>"
                    "%d shielded address%s</a>",
                    z_count, z_count == 1 ? "" : "es");
            if (token_count > 0)
                APPEND(off, r, max,
                    "<a href='/explorer/tokens' style='color:#6b7280'>"
                    "%d ZSLP tokens</a>", token_count);
            APPEND(off, r, max, "</div>");
        }
    }

    /* ── Transparent UTXOs breakdown ─────────────────────────── */
    APPEND(off, r, max,
        "<div style='margin-top:24px'>"
        "<div style='display:flex;justify-content:space-between;"
        "align-items:baseline'>"
        "<div style='color:#6b7280;font-size:12px;font-weight:600;"
        "text-transform:uppercase;letter-spacing:0.05em'>"
        "Your Coins</div>"
        "<div style='color:#34d399;font-size:14px;font-weight:700;"
        "font-family:monospace'>%.8f ZCL</div></div>",
        (double)transparent / (double)ZATOSHI_PER_ZCL);

    /* Show top UTXOs from ground-truth query (P2PKH + P2SH change) */
    {
        sqlite3_stmt *us = NULL;
        int shown = 0;
        /* Union P2PKH and P2SH change UTXOs, sorted by value desc */
        const char *utxo_sql =
            "SELECT u.value, u.height, hex(u.txid), u.vout, "
            "  CASE WHEN u.address_hash IN "
            "    (SELECT pubkey_hash FROM wallet_keys) "
            "  THEN 'P2PKH' ELSE 'P2SH' END as stype "
            "FROM utxos u "
            "WHERE u.address_hash IN "
            "  (SELECT pubkey_hash FROM wallet_keys) "
            "OR u.address_hash IN ("
            "  SELECT DISTINCT to2.address_hash "
            "  FROM tx_inputs ti "
            "  JOIN tx_outputs to1 ON ti.prev_txid = to1.txid "
            "    AND ti.prev_vout = to1.vout "
            "  JOIN tx_outputs to2 ON ti.txid = to2.txid "
            "  WHERE to1.address_hash IN "
            "    (SELECT pubkey_hash FROM wallet_keys) "
            "  AND to2.address_hash != to1.address_hash"
            ") "
            "ORDER BY u.value DESC LIMIT 5";
        if (sqlite3_prepare_v2(db, utxo_sql, -1, &us, NULL) == SQLITE_OK) {
            while (sqlite3_step(us) == SQLITE_ROW && off + 500 < max) {
                int64_t val = sqlite3_column_int64(us, 0);
                int h = sqlite3_column_int(us, 1);
                const char *txid = (const char *)sqlite3_column_text(us, 2);
                int vout = sqlite3_column_int(us, 3);
                const char *stype = (const char *)sqlite3_column_text(us, 4);
                if (!txid) continue;

                int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
                if (confs < 0) confs = 0;
                char short_tx[18];
                txid_short(txid, short_tx, sizeof(short_tx));
                char lower_tx[65];
                txid_lower(txid, lower_tx, sizeof(lower_tx));

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
                /* Format confirmations with commas */
                char c_fmt[20];
                {
                    char tmp[20];
                    int tl = snprintf(tmp, sizeof(tmp), "%d", confs);
                    int ci = 0, ti = 0;
                    int digits_left = tl;
                    for (int di = 0; di < tl && ci < (int)sizeof(c_fmt)-1; di++) {
                        c_fmt[ci++] = tmp[ti++];
                        digits_left--;
                        if (digits_left > 0 && digits_left % 3 == 0)
                            c_fmt[ci++] = ',';
                    }
                    c_fmt[ci] = '\0';
                }
                const char *type_label = (stype && stype[2] == 'S') ? "Script" : "Standard";

                APPEND(off, r, max,
                    "<div style='display:flex;justify-content:space-between;"
                    "align-items:center;padding:8px 0;"
                    "border-bottom:1px solid #1a1a1a'>"
                    "<div>"
                    "<a href='/explorer/tx/%s' style='color:#4db8ff;"
                    "font-family:monospace;font-size:12px'>%s:%d</a>"
                    " <span class='pill pill-%s' "
                    "style='font-size:9px'>%s</span>"
                    "</div>"
                    "<div style='text-align:right'>"
                    "<span style='color:#34d399;font-size:14px;"
                    "font-weight:700;font-family:monospace'>%.8f</span>"
                    "<span style='color:#555;font-size:11px;"
                    "margin-left:6px'>Block %s &middot; %s conf</span>"
                    "</div></div>",
                    lower_tx, short_tx, vout,
                    stype && stype[2] == 'S' ? "z" : "t", type_label,
                    (double)val / 1e8, h_fmt, c_fmt);
                shown++;
            }
            sqlite3_finalize(us);
        }
        if (t_utxos > shown && shown > 0) {
            /* Calculate remaining value */
            APPEND(off, r, max,
                "<div style='color:#555;font-size:12px;padding:6px 0;"
                "text-align:center'>+ %d more UTXO%s</div>",
                t_utxos - shown, (t_utxos - shown) == 1 ? "" : "s");
        }
    }
    APPEND(off, r, max, "</div>");

    /* ── Data Integrity check ─────────────────────────────────── */
    {
        int64_t discrepancy = transparent - speed_bal;
        if (discrepancy != 0 && speed_bal > 0) {
            APPEND(off, r, max,
                "<div style='margin-top:16px;background:#1a1510;"
                "border:1px solid #4a3520;border-radius:8px;padding:12px;"
                "font-size:12px'>"
                "<div style='color:#f59e0b;font-weight:700;"
                "margin-bottom:4px'>Balance Discrepancy</div>"
                "<div style='color:#92712a'>"
                "Cached balance: %.8f ZCL"
                "<br>Chain-verified: %.8f ZCL "
                "(%d UTXOs from chain UTXO set)"
                "<br>Difference: %.8f ZCL &mdash; "
                "run <code style='color:#f59e0b'>rescanwallet</code>"
                " to fix</div></div>",
                (double)speed_bal / 1e8,
                (double)transparent / 1e8, t_utxos,
                (double)discrepancy / 1e8);
        }
    }

    /* ── Recent transactions ─────────────────────────────────── */
    APPEND(off, r, max,
        "<div style='margin-top:20px'>"
        "<div style='display:flex;justify-content:space-between;"
        "align-items:baseline'>"
        "<div style='color:#6b7280;font-size:12px;font-weight:600;"
        "text-transform:uppercase;letter-spacing:0.05em'>"
        "Recent</div>"
        "<a href='/wallet/history' style='color:#6b7280;"
        "font-size:12px'>View all</a></div>");

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT u.value, u.height, COALESCE(b.time,0), hex(u.txid) "
            "FROM utxos u "
            "WHERE u.address_hash IN "
            "  (SELECT pubkey_hash FROM wallet_keys) "
            "OR u.address_hash IN ("
            "  SELECT DISTINCT to2.address_hash "
            "  FROM tx_inputs ti "
            "  JOIN tx_outputs to1 ON ti.prev_txid = to1.txid "
            "    AND ti.prev_vout = to1.vout "
            "  JOIN tx_outputs to2 ON ti.txid = to2.txid "
            "  WHERE to1.address_hash IN "
            "    (SELECT pubkey_hash FROM wallet_keys) "
            "  AND to2.address_hash != to1.address_hash"
            ") "
            "LEFT JOIN blocks b ON b.height = u.height "
            "ORDER BY u.height DESC LIMIT 5",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 400 < max) {
            int64_t value = sqlite3_column_int64(s, 0);
            int height = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            const char *txid = (const char *)sqlite3_column_text(s, 3);

            char rel_time[48], esc_rel[96];
            format_relative_time(btime, rel_time, sizeof(rel_time));
            html_escape(esc_rel, sizeof(esc_rel), rel_time);

            char lower_tx[65];
            if (txid) txid_lower(txid, lower_tx, sizeof(lower_tx));
            else lower_tx[0] = '\0';

            int confs = (tip > 0 && height > 0) ? (tip - height + 1) : 0;

            char amt[32];
            zcl_format_zcl(amt, sizeof(amt), value);

            APPEND(off, r, max,
                "<div style='display:flex;justify-content:space-between;"
                "align-items:center;padding:10px 0;"
                "border-bottom:1px solid #1a1a1a'>"
                "<div>"
                "<span style='color:#34d399;font-size:16px;font-weight:700;"
                "font-family:monospace'>+%s</span>"
                "<span style='color:#6b7280;font-size:12px;"
                "margin-left:8px'>ZCL</span></div>"
                "<div style='text-align:right'>"
                "<div style='color:#6b7280;font-size:13px'>%s</div>"
                "<a href='/explorer/tx/%s' style='color:#374151;"
                "font-size:11px;font-family:monospace'>%d conf%s</a>"
                "</div></div>",
                amt, esc_rel, lower_tx,
                confs, confs == 1 ? "" : "s");
        }
        sqlite3_finalize(s);
    }
    APPEND(off, r, max, "</div>");

    /* Live pulse + event ticker JS — polls ring buffer for events */
    APPEND(off, r, max,
        "<script>"
        "function fmt(z){var v=z/1e8;if(z===0)return'0.00';"
        "if(z%%1000000===0)return v.toFixed(2);"
        "if(z%%10000===0)return v.toFixed(4);return v.toFixed(8);}"
        "setInterval(function(){"
        "fetch('zcl://node/api/wallet/pulse')"
        ".then(function(r){return r.json()})"
        ".then(function(d){"
        "var b=document.getElementById('bal');"
        "if(b){var n=fmt(d.balance+d.shielded)+' ZCL';"
        "if(b.textContent!==n){b.textContent=n;}}"
        "var s=document.getElementById('sync');"
        "if(s){s.textContent=d.sync==='at_tip'?'Synced':d.sync;"
        "s.className='pill '+(d.sync==='at_tip'?'pill-t':'pill-syncing');}"
        "var bd=document.getElementById('breakdown');"
        "if(bd){var t=fmt(d.balance)+' transparent';"
        "if(d.shielded>0)t+=' + '+fmt(d.shielded)+' shielded';"
        "bd.textContent=t;}"
        "}).catch(function(){});"
        "},2000);"
        "</script>");

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

    size_t off = emit_header(r, max, "Send ZCL", "/wallet/send");

    char bal_fmt[32];
    zcl_format_zcl(bal_fmt, sizeof(bal_fmt), balance);

    APPEND(off, r, max,
        "<div style='text-align:center;padding:16px 0;color:#6b7280;"
        "font-size:13px'>Available: <span style='color:#34d399;"
        "font-weight:600'>%s ZCL</span></div>",
        bal_fmt);

    APPEND(off, r, max,
        "<div class='card'>"
        "<form id='send-form' method='POST' action='zcl://node/wallet/send/review' "
        "onsubmit='return validateSend()'>"
        "<label class='label' for='addr'>To</label>"
        "<input type='text' id='addr' name='address' "
        "placeholder='t1... or zs1...' required>"
        "<div id='addr-err' class='err'></div>"
        "<label class='label' for='amt' style='margin-top:12px'>Amount</label>"
        "<div style='display:flex;gap:8px'>"
        "<input type='text' id='amt' name='amount' style='flex:1' "
        "placeholder='0.00' required oninput='updateRemaining()'>"
        "<button type='button' style='background:#333;color:#34d399;"
        "border:1px solid #333;padding:8px 12px;border-radius:6px;"
        "font-size:12px;cursor:pointer;white-space:nowrap' "
        "onclick='document.getElementById(\"amt\").value="
        "(BAL-%.4f).toFixed(8);updateRemaining()'>Send Max</button></div>"
        "<div id='remaining' class='remaining'></div>"
        "<div id='amt-err' class='err'></div>"
        "<button type='submit' style='margin-top:16px' "
        "onclick='this.disabled=true;this.form.submit()'>Send</button>"
        "</form></div>"
        "<script>"
        "var BAL=%.8f;"
        "function updateRemaining(){"
        "var a=parseFloat(document.getElementById('amt').value)||0;"
        "var r=document.getElementById('remaining');"
        "if(a>0&&a<=BAL){r.textContent='Remaining: '+(BAL-a-%.4f).toFixed(8)+' ZCL';"
        "r.style.color='#6b7280';}"
        "else if(a>BAL){r.textContent='Insufficient funds';"
        "r.style.color='#f87171';}"
        "else{r.textContent='';}}"
        "function validateSend(){"
        "var a=document.getElementById('addr').value.trim();"
        "var m=document.getElementById('amt').value.trim();"
        "document.getElementById('addr-err').textContent='';"
        "document.getElementById('amt-err').textContent='';"
        "if(!a||a.length<26){"
        "document.getElementById('addr-err').textContent="
        "'Enter a valid address';return false;}"
        "var amt=parseFloat(m);"
        "if(isNaN(amt)||amt<=0){"
        "document.getElementById('amt-err').textContent="
        "'Enter an amount';return false;}"
        "if(amt+%.4f>BAL){"
        "document.getElementById('amt-err').textContent="
        "'Insufficient funds';return false;}"
        "return true;}"
        "</script>",
        (double)balance / (double)ZATOSHI_PER_ZCL, FEE_ZCL, FEE_ZCL, FEE_ZCL);

    emit_footer(r, max, &off);
    return off;
}

/* ── Receive (/wallet/receive) ──────────────────────────────── */

static size_t serve_receive(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    size_t off = emit_header(r, max, "Receive — ZClassic23", "/wallet/receive");

    /* QR code is the hero — the address IS the page */
    APPEND(off, r, max,
        "<div style='text-align:center;padding:16px 0'>"
        "<div style='color:#6b7280;font-size:14px;margin-bottom:12px'>"
        "Share this address to receive ZCL</div>");
    off = emit_qr_svg(r, max, off, PRIMARY_ADDR, 5);
    APPEND(off, r, max,
        "<div class='addr-box' style='margin-top:16px'>"
        PRIMARY_ADDR "</div>"
        "<div id='copy-msg' style='color:#6b7280;font-size:12px;"
        "margin-top:4px;height:16px'></div>"
        "</div>");

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
                if (z_shown == 0)
                    APPEND(off, r, max,
                        "<div class='card' style='border-left-color:#a78bfa'>"
                        "<div class='label'>Shielded Address</div>");
                APPEND(off, r, max,
                    "<div class='addr-box-sm'>%s</div>", escaped);
                z_shown++;
            }
            sqlite3_finalize(s);
        }
        sqlite3_close(db);
    }

    /* RPC fallback: get z-addresses from running node if SQLite empty */
    if (z_shown == 0) {
        char rpc_buf[8192];
        if (wallet_rpc_call_port("z_listaddresses", "[]",
                                  rpc_buf, sizeof(rpc_buf),
                                  18232, NULL) > 0) {
            /* Parse JSON array of strings: ["zs1...", "zs1...", ...] */
            const char *p = strchr(rpc_buf, '[');
            if (p) {
                p++;
                while (*p && z_shown < 3 && off + 512 < max) {
                    while (*p == ' ' || *p == '\n' || *p == ',') p++;
                    if (*p == '"') {
                        p++;
                        const char *end = strchr(p, '"');
                        if (end && end - p > 10) {
                            char addr[256];
                            size_t alen = (size_t)(end - p);
                            if (alen >= sizeof(addr)) alen = sizeof(addr) - 1;
                            memcpy(addr, p, alen);
                            addr[alen] = '\0';
                            if (z_shown == 0)
                                APPEND(off, r, max,
                                    "<div class='card' "
                                    "style='border-left-color:#a78bfa'>"
                                    "<div class='label'>Shielded Addresses "
                                    "(from wallet)</div>");
                            APPEND(off, r, max,
                                "<div class='addr-box-sm'>%s</div>", addr);
                            z_shown++;
                            p = end + 1;
                        } else break;
                    } else break;
                }
            }
        }
    }
    if (z_shown > 0)
        APPEND(off, r, max, "</div>");

    /* Click-to-copy with "Copied!" feedback */
    APPEND(off, r, max,
        "<script>"
        "document.querySelectorAll('.addr-box,.addr-box-sm')"
        ".forEach(function(el){"
        "el.style.cursor='pointer';"
        "el.addEventListener('click',function(){"
        "var txt=this.textContent.trim();"
        "navigator.clipboard.writeText(txt).then(function(){"
        "el.style.borderColor='#34d399';"
        "var msg=document.getElementById('copy-msg');"
        "if(msg)msg.textContent='Copied!';"
        "setTimeout(function(){el.style.borderColor='';"
        "if(msg)msg.textContent='';},1500);});"
        "});});"
        "</script>");

    emit_footer(r, max, &off);
    return off;
}

/* ── History (/wallet/history) ──────────────────────────────── */

static size_t serve_history(uint8_t *r, size_t max, int page) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");
    int per_page = 50;

    size_t off = emit_header(r, max, "Transaction History", "/wallet/history");

    int tx_count = query_int(db,
        "SELECT count(*) FROM wallet_transactions");

    int total_pages = (tx_count + per_page - 1) / per_page;
    if (page >= total_pages && total_pages > 0) page = total_pages - 1;

    APPEND(off, r, max,
        "<h2>Transaction History</h2>"
        "<div class='sub'>%d transaction%s (page %d of %d)</div>",
        tx_count, tx_count == 1 ? "" : "s",
        page + 1, total_pages > 0 ? total_pages : 1);

    /* Timeline view (tx-cards).
     * Use from_me to determine send vs receive.
     * Compute net value from wallet UTXOs for this txid. */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(wt.txid), wt.block_height, b.time, "
            "wt.from_me, wt.fee, "
            "COALESCE((SELECT SUM(wu.value) FROM wallet_utxos wu "
            "  WHERE wu.txid = wt.txid),0) "
            "FROM wallet_transactions wt "
            "LEFT JOIN blocks b ON wt.block_height = b.height "
            "ORDER BY wt.block_height DESC LIMIT ? OFFSET ?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, per_page);
        sqlite3_bind_int(s, 2, page * per_page);
        while (sqlite3_step(s) == SQLITE_ROW && off + 600 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int from_me = sqlite3_column_int(s, 3);
            int64_t fee = sqlite3_column_int64(s, 4);
            int64_t wallet_output = sqlite3_column_int64(s, 5);
            if (!txid) continue;

            bool is_recv = (from_me == 0);
            int64_t display_val = is_recv ? wallet_output :
                                  (fee > 0 ? fee : wallet_output);

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
                "<div class='tx-card' style='border-left-color:%s'>"
                "<div class='tx-amount %s'>%s%.8f ZCL</div>"
                "<div class='tx-meta'>"
                "<span class='tx-time' title='%s'>%s</span>"
                "<a href='/explorer/tx/%s' class='tx-hash'>%s</a>"
                "<span class='tx-conf'>Block %s &middot; %d conf%s</span>"
                "</div></div>",
                is_recv ? "#33ff99" : "#ff6666",
                is_recv ? "recv" : "send",
                is_recv ? "+" : "-",
                (double)display_val / 1e8,
                esc_ts,
                esc_rel,
                esc_lower, esc_short,
                h_fmt, confs, confs == 1 ? "" : "s");
        }
        sqlite3_finalize(s);
    }

    /* Pagination */
    if (total_pages > 1) {
        APPEND(off, r, max,
            "<div style='display:flex;justify-content:center;gap:12px;"
            "margin:20px 0;font-size:14px'>");
        if (page > 0)
            APPEND(off, r, max,
                "<a href='/wallet/history?page=%d' style='color:#4db8ff'>"
                "&larr; Newer</a>", page - 1);
        if (page < total_pages - 1)
            APPEND(off, r, max,
                "<a href='/wallet/history?page=%d' style='color:#4db8ff'>"
                "Older &rarr;</a>", page + 1);
        APPEND(off, r, max, "</div>");
    }

    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Coins (/wallet/coins) — Full UTXO audit view ──────────── */

static size_t serve_coins(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

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
        "SELECT hex(u.txid), u.vout, u.value, u.height, "
        "  CASE WHEN u.address_hash IN "
        "    (SELECT pubkey_hash FROM wallet_keys) "
        "  THEN 'P2PKH' ELSE 'P2SH' END "
        "FROM utxos u "
        "WHERE u.address_hash IN "
        "  (SELECT pubkey_hash FROM wallet_keys) "
        "OR u.address_hash IN ("
        "  SELECT DISTINCT to2.address_hash "
        "  FROM tx_inputs ti "
        "  JOIN tx_outputs to1 ON ti.prev_txid = to1.txid "
        "    AND ti.prev_vout = to1.vout "
        "  JOIN tx_outputs to2 ON ti.txid = to2.txid "
        "  WHERE to1.address_hash IN "
        "    (SELECT pubkey_hash FROM wallet_keys) "
        "  AND to2.address_hash != to1.address_hash"
        ") "
        "ORDER BY u.value DESC";
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
                "<td class='zcl'>%.8f</td>"
                "<td>%d</td>"
                "<td>%d</td>"
                "</tr>",
                lower_tx, short_tx, vout,
                stype && stype[2] == 'S' ? "z" : "t",
                (stype && stype[2] == 'S') ? "Script" : "Standard",
                (double)val / 1e8, h, confs);
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
    }

    if (amount <= 0) {
        size_t off = emit_header(r, max, "Shield — ZClassic23", "/wallet");
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#ff4444'>"
            "<div class='label' style='color:#ff4444'>Invalid Amount</div>"
            "<div class='sub'>Amount must be greater than 0.</div>"
            "<a href='/wallet' style='color:#4db8ff'>Back to Wallet</a></div>");
        emit_footer(r, max, &off);
        return off;
    }

    double fee = FEE_ZCL;
    double total_cost = amount + fee;

    size_t off = emit_header(r, max, "Shield Funds — ZClassic23", "/wallet");

    APPEND(off, r, max,
        "<div class='card' style='border-left-color:#9966ff;padding:20px;"
        "background:linear-gradient(135deg,#141414,#1a1a2a)'>"
        "<div style='text-align:center'>"
        "<div style='font-size:14px;color:#888;margin-bottom:8px'>"
        "Shielding</div>"
        "<div style='font-size:40px;color:#bb99ff;font-weight:800'>"
        "%.8f ZCL</div>"
        "<div style='color:#888;font-size:13px;margin-top:8px'>"
        "Fee: %.4f ZCL &middot; Total: %.8f ZCL</div>"
        "</div></div>",
        amount, fee, total_cost);

    APPEND(off, r, max,
        "<div class='card'>"
        "<div style='color:#888;font-size:13px;line-height:1.6'>"
        "<div style='margin-bottom:8px'>"
        "<span style='color:#33ff99;font-weight:700'>Step 1:</span> "
        "Your transparent ZCL moves to a shielded address.</div>"
        "<div style='margin-bottom:8px'>"
        "<span style='color:#bb99ff;font-weight:700'>Step 2:</span> "
        "Wait ~6 hours for the timing link to break.</div>"
        "<div>"
        "<span style='color:#4db8ff;font-weight:700'>Step 3:</span> "
        "Your funds are fully private and untraceable.</div>"
        "</div></div>");

    APPEND(off, r, max,
        "<div style='display:flex;gap:10px;margin:16px 0'>"
        "<a href='/wallet' style='flex:1;background:#333;color:#e8e8e8;"
        "padding:12px;border-radius:6px;text-align:center;"
        "font-weight:700;text-decoration:none;font-size:16px'>Cancel</a>"
        "<a href='/wallet/shield/confirm?amount=%.8f' "
        "style='flex:2;background:#9966ff;color:#fff;"
        "padding:12px;border-radius:6px;text-align:center;"
        "font-weight:700;text-decoration:none;font-size:16px'>"
        "Confirm Shield</a></div>",
        amount);

    emit_footer(r, max, &off);
    return off;
}

/* ── Shield Confirm (/wallet/shield/confirm?amount=X) ──────── */
/* Executes the shielding transaction via the node's z_sendmany. */

static size_t serve_shield_confirm(uint8_t *r, size_t max, const char *query) {
    double amount = 0;
    if (query) {
        const char *amt = strstr(query, "amount=");
        if (amt) amount = strtod(amt + 7, NULL);
    }

    size_t off = emit_header(r, max, "Shielding — ZClassic23", "/wallet");

    if (amount <= 0) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#ff4444'>"
            "<div class='label' style='color:#ff4444'>Invalid amount</div>"
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
            "<div class='card' style='border-left-color:#ff4444;padding:20px'>"
            "<div style='text-align:center'>"
            "<div style='font-size:40px;margin-bottom:8px'>&#x274C;</div>"
            "<div style='font-size:20px;color:#ff4444;font-weight:700'>"
            "No Shielded Address Available</div>"
            "<div style='color:#888;font-size:13px;margin-top:8px'>"
            "The wallet has no shielded addresses. Generate one with:<br>"
            "<code style='color:#4db8ff'>zcl-rpc z_getnewaddress</code></div>"
            "</div></div>"
            "<div style='text-align:center;margin:16px'>"
            "<a href='/wallet' style='color:#4db8ff;font-size:16px'>"
            "Back to Wallet</a></div>");
        emit_footer(r, max, &off);
        return off;
    }

    /* Build RPC call: send FROM transparent TO shielded z-address */
    char rpc_body[1024];
    snprintf(rpc_body, sizeof(rpc_body),
        "{\"method\":\"z_sendmany\",\"params\":[\"" PRIMARY_ADDR "\","
        "[{\"address\":\"%s\",\"amount\":%.8f}]"
        ",1,%.4f],\"id\":1}", z_dest, amount, FEE_ZCL);

    /* Read cookie for auth */
    char cookie[256] = "";
    if (g_datadir) {
        char cp[1024];
        snprintf(cp, sizeof(cp), "%s/.cookie", g_datadir);
        FILE *f = fopen(cp, "r");
        if (f) {
            if (fgets(cookie, sizeof(cookie), f)) {
                char *nl = strchr(cookie, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
        }
    }

    /* Quick check: is the node even running? Try connect with 500ms timeout */
    char result[4096] = "";
    bool success = false;
    bool node_reachable = false;

    {
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe >= 0) {
            struct sockaddr_in pa;
            memset(&pa, 0, sizeof(pa));
            pa.sin_family = AF_INET;
            pa.sin_port = htons(18232);
            pa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            /* Non-blocking connect check */
            struct timeval ptv = {.tv_sec = 0, .tv_usec = 500000};
            setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, &ptv, sizeof(ptv));
            if (connect(probe, (struct sockaddr *)&pa, sizeof(pa)) == 0)
                node_reachable = true;
            close(probe);
        }
    }

    if (!node_reachable) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#ff4444;padding:20px'>"
            "<div style='text-align:center'>"
            "<div style='font-size:40px;margin-bottom:8px'>&#x274C;</div>"
            "<div style='font-size:20px;color:#ff4444;font-weight:700'>"
            "Node Offline</div>"
            "<div style='color:#888;font-size:13px;margin-top:8px'>"
            "The ZClassic23 node is not running. Start it with:<br>"
            "<code style='color:#4db8ff'>./zclassic23 -datadir=~/.zclassic-c23</code></div>"
            "</div></div>"
            "<div style='text-align:center;margin:16px'>"
            "<a href='/wallet' style='color:#4db8ff;font-size:16px'>"
            "Back to Wallet</a></div>");
        emit_footer(r, max, &off);
        return off;
    }

    /* Node is reachable — make the z_sendmany RPC call */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(18232);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        struct timeval tv = {.tv_sec = 2};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            /* Base64 encode cookie */
            static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            char auth[512];
            size_t cl = strlen(cookie), bo = 0;
            for (size_t i = 0; i < cl; i += 3) {
                uint32_t n = ((uint32_t)(uint8_t)cookie[i]) << 16;
                if (i+1 < cl) n |= ((uint32_t)(uint8_t)cookie[i+1]) << 8;
                if (i+2 < cl) n |= (uint32_t)(uint8_t)cookie[i+2];
                auth[bo++] = b64[(n>>18)&63]; auth[bo++] = b64[(n>>12)&63];
                auth[bo++] = (i+1<cl) ? b64[(n>>6)&63] : '=';
                auth[bo++] = (i+2<cl) ? b64[n&63] : '=';
            }
            auth[bo] = '\0';

            char req[2048];
            int rl = snprintf(req, sizeof(req),
                "POST / HTTP/1.1\r\nHost:127.0.0.1\r\n"
                "Authorization:Basic %s\r\n"
                "Content-Type:application/json\r\n"
                "Content-Length:%zu\r\nConnection:close\r\n\r\n%s",
                auth, strlen(rpc_body), rpc_body);
            write(fd, req, (size_t)rl);

            char raw[8192]; size_t tot = 0;
            while (tot < sizeof(raw)-1) {
                ssize_t n = read(fd, raw+tot, sizeof(raw)-1-tot);
                if (n <= 0) break;
                tot += (size_t)n;
            }
            raw[tot] = '\0';

            char *body = strstr(raw, "\r\n\r\n");
            if (body) {
                body += 4;
                snprintf(result, sizeof(result), "%s", body);
                /* Check for operation ID (success) */
                if (strstr(result, "opid-"))
                    success = true;
            }
        }
        close(fd);
    }

    if (success) {
        /* Extract operation ID */
        char *opid = strstr(result, "opid-");
        char opid_str[128] = "";
        if (opid) {
            size_t i = 0;
            while (opid[i] && opid[i] != '"' && opid[i] != '}' && i < 127) {
                opid_str[i] = opid[i]; i++;
            }
            opid_str[i] = '\0';
        }

        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#33ff99;padding:20px'>"
            "<div style='text-align:center'>"
            "<div style='font-size:40px;margin-bottom:8px'>&#x2705;</div>"
            "<div style='font-size:20px;color:#33ff99;font-weight:700'>"
            "Shielding Started</div>"
            "<div style='color:#888;font-size:14px;margin-top:8px'>"
            "%.8f ZCL is being moved to a shielded address.</div>"
            "<div style='color:#888;font-size:12px;margin-top:12px;"
            "font-family:monospace;word-break:break-all'>%s</div>"
            "<div style='color:#555;font-size:13px;margin-top:12px'>"
            "Your funds will be fully private in ~6 hours.</div>"
            "</div></div>",
            amount, opid_str);
    } else {
        /* Show the error clearly */
        char safe_result[1024];
        html_escape(safe_result, sizeof(safe_result), result);

        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#ff8800;padding:20px'>"
            "<div style='text-align:center'>"
            "<div style='font-size:40px;margin-bottom:8px'>&#x26A0;</div>"
            "<div style='font-size:20px;color:#ff8800;font-weight:700'>"
            "Could Not Shield</div>"
            "<div style='color:#888;font-size:13px;margin-top:8px'>"
            "The node may not be running, or the wallet has no "
            "spendable funds. Check that zclassic23 is running.</div>"
            "<div style='color:#555;font-size:11px;margin-top:12px;"
            "font-family:monospace;word-break:break-all;text-align:left;"
            "background:#0a0a0a;padding:10px;border-radius:6px'>%s</div>"
            "</div></div>",
            safe_result[0] ? safe_result : "No response from node");
    }

    APPEND(off, r, max,
        "<div style='text-align:center;margin:16px'>"
        "<a href='/wallet' style='color:#4db8ff;font-size:16px'>"
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

        if (height != pulse_cache.height || pulse_cache.height == 0) {
            /* Block height changed — recompute balances */
            balance = query_ground_truth_balance(db, &t_utxos);
            shielded = query_shielded_balance(db, &z_notes);
            speed_bal = query_speed_balance(db);

            /* RPC fallback if SQLite has no balance */
            if (balance == 0 && shielded == 0) {
                int64_t rpc_t = 0, rpc_z = 0;
                if (query_node_balance(&rpc_t, &rpc_z)) {
                    balance = rpc_t;
                    shielded = rpc_z;
                }
            }
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
    size_t off = emit_header(r, max, "Review Send", "/wallet/send");

    char address[128] = "", amount_str[32] = "";
    parse_form_field(body, body_len, "address", address, sizeof(address));
    parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));

    /* Validate address: must be alphanumeric with valid ZCL prefix */
    size_t alen = strlen(address);
    bool addr_ok = alen >= 26 && alen <= 96;
    for (size_t i = 0; addr_ok && address[i]; i++)
        if (!((address[i]>='a'&&address[i]<='z') || (address[i]>='A'&&address[i]<='Z') ||
              (address[i]>='0'&&address[i]<='9')))
            addr_ok = false;
    /* Require valid ZCL prefix */
    if (addr_ok) {
        bool has_prefix = (address[0] == 't' && (address[1] == '1' || address[1] == '3'))
                       || (alen >= 3 && address[0] == 'z' && address[1] == 's' && address[2] == '1');
        if (!has_prefix) addr_ok = false;
    }

    double amount = strtod(amount_str, NULL);
    const char *err_reason = !addr_ok
        ? "Invalid address. ZClassic addresses start with t1, t3, or zs1."
        : "Invalid amount";
    if (!addr_ok || amount <= 0) {
        APPEND(off, r, max,
            "<div style='text-align:center;padding:32px'>"
            "<div style='font-size:48px;color:#f87171'>&#x2717;</div>"
            "<h2 style='color:#e5e7eb'>Invalid Transaction</h2>"
            "<p style='color:#6b7280'>%s</p>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div>",
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
        "<div class='card' style='border-left-color:%s;padding:20px'>"
        "<div style='text-align:center;margin-bottom:16px'>"
        "<div style='font-size:14px;color:#888'>Review Transaction</div>"
        "</div>"
        "<table style='width:100%%;font-size:14px'>"
        "<tr><td style='color:#888;padding:8px 0'>To</td>"
        "<td style='color:#4db8ff;font-family:monospace;font-size:12px;"
        "word-break:break-all;text-align:right'>%s</td></tr>"
        "<tr><td style='color:#888;padding:8px 0'>Amount</td>"
        "<td style='color:#34d399;font-size:18px;font-weight:700;"
        "text-align:right'>%.8f ZCL</td></tr>"
        "<tr><td style='color:#888;padding:8px 0'>Fee</td>"
        "<td style='color:#6b7280;text-align:right'>%.4f ZCL</td></tr>"
        "<tr style='border-top:1px solid #333'>"
        "<td style='color:#888;padding:8px 0;font-weight:700'>Total deducted</td>"
        "<td style='color:#e5e7eb;font-weight:700;text-align:right'>"
        "%.8f ZCL</td></tr>"
        "<tr><td style='color:#888;padding:8px 0'>Remaining balance</td>"
        "<td style='color:#6b7280;text-align:right'>%.8f ZCL</td></tr>"
        "<tr><td style='color:#888;padding:8px 0'>Privacy</td>"
        "<td style='text-align:right'>"
        "<span class='pill %s'>%s</span></td></tr>"
        "</table></div>",
        is_shielded ? "#a78bfa" : "#33ff99",
        safe_addr, amount, fee, total_deducted, remaining,
        is_shielded ? "pill-z" : "pill-t",
        is_shielded ? "Private (shielded)" : "Public (transparent)");

    /* Cancel / Confirm buttons */
    APPEND(off, r, max,
        "<div style='display:flex;gap:10px;margin:16px 0'>"
        "<a href='/wallet/send' style='flex:1;background:#333;color:#e8e8e8;"
        "padding:12px;border-radius:6px;text-align:center;"
        "font-weight:700;text-decoration:none;font-size:16px'>Cancel</a>"
        "<form method='POST' action='zcl://node/wallet/send/confirm' "
        "style='flex:2;margin:0'>"
        "<input type='hidden' name='address' value='%s'>"
        "<input type='hidden' name='amount' value='%.8f'>"
        "<button type='submit' style='background:%s;color:%s;"
        "border:none;padding:12px;border-radius:6px;font-size:16px;"
        "font-weight:700;cursor:pointer;width:100%%'"
        " onclick='this.disabled=true;this.form.submit()'>"
        "Confirm Send</button></form></div>",
        safe_addr, amount,
        is_shielded ? "#a78bfa" : "#34d399",
        is_shielded ? "#fff" : "#0a0a0a");

    emit_footer(r, max, &off);
    return off;
}

/* ── Send Confirm (/wallet/send/confirm POST) ──────────────── */

static size_t serve_send_confirm(uint8_t *r, size_t max,
                                  const uint8_t *body, size_t body_len) {
    size_t off = emit_header(r, max, "Sending...", "/wallet/send");

    char address[128] = "", amount_str[32] = "";
    parse_form_field(body, body_len, "address", address, sizeof(address));
    parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));

    /* Validate address: alphanumeric with valid ZCL prefix */
    size_t alen = strlen(address);
    bool addr_ok = alen >= 26 && alen <= 96;
    for (size_t i = 0; addr_ok && address[i]; i++)
        if (!((address[i]>='a'&&address[i]<='z') || (address[i]>='A'&&address[i]<='Z') ||
              (address[i]>='0'&&address[i]<='9')))
            addr_ok = false;
    if (addr_ok) {
        bool has_prefix = (address[0] == 't' && (address[1] == '1' || address[1] == '3'))
                       || (alen >= 3 && address[0] == 'z' && address[1] == 's' && address[2] == '1');
        if (!has_prefix) addr_ok = false;
    }

    double amount = strtod(amount_str, NULL);
    if (!addr_ok || amount <= 0) {
        APPEND(off, r, max,
            "<div style='text-align:center;padding:32px'>"
            "<div style='font-size:48px;color:#f87171'>&#x2717;</div>"
            "<h2 style='color:#e5e7eb'>Invalid Transaction</h2>"
            "<p style='color:#6b7280'>%s</p>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div>",
            !addr_ok ? "Invalid address" : "Invalid amount");
        emit_footer(r, max, &off);
        return off;
    }

    /* Determine send type and make RPC call */
    char rpc_buf[4096] = "";
    char params[512];
    bool is_shielded = (strncmp(address, "zs1", 3) == 0);

    if (is_shielded) {
        snprintf(params, sizeof(params),
            "[\"" PRIMARY_ADDR "\", [{\"address\":\"%s\",\"amount\":%.8f}], 1, %.4f]",
            address, amount, FEE_ZCL);
        wallet_rpc_call_port("z_sendmany", params, rpc_buf, sizeof(rpc_buf), 18232, NULL);
    } else {
        snprintf(params, sizeof(params), "[\"%s\", %.8f]", address, amount);
        wallet_rpc_call_port("sendtoaddress", params, rpc_buf, sizeof(rpc_buf), 18232, NULL);
    }

    /* Parse result */
    char result_val[128] = "";
    char error_msg[256] = "";
    zcl_json_extract_str(rpc_buf, "result", result_val, sizeof(result_val));

    /* Check for error */
    const char *err_ptr = strstr(rpc_buf, "\"error\":");
    bool has_error = false;
    if (err_ptr) {
        const char *msg = strstr(err_ptr, "\"message\":");
        if (msg) {
            zcl_json_extract_str(err_ptr, "message", error_msg, sizeof(error_msg));
            if (error_msg[0]) has_error = true;
        }
    }

    if (!rpc_buf[0]) {
        /* Node offline */
        APPEND(off, r, max,
            "<div style='text-align:center;padding:32px'>"
            "<div style='font-size:48px;color:#fbbf24'>&#x26A0;</div>"
            "<h2 style='color:#e5e7eb'>Node Offline</h2>"
            "<p style='color:#6b7280'>Cannot reach the node on port 18232.</p>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div>");
    } else if (has_error) {
        char safe_err[512];
        html_escape(safe_err, sizeof(safe_err), error_msg);
        APPEND(off, r, max,
            "<div style='text-align:center;padding:32px'>"
            "<div style='font-size:48px;color:#f87171'>&#x2717;</div>"
            "<h2 style='color:#e5e7eb'>Send Failed</h2>"
            "<p style='color:#6b7280'>%s</p>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div>", safe_err);
    } else if (result_val[0]) {
        char safe_addr[256], safe_txid[256];
        html_escape(safe_addr, sizeof(safe_addr), address);
        html_escape(safe_txid, sizeof(safe_txid), result_val);

        bool is_opid = (strncmp(result_val, "opid-", 5) == 0);

        APPEND(off, r, max,
            "<div style='text-align:center;padding:32px'>"
            "<div style='font-size:48px;color:#34d399'>&#x2713;</div>"
            "<h2 style='color:#e5e7eb'>%s</h2>"
            "<p style='color:#6b7280'>%.8f ZCL to %s</p>",
            is_opid ? "Shielded Send Initiated" : "Transaction Sent",
            amount, safe_addr);

        if (is_opid) {
            APPEND(off, r, max,
                "<p style='color:#a78bfa;font-family:monospace;font-size:12px;"
                "word-break:break-all'>%s</p>"
                "<p style='color:#6b7280;font-size:13px'>"
                "Shielded transactions take ~6 hours to confirm.</p>",
                safe_txid);
        } else {
            APPEND(off, r, max,
                "<a href='/explorer/tx/%s' style='color:#60a5fa;"
                "font-family:monospace;font-size:12px;"
                "word-break:break-all'>%s</a>",
                safe_txid, safe_txid);
        }

        APPEND(off, r, max,
            "<div style='margin-top:24px'>"
            "<a href='/wallet' style='color:#34d399;font-size:16px'>"
            "Back to Wallet</a></div></div>");
    } else {
        APPEND(off, r, max,
            "<div style='text-align:center;padding:32px'>"
            "<div style='font-size:48px;color:#fbbf24'>&#x26A0;</div>"
            "<h2 style='color:#e5e7eb'>Unknown Response</h2>"
            "<a href='/wallet/send' style='color:#34d399'>Try Again</a>"
            "</div>");
    }

    emit_footer(r, max, &off);
    return off;
}

/* ── Router ─────────────────────────────────────────────────── */

void wallet_view_init(const char *datadir) {
    g_datadir = datadir;
}

size_t wallet_view_handle_request(const char *method, const char *path,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *response, size_t response_max)
{
    (void)method;
    if (!path || !response || response_max == 0) return 0;

    /* JSON pulse endpoint — polled every 2s by dashboard JS */
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
    if (strncmp(path, "/wallet/shield/confirm", 22) == 0) {
        const char *q = strchr(path, '?');
        return serve_shield_confirm(response, response_max, q);
    }
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
        return serve_history(response, response_max, page);
    }
    if (strcmp(path, "/wallet/coins") == 0)
        return serve_coins(response, response_max);

    return 0;
}
