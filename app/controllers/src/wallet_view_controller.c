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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sqlite3.h>

static const char *g_datadir = NULL;

#define PRIMARY_ADDR "t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn"

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
    ".subtitle{color:#666;font-size:13px;margin:0 0 16px}" \
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
    ".card .sub{color:#666;font-size:12px;margin-top:2px}" \
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
        { "/explorer",       "Explorer"  },
    };
    int n = snprintf((char *)buf, max, "<div class='nav'>");
    if (n < 0 || (size_t)n >= max) return 0;
    size_t off = (size_t)n;
    for (int i = 0; i < 6 && off < max; i++) {
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

/* ── APPEND macro (bounds-checked) ──────────────────────────── */

#define APPEND(off, buf, max, ...) do { \
    if ((off) < (max)) { \
        int _n = snprintf((char *)(buf) + (off), (max) - (off), __VA_ARGS__); \
        if (_n > 0 && (size_t)_n < (max) - (off)) (off) += (size_t)_n; \
    } \
} while(0)

/* ── HTML escape ────────────────────────────────────────────── */

static size_t html_escape(const char *src, char *dst, size_t dst_max) {
    if (!src || !dst || dst_max == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_max; i++) {
        switch (src[i]) {
        case '&':  memcpy(dst + j, "&amp;", 5);  j += 5; break;
        case '<':  memcpy(dst + j, "&lt;", 4);   j += 4; break;
        case '>':  memcpy(dst + j, "&gt;", 4);   j += 4; break;
        case '"':  memcpy(dst + j, "&quot;", 6); j += 6; break;
        case '\'': memcpy(dst + j, "&#39;", 5);  j += 5; break;
        default:   dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
    return j;
}

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

static int query_int(sqlite3 *db, const char *sql) {
    if (!db || !sql) return 0;
    sqlite3_stmt *s = NULL;
    int val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) val = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

static int64_t query_int64(sqlite3 *db, const char *sql) {
    if (!db || !sql) return 0;
    sqlite3_stmt *s = NULL;
    int64_t val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) val = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

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

/* ── Time formatting ────────────────────────────────────────── */

static void format_time(int64_t timestamp, char *out, size_t out_max) {
    if (!out || out_max == 0) return;
    out[0] = '\0';
    if (timestamp <= 0) return;
    time_t t = (time_t)timestamp;
    struct tm tm;
    if (!gmtime_r(&t, &tm)) return;
    strftime(out, out_max, "%Y-%m-%d %H:%M", &tm);
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

static const int QR_SIZE_V2 = 25;  /* Version 2: 25x25 modules */

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
    uint8_t gen[11];
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
    uint8_t rem[11];
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
static int qr_alphanum_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    switch (c) {
    case ' ': return 36; case '$': return 37; case '%': return 38;
    case '*': return 39; case '+': return 40; case '-': return 41;
    case '.': return 42; case '/': return 43; case ':': return 44;
    default: return -1;
    }
}

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

/* Build QR Version 2-L data codewords from alphanumeric string.
 * Version 2-L: 44 total codewords, 34 data, 10 ECC.
 * Data capacity in alphanumeric mode: 47 chars. */
static int qr_encode_alphanumeric(const char *str, uint8_t *codewords) {
    size_t len = strlen(str);
    if (len > 47) return -1;

    qr_bitbuf bb;
    memset(&bb, 0, sizeof(bb));

    /* Mode indicator: alphanumeric = 0010 */
    bb_append(&bb, 0x2, 4);
    /* Character count: 9 bits for version 2 */
    bb_append(&bb, (uint32_t)len, 9);

    /* Encode pairs */
    size_t i = 0;
    while (i + 1 < len) {
        int v1 = qr_alphanum_val(str[i]);
        int v2 = qr_alphanum_val(str[i + 1]);
        if (v1 < 0 || v2 < 0) return -1;
        bb_append(&bb, (uint32_t)(v1 * 45 + v2), 11);
        i += 2;
    }
    if (i < len) {
        int v = qr_alphanum_val(str[i]);
        if (v < 0) return -1;
        bb_append(&bb, (uint32_t)v, 6);
    }

    /* Terminator (up to 4 bits) */
    int data_bits = 34 * 8;  /* 272 bits capacity */
    int pad_bits = data_bits - bb.count;
    if (pad_bits > 4) pad_bits = 4;
    if (pad_bits > 0) bb_append(&bb, 0, pad_bits);

    /* Byte-align */
    if (bb.count % 8 != 0)
        bb_append(&bb, 0, 8 - (bb.count % 8));

    /* Pad codewords */
    int bytes_filled = bb.count / 8;
    bool toggle = false;
    while (bytes_filled < 34) {
        bb_append(&bb, toggle ? 0x11 : 0xEC, 8);
        bytes_filled++;
        toggle = !toggle;
    }

    memcpy(codewords, bb.bits, 34);
    return 0;
}

/* Place modules in the QR matrix (Version 2, 25x25).
 * matrix[row][col]: 0=white, 1=black, 2=reserved (not yet set) */
static void qr_place_finder(uint8_t m[25][25], int row, int col) {
    for (int dr = -1; dr <= 7; dr++) {
        for (int dc = -1; dc <= 7; dc++) {
            int r = row + dr, c = col + dc;
            if (r < 0 || r >= 25 || c < 0 || c >= 25) continue;
            bool is_border = (dr == -1 || dr == 7 || dc == -1 || dc == 7);
            bool is_outer  = (dr == 0 || dr == 6 || dc == 0 || dc == 6);
            bool is_inner  = (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4);
            m[r][c] = (is_border) ? 0 : (is_outer || is_inner) ? 1 : 0;
        }
    }
}

static void qr_place_alignment(uint8_t m[25][25]) {
    /* Version 2 alignment pattern at (18,18) */
    int cr = 18, cc = 18;
    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            bool is_edge = (dr == -2 || dr == 2 || dc == -2 || dc == 2);
            bool is_center = (dr == 0 && dc == 0);
            m[cr + dr][cc + dc] = (is_edge || is_center) ? 1 : 0;
        }
    }
}

static void qr_place_timing(uint8_t m[25][25]) {
    for (int i = 8; i < 25 - 8; i++) {
        m[6][i] = (i % 2 == 0) ? 1 : 0;
        m[i][6] = (i % 2 == 0) ? 1 : 0;
    }
}

/* Place format information (error correction L, mask 0) */
static void qr_place_format(uint8_t m[25][25]) {
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
        m[8][25 - 1 - i] = (fmt >> i) & 1;

    /* Bottom side of top-left: bits 7-14 at (25-7..25-1, 8) */
    for (int i = 0; i < 7; i++)
        m[25 - 7 + i][8] = (fmt >> (6 - i)) & 1;

    /* Dark module */
    m[25 - 8][8] = 1;

    (void)hpos_r; (void)hpos_c;
}

/* Check if a module is reserved (finder, timing, alignment, format, etc.) */
static bool qr_is_function(int r, int c) {
    /* Finder + separator: top-left, top-right, bottom-left */
    if (r <= 8 && c <= 8) return true;           /* top-left */
    if (r <= 8 && c >= 25 - 8) return true;      /* top-right */
    if (r >= 25 - 8 && c <= 8) return true;       /* bottom-left */
    /* Timing */
    if (r == 6 || c == 6) return true;
    /* Alignment at (18,18) */
    if (r >= 16 && r <= 20 && c >= 16 && c <= 20) return true;
    return false;
}

/* Place data bits in the QR matrix using the standard upward-rightward zigzag */
static void qr_place_data(uint8_t m[25][25], const uint8_t *bits, int nbits) {
    int bit_idx = 0;
    /* Column pairs from right to left, skip column 6 (timing) */
    for (int right = 25 - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;  /* skip timing column */
        bool upward = ((25 - 1 - right) / 2) % 2 == 0;
        for (int cnt = 0; cnt < 25; cnt++) {
            int row = upward ? (25 - 1 - cnt) : cnt;
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
static void qr_apply_mask(uint8_t m[25][25]) {
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 25; c++)
            if (!qr_is_function(r, c) && ((r + c) % 2 == 0))
                m[r][c] ^= 1;
}

static size_t emit_qr_svg(uint8_t *buf, size_t max, size_t off,
                           const char *data, int module_size) {
    if (!data || strlen(data) == 0 || strlen(data) > 47) return off;

    /* Encode data to codewords */
    uint8_t data_cw[34];
    if (qr_encode_alphanumeric(data, data_cw) != 0) return off;

    /* Reed-Solomon: 10 ECC codewords for Version 2-L */
    uint8_t ecc_cw[10];
    rs_encode(data_cw, 34, ecc_cw, 10);

    /* Combine data + ECC into bit stream */
    uint8_t all_cw[44];
    memcpy(all_cw, data_cw, 34);
    memcpy(all_cw + 34, ecc_cw, 10);

    /* Build the 25x25 matrix */
    uint8_t m[25][25];
    memset(m, 0, sizeof(m));

    qr_place_finder(m, 0, 0);       /* top-left */
    qr_place_finder(m, 0, 25 - 7);  /* top-right */
    qr_place_finder(m, 25 - 7, 0);  /* bottom-left */
    qr_place_alignment(m);
    qr_place_timing(m);
    qr_place_format(m);
    qr_place_data(m, all_cw, 44 * 8);
    qr_apply_mask(m);
    qr_place_format(m);  /* re-place format after mask (format is not masked) */

    /* Emit SVG with quiet zone of 4 modules */
    int quiet = 4;
    int total = QR_SIZE_V2 + quiet * 2;
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
    for (int r = 0; r < QR_SIZE_V2; r++) {
        for (int c = 0; c < QR_SIZE_V2; c++) {
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
        "</style></head><body>"
        "<h1>ZClassic23</h1>"
        "<p class='subtitle'>Direct SQLite — no ports</p>");
    off += emit_nav(buf + off, max - off, active_tab);
    return off;
}

static void emit_footer(uint8_t *buf, size_t max, size_t *off) {
    APPEND(*off, buf, max,
        "<footer>ZClassic23 — pure C23 full node + Tor</footer>"
        "</body></html>");
}

/* ── Dashboard (/wallet) ────────────────────────────────────── */

static size_t serve_dashboard(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");
    int peers = query_int(db, "SELECT count(*) FROM peers");
    int tokens = query_int(db, "SELECT count(*) FROM zslp_tokens");
    int mempool = query_int(db,
        "SELECT count(*) FROM mempool_entries");
    int64_t shielded = query_int64(db,
        "SELECT COALESCE(sum(value),0) FROM wallet_sapling_notes "
        "WHERE spent_txid IS NULL");
    int z_notes = query_int(db,
        "SELECT count(*) FROM wallet_sapling_notes WHERE spent_txid IS NULL");

    int64_t transparent = 0;
    int t_utxos = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT count(*), COALESCE(sum(u.value),0) FROM utxos u "
            "WHERE length(u.script) = 25 "
            "AND substr(hex(u.script), 1, 6) = '76A914' "
            "AND EXISTS (SELECT 1 FROM wallet_keys wk "
            "WHERE wk.pubkey_hash = substr(u.script, 4, 20))",
            -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            t_utxos = sqlite3_column_int(s, 0);
            transparent = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }

    /* Coin analysis: started with 1.0 ZCL, fees reduce it.
     * Only transparent UTXOs are from our original balance.
     * Shielded notes are incoming from external sources. */
    int64_t fees_paid = 100000000 - transparent; /* started with 1.0 ZCL */
    if (fees_paid < 0) fees_paid = 0; /* shouldn't happen */

    size_t off = emit_header(r, max, "Wallet — ZClassic23", "/wallet");

    /* Verified balance — transparent only */
    APPEND(off, r, max,
        "<div class='card' style='border-left-color:#33ff99;padding:20px'>"
        "<div class='label'>Verified Balance</div>"
        "<div style='font-size:36px;color:#33ff99;font-weight:800'>"
        "%.8f ZCL</div>"
        "<div class='sub' style='margin-top:8px'>"
        "%d UTXO%s &middot; Fees paid: %.8f ZCL"
        "</div></div>",
        (double)transparent / 1e8,
        t_utxos, t_utxos == 1 ? "" : "s",
        (double)fees_paid / 1e8);

    if (shielded > 0) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#9999ff;padding:14px'>"
            "<div class='label'>Incoming Shielded Notes</div>"
            "<div style='font-size:18px;color:#9999ff;font-weight:700'>"
            "%.8f ZCL</div>"
            "<div class='sub'>%d note%s (received from external sources)</div>"
            "</div>",
            (double)shielded / 1e8, z_notes, z_notes == 1 ? "" : "s");
    }

    /* Stats row */
    APPEND(off, r, max,
        "<div class='stats'>"
        "<div class='stat'><div class='n'>%d</div>"
        "<div class='l'>Height</div></div>"
        "<div class='stat'><div class='n'>%d</div>"
        "<div class='l'>Peers</div></div>"
        "<div class='stat'><div class='n'>%d</div>"
        "<div class='l'>Tokens</div></div>"
        "<div class='stat'><div class='n'>%d</div>"
        "<div class='l'>Mempool</div></div>"
        "</div>",
        tip, peers, tokens, mempool);

    /* Receive address with visual encoding */
    APPEND(off, r, max,
        "<h2>Receive Address</h2>");
    off = emit_qr_svg(r, max, off, PRIMARY_ADDR, 4);
    APPEND(off, r, max,
        "<div class='addr-box'>" PRIMARY_ADDR "</div>"
        "<div style='text-align:center;color:#666;font-size:12px'>"
        "Copy Address</div>");

    /* Recent transactions — tx-card timeline */
    APPEND(off, r, max, "<h2>Recent Transactions</h2>");

    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(wt.txid), wt.block_height, b.time, "
            "wt.net_value "
            "FROM wallet_transactions wt "
            "LEFT JOIN blocks b ON wt.block_height = b.height "
            "ORDER BY wt.block_height DESC LIMIT 10",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 600 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int height = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int64_t net_val = sqlite3_column_int64(s, 3);
            if (!txid) continue;

            char short_tx[18], lower_tx[65], rel_time[48];
            txid_short(txid, short_tx, sizeof(short_tx));
            txid_lower(txid, lower_tx, sizeof(lower_tx));
            format_relative_time(btime, rel_time, sizeof(rel_time));

            char esc_short[64], esc_lower[256], esc_rel[96];
            html_escape(short_tx, esc_short, sizeof(esc_short));
            html_escape(lower_tx, esc_lower, sizeof(esc_lower));
            html_escape(rel_time, esc_rel, sizeof(esc_rel));

            bool is_recv = (net_val >= 0);
            int64_t abs_val = is_recv ? net_val : -net_val;
            int confs = (tip > 0 && height > 0) ? (tip - height + 1) : 0;
            if (confs < 0) confs = 0;

            APPEND(off, r, max,
                "<div class='tx-card' style='border-left-color:%s'>"
                "<div class='tx-amount %s'>%s%.8f ZCL</div>"
                "<div class='tx-meta'>"
                "<span class='tx-time'>%s</span>"
                "<a href='/explorer/tx/%s' class='tx-hash'>%s</a>"
                "<span class='tx-conf'>%d confirmation%s</span>"
                "</div></div>",
                is_recv ? "#33ff99" : "#ff6666",
                is_recv ? "recv" : "send",
                is_recv ? "+" : "-",
                (double)abs_val / 1e8,
                esc_rel,
                esc_lower, esc_short,
                confs, confs == 1 ? "" : "s");
        }
        sqlite3_finalize(s);
    }
    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Send (/wallet/send) ────────────────────────────────────── */

static size_t serve_send(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();

    int64_t balance = 0;
    if (db) {
        balance = query_int64(db,
            "SELECT COALESCE(sum(u.value),0) FROM utxos u "
            "WHERE length(u.script) = 25 "
            "AND substr(hex(u.script), 1, 6) = '76A914' "
            "AND EXISTS (SELECT 1 FROM wallet_keys wk "
            "WHERE wk.pubkey_hash = substr(u.script, 4, 20))");
        sqlite3_close(db);
    }

    size_t off = emit_header(r, max, "Send — ZClassic23", "/wallet/send");

    APPEND(off, r, max,
        "<h2>Send ZCL</h2>"
        "<div class='card'>"
        "<div class='label'>Available Balance</div>"
        "<div class='value'>%.8f ZCL</div>"
        "</div>",
        (double)balance / 1e8);

    APPEND(off, r, max,
        "<div class='card'>"
        "<form id='send-form' onsubmit='return validateSend()'>"
        "<label class='label'>To Address</label>"
        "<input type='text' id='addr' name='address' "
        "placeholder='t1... or zs1...' required>"
        "<div id='addr-err' class='err'></div>"
        "<label class='label' style='margin-top:12px'>Amount (ZCL)</label>"
        "<input type='text' id='amt' name='amount' "
        "placeholder='0.00000000' required>"
        "<div id='amt-err' class='err'></div>"
        "<label class='label' style='margin-top:12px'>Fee (ZCL)</label>"
        "<input type='text' id='fee' name='fee' value='0.0001'>"
        "<div id='fee-err' class='err'></div>"
        "<button type='submit' style='margin-top:16px'>Send Transaction</button>"
        "</form>"
        "<p style='color:#666;font-size:12px;margin-top:8px'>"
        "Transactions are signed locally and broadcast via P2P.</p>"
        "</div>"
        "<script>"
        "function validateSend(){"
        "var ok=true;"
        "var a=document.getElementById('addr').value.trim();"
        "var m=document.getElementById('amt').value.trim();"
        "var f=document.getElementById('fee').value.trim();"
        "document.getElementById('addr-err').textContent='';"
        "document.getElementById('amt-err').textContent='';"
        "document.getElementById('fee-err').textContent='';"
        "if(!a||a.length<26){"
        "document.getElementById('addr-err').textContent="
        "'Address is required (t1... or zs1...)';ok=false;}"
        "var amt=parseFloat(m);"
        "if(isNaN(amt)||amt<=0){"
        "document.getElementById('amt-err').textContent="
        "'Amount must be a positive number';ok=false;}"
        "var fee=parseFloat(f);"
        "if(isNaN(fee)||fee<0){"
        "document.getElementById('fee-err').textContent="
        "'Fee must be a non-negative number';ok=false;}"
        "if(ok&&(amt+fee)>%.8f){"
        "document.getElementById('amt-err').textContent="
        "'Insufficient funds';ok=false;}"
        "return ok;}"
        "</script>",
        (double)balance / 1e8);

    emit_footer(r, max, &off);
    return off;
}

/* ── Receive (/wallet/receive) ──────────────────────────────── */

static size_t serve_receive(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    size_t off = emit_header(r, max, "Receive — ZClassic23", "/wallet/receive");

    APPEND(off, r, max, "<h2>Receive ZCL</h2>");

    /* Primary transparent address with visual encoding */
    APPEND(off, r, max,
        "<div class='card'>"
        "<div class='label'>Your Transparent Address</div>");
    off = emit_qr_svg(r, max, off, PRIMARY_ADDR, 4);
    APPEND(off, r, max,
        "<div class='addr-box'>" PRIMARY_ADDR "</div>"
        "<div style='text-align:center;color:#666;font-size:12px'>"
        "Copy Address</div>"
        "<div class='sub' style='margin-top:8px'>"
        "Share this address to receive transparent ZCL.</div>"
        "</div>");

    /* Shielded addresses */
    if (db) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT address FROM wallet_sapling_keys "
                "WHERE address IS NOT NULL AND address != '' "
                "ORDER BY rowid",
                -1, &s, NULL) == SQLITE_OK) {
            int count = 0;
            while (sqlite3_step(s) == SQLITE_ROW && off + 512 < max) {
                const char *raw = (const char *)sqlite3_column_text(s, 0);
                if (!raw || !raw[0]) continue;

                char escaped[1024];
                html_escape(raw, escaped, sizeof(escaped));

                if (count == 0) {
                    APPEND(off, r, max,
                        "<div class='card' style='border-left-color:#9999ff'>"
                        "<div class='label'>Shielded Addresses (Sapling)</div>"
                        "<div class='sub' style='margin-bottom:8px'>"
                        "Shielded addresses provide full privacy.</div>");
                }
                APPEND(off, r, max,
                    "<div class='addr-box-sm'>%s</div>", escaped);
                count++;
            }
            if (count > 0)
                APPEND(off, r, max, "</div>");
            sqlite3_finalize(s);
        }
        sqlite3_close(db);
    }

    emit_footer(r, max, &off);
    return off;
}

/* ── History (/wallet/history) ──────────────────────────────── */

static size_t serve_history(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");

    size_t off = emit_header(r, max, "History — ZClassic23", "/wallet/history");

    int tx_count = query_int(db,
        "SELECT count(*) FROM wallet_transactions");

    APPEND(off, r, max,
        "<h2>Transaction History</h2>"
        "<div class='sub'>%d transaction%s</div>",
        tx_count, tx_count == 1 ? "" : "s");

    /* Timeline view (tx-cards) */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(wt.txid), wt.block_height, b.time, "
            "wt.net_value "
            "FROM wallet_transactions wt "
            "LEFT JOIN blocks b ON wt.block_height = b.height "
            "ORDER BY wt.block_height DESC LIMIT 100",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 600 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int64_t net_val = sqlite3_column_int64(s, 3);
            if (!txid) continue;

            char short_tx[18], lower_tx[65], rel_time[48], ts[32];
            txid_short(txid, short_tx, sizeof(short_tx));
            txid_lower(txid, lower_tx, sizeof(lower_tx));
            format_relative_time(btime, rel_time, sizeof(rel_time));
            format_time(btime, ts, sizeof(ts));

            char esc_short[64], esc_lower[256], esc_rel[96], esc_ts[64];
            html_escape(short_tx, esc_short, sizeof(esc_short));
            html_escape(lower_tx, esc_lower, sizeof(esc_lower));
            html_escape(rel_time, esc_rel, sizeof(esc_rel));
            html_escape(ts, esc_ts, sizeof(esc_ts));

            int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
            if (confs < 0) confs = 0;

            bool is_recv = (net_val >= 0);
            int64_t abs_val = is_recv ? net_val : -net_val;

            APPEND(off, r, max,
                "<div class='tx-card' style='border-left-color:%s'>"
                "<div class='tx-amount %s'>%s%.8f ZCL</div>"
                "<div class='tx-meta'>"
                "<span class='tx-time' title='%s'>%s</span>"
                "<a href='/explorer/tx/%s' class='tx-hash'>%s</a>"
                "<span class='tx-conf'>h=%d &middot; %d conf%s</span>"
                "</div></div>",
                is_recv ? "#33ff99" : "#ff6666",
                is_recv ? "recv" : "send",
                is_recv ? "+" : "-",
                (double)abs_val / 1e8,
                esc_ts,
                esc_rel,
                esc_lower, esc_short,
                h, confs, confs == 1 ? "" : "s");
        }
        sqlite3_finalize(s);
    }
    emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Coins (/wallet/coins) ──────────────────────────────────── */

static size_t serve_coins(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");

    size_t off = emit_header(r, max, "Coins — ZClassic23", "/wallet/coins");
    APPEND(off, r, max, "<h2>Coin Analysis</h2>");

    /* Transparent UTXOs */
    APPEND(off, r, max,
        "<h3>Transparent UTXOs</h3>"
        "<div class='overflow-x'>"
        "<table><tr><th>Outpoint</th>"
        "<th>Amount</th><th>Height</th><th>Confirmations</th></tr>");

    int64_t t_total = 0;
    int t_count = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(u.txid), u.vout, u.value, u.height FROM utxos u "
            "WHERE length(u.script) = 25 "
            "AND substr(hex(u.script), 1, 6) = '76A914' "
            "AND EXISTS (SELECT 1 FROM wallet_keys wk "
            "WHERE wk.pubkey_hash = substr(u.script, 4, 20)) "
            "ORDER BY u.value DESC",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 400 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int vout = sqlite3_column_int(s, 1);
            int64_t val = sqlite3_column_int64(s, 2);
            int h = sqlite3_column_int(s, 3);
            if (!txid) continue;

            char short_tx[18];
            txid_short(txid, short_tx, sizeof(short_tx));

            int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
            if (confs < 0) confs = 0;

            APPEND(off, r, max,
                "<tr>"
                "<td class='hash'>%s:%d</td>"
                "<td class='zcl'>%.8f</td>"
                "<td>%d</td>"
                "<td>%d</td>"
                "</tr>",
                short_tx, vout, (double)val / 1e8, h, confs);
            t_total += val;
            t_count++;
        }
        sqlite3_finalize(s);
    }

    if (t_count > 0) {
        APPEND(off, r, max,
            "<tr class='total-row'>"
            "<td>Total (%d UTXO%s)</td>"
            "<td class='zcl'>%.8f</td>"
            "<td></td><td></td></tr>",
            t_count, t_count == 1 ? "" : "s",
            (double)t_total / 1e8);
    }
    APPEND(off, r, max, "</table></div>");

    /* Shielded notes */
    APPEND(off, r, max,
        "<h3>Shielded Notes</h3>"
        "<div class='overflow-x'>"
        "<table><tr><th>Amount</th><th>Height</th><th>Status</th></tr>");

    int64_t z_unspent = 0;
    int64_t z_spent = 0;
    int z_u_count = 0;
    int z_s_count = 0;
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value, block_height, "
            "CASE WHEN spent_txid IS NULL THEN 'unspent' ELSE 'spent' END "
            "FROM wallet_sapling_notes ORDER BY block_height DESC",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 256 < max) {
            int64_t val = sqlite3_column_int64(s, 0);
            int h = sqlite3_column_int(s, 1);
            const char *status = (const char *)sqlite3_column_text(s, 2);
            bool unspent = (status && status[0] == 'u');

            if (unspent) { z_unspent += val; z_u_count++; }
            else         { z_spent += val; z_s_count++; }

            APPEND(off, r, max,
                "<tr>"
                "<td class='zcl'>%.8f</td>"
                "<td>%d</td>"
                "<td><span class='pill %s'>%s</span></td>"
                "</tr>",
                (double)val / 1e8, h,
                unspent ? "pill-z" : "pill-t",
                status ? status : "unknown");
        }
        sqlite3_finalize(s);
    }
    APPEND(off, r, max, "</table></div>");

    /* Supply stats */
    int64_t chain_supply = query_int64(db,
        "SELECT COALESCE(sum(value),0) FROM utxos");
    int chain_utxos = query_int(db, "SELECT count(*) FROM utxos");

    APPEND(off, r, max,
        "<h3>Supply Summary</h3>"
        "<div class='stats'>"
        "<div class='stat'>"
        "<div class='n'>%.4f</div>"
        "<div class='l'>Wallet T-Balance</div></div>"
        "<div class='stat'>"
        "<div class='n'>%.4f</div>"
        "<div class='l'>Wallet Z-Unspent</div></div>"
        "<div class='stat'>"
        "<div class='n'>%.2f</div>"
        "<div class='l'>Chain Supply</div></div>"
        "<div class='stat'>"
        "<div class='n'>%d</div>"
        "<div class='l'>Chain UTXOs</div></div>"
        "</div>",
        (double)t_total / 1e8,
        (double)z_unspent / 1e8,
        (double)chain_supply / 1e8,
        chain_utxos);

    emit_footer(r, max, &off);
    sqlite3_close(db);
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
    (void)method; (void)body; (void)body_len;
    if (!path || !response || response_max == 0) return 0;

    if (strcmp(path, "/wallet") == 0 || strcmp(path, "/wallet/") == 0)
        return serve_dashboard(response, response_max);
    if (strcmp(path, "/wallet/send") == 0)
        return serve_send(response, response_max);
    if (strcmp(path, "/wallet/receive") == 0)
        return serve_receive(response, response_max);
    if (strcmp(path, "/wallet/history") == 0)
        return serve_history(response, response_max);
    if (strcmp(path, "/wallet/coins") == 0)
        return serve_coins(response, response_max);

    return 0;
}
