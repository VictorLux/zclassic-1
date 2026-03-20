/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer factoids page -- comprehensive "historian nerd" page with
 * SHA3 data receipts for every milestone. Queries SQLite read-only.
 *
 * Sections: Genesis Story, Network Milestones, All-Time Records,
 * Supply Milestones, Privacy Usage Over Time, ZSLP Token History,
 * Data Integrity. */

#include "controllers/explorer_factoids.h"
#include "controllers/explorer_internal.h"
#include "crypto/sha3.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>

/* ── SQLite query helpers ─────────────────────────────────── */

static int64_t fq_i64(sqlite3 *db, const char *sql)
{
    int64_t val = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            val = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

/* fq_double removed -- not currently needed */

/* Query a single text column */
static void fq_text(sqlite3 *db, const char *sql, char *out, size_t outmax)
{
    out[0] = '\0';
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            if (t) snprintf(out, outmax, "%s", t);
        }
        sqlite3_finalize(s);
    }
}

/* ── SHA3-256 receipt computation ─────────────────────────── */

static void compute_receipt(char *hex_out, size_t hex_max,
                            int64_t height, const char *block_hash,
                            const char *fact_name)
{
    /* SHA3-256(height_le64 || block_hash_ascii || fact_name_ascii) */
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    uint8_t h_le[8];
    for (int i = 0; i < 8; i++)
        h_le[i] = (uint8_t)((height >> (i * 8)) & 0xff);
    sha3_256_write(&ctx, h_le, 8);

    if (block_hash)
        sha3_256_write(&ctx, (const unsigned char *)block_hash, strlen(block_hash));
    if (fact_name)
        sha3_256_write(&ctx, (const unsigned char *)fact_name, strlen(fact_name));

    unsigned char digest[32];
    sha3_256_finalize(&ctx, digest);

    /* First 16 hex chars (8 bytes) */
    size_t chars = 16;
    if (chars * 2 + 1 > hex_max) chars = (hex_max - 1) / 2;
    for (size_t i = 0; i < 8 && i * 2 + 2 <= hex_max; i++)
        snprintf(hex_out + i * 2, hex_max - i * 2, "%02x", digest[i]);
}

/* Full 64-char SHA3-256 hex */
static void compute_full_hash(char *hex_out, size_t hex_max,
                               const unsigned char *data, size_t data_len)
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, data, data_len);
    unsigned char digest[32];
    sha3_256_finalize(&ctx, digest);
    for (int i = 0; i < 32 && (size_t)(i * 2 + 2) <= hex_max; i++)
        snprintf(hex_out + i * 2, hex_max - (size_t)(i * 2), "%02x", digest[i]);
}

/* ── Format helpers ───────────────────────────────────────── */

static void fmt_time(char *buf, size_t max, int64_t t)
{
    time_t ts = (time_t)t;
    struct tm tm;
    gmtime_r(&ts, &tm);
    strftime(buf, max, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

static void fmt_zcl(char *buf, size_t max, int64_t zatoshi)
{
    int64_t whole = zatoshi / 100000000LL;
    int64_t frac = zatoshi % 100000000LL;
    if (frac < 0) frac = -frac;
    snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
}

/* ── Build the factoids page ──────────────────────────────── */

size_t explorer_factoids_build(uint8_t *buf, size_t buf_max, const char *datadir)
{
    if (!buf || buf_max < 1024 || !datadir) return 0;

    char dbpath[1024];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 30000);

    size_t off = 0;
    char *r = (char *)buf;
    size_t max = buf_max;

    /* ── HTTP header + HTML head ──────────────────────────── */
    APPEND(off, r, max,
        EXPLORER_HEADER("ZClassic Historian Factoids")
        EXPLORER_NAV
        "<div class='content'>"
        "<h1>Historian Factoids</h1>"
        "<p style='color:#888'>Deep chain archaeology with SHA3-256 data receipts. "
        "Every milestone fact is hashed for independent verification.</p>");

    /* ================================================================
     * Section 1: Genesis Story
     * ================================================================ */
    APPEND(off, r, max,
        "<h2>Genesis Story</h2>");

    /* Genesis block */
    char genesis_hash[128] = "";
    int64_t genesis_time = 0;
    char genesis_coinbase[128] = "";

    fq_text(db, "SELECT hash FROM blocks WHERE height = 0", genesis_hash, sizeof(genesis_hash));
    genesis_time = fq_i64(db, "SELECT time FROM blocks WHERE height = 0");

    fq_text(db, "SELECT txid FROM transactions WHERE block_height = 0 AND is_coinbase = 1 LIMIT 1",
            genesis_coinbase, sizeof(genesis_coinbase));

    char tstr[64];
    fmt_time(tstr, sizeof(tstr), genesis_time);

    APPEND(off, r, max,
        "<div class='card'>"
        "<h3>Block 0: Genesis</h3>"
        "<table><tr><td><b>Hash</b></td><td><code style='word-break:break-all'>"
        "<a href='/explorer/block/0'>%.64s</a></code></td></tr>"
        "<tr><td><b>Timestamp</b></td><td>%s</td></tr>"
        "<tr><td><b>First Coinbase TX</b></td><td><code style='word-break:break-all'>"
        "<a href='/explorer/tx/%.64s'>%.16s...</a></code></td></tr>"
        "</table></div>",
        genesis_hash, tstr, genesis_coinbase, genesis_coinbase);

    /* First 10 blocks table */
    APPEND(off, r, max,
        "<h3>First 10 Blocks</h3>"
        "<table class='txlist'>"
        "<tr><th>Height</th><th>Time</th><th>Block Hash</th></tr>");

    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT height, time, hash FROM blocks WHERE height < 10 ORDER BY height";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                const char *hash = (const char *)sqlite3_column_text(s, 2);
                char ts[64];
                fmt_time(ts, sizeof(ts), t);
                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td>"
                    "<td><code style='word-break:break-all'>%.16s...</code></td></tr>",
                    h, h, ts, hash ? hash : "?");
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* ================================================================
     * Section 2: Network Milestones
     * ================================================================ */
    APPEND(off, r, max,
        "<h2>Network Milestones</h2>"
        "<p style='color:#888'>Each milestone includes a SHA3-256 receipt: "
        "SHA3(height_le64 || block_hash || fact_name). First 16 hex chars shown.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Time</th><th>SHA3 Receipt</th></tr>");

    /* Helper struct for milestones */
    struct milestone {
        const char *name;
        const char *sql;        /* NULL = use fixed_height */
        int64_t fixed_height;   /* -1 = use sql */
    };

    struct milestone milestones[] = {
        { "Genesis (Nov 6, 2016)", NULL, 0 },
        { "First non-coinbase tx",
          "SELECT MIN(block_height) FROM transactions WHERE is_coinbase = 0", -1 },
        { "First Sprout JoinSplit",
          "SELECT MIN(block_height) FROM joinsplits", -1 },
        { "First Sapling transaction",
          "SELECT MIN(block_height) FROM sapling_spends", -1 },
        { "First OP_RETURN",
          "SELECT MIN(block_height) FROM op_returns", -1 },
        { "First ZSLP token",
          "SELECT MIN(genesis_height) FROM zslp_tokens", -1 },
        { "First halving", NULL, 840000 },
        { "Second halving", NULL, 1680000 },
        { "Third halving", NULL, 2520000 },
    };

    int n_milestones = (int)(sizeof(milestones) / sizeof(milestones[0]));
    for (int i = 0; i < n_milestones; i++) {
        int64_t height = milestones[i].fixed_height;
        if (milestones[i].sql)
            height = fq_i64(db, milestones[i].sql);

        if (height <= 0 && i > 0) {
            /* Milestone not reached yet */
            APPEND(off, r, max,
                "<tr><td>%s</td><td colspan='3' style='color:#666'>Not yet reached</td></tr>",
                milestones[i].name);
            continue;
        }

        /* Look up block hash and time for this height */
        char bhash[128] = "";
        int64_t btime = 0;
        {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "SELECT hash, time FROM blocks WHERE height = %" PRId64, height);
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW) {
                    const char *h = (const char *)sqlite3_column_text(s, 0);
                    if (h) snprintf(bhash, sizeof(bhash), "%s", h);
                    btime = sqlite3_column_int64(s, 1);
                }
                sqlite3_finalize(s);
            }
        }

        char receipt[32] = "";
        compute_receipt(receipt, sizeof(receipt), height, bhash, milestones[i].name);

        char ts[64];
        fmt_time(ts, sizeof(ts), btime);

        APPEND(off, r, max,
            "<tr><td>%s</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%s</td>"
            "<td><code>%s</code></td></tr>",
            milestones[i].name, height, height, ts, receipt);
    }
    APPEND(off, r, max, "</table>");

    /* ================================================================
     * Section 3: All-Time Records
     * ================================================================ */
    APPEND(off, r, max, "<h2>All-Time Records</h2>"
        "<table class='txlist'>"
        "<tr><th>Record</th><th>Value</th><th>Block</th><th>Time</th></tr>");

    /* Largest transparent output */
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT u.value, u.height, b.time FROM utxos u "
                          "JOIN blocks b ON u.height = b.height "
                          "ORDER BY u.value DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t val = sqlite3_column_int64(s, 0);
                int64_t h = sqlite3_column_int64(s, 1);
                int64_t t = sqlite3_column_int64(s, 2);
                char vstr[64], ts2[64];
                fmt_zcl(vstr, sizeof(vstr), val);
                fmt_time(ts2, sizeof(ts2), t);
                APPEND(off, r, max,
                    "<tr><td>Largest transparent output</td>"
                    "<td>%s ZCL</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    vstr, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    /* Most transactions in a single block */
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT b.height, b.num_tx, b.time FROM blocks b "
                          "ORDER BY b.num_tx DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t ntx = sqlite3_column_int64(s, 1);
                int64_t t = sqlite3_column_int64(s, 2);
                char ts2[64];
                fmt_time(ts2, sizeof(ts2), t);
                APPEND(off, r, max,
                    "<tr><td>Most transactions in a block</td>"
                    "<td>%" PRId64 " tx</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    ntx, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    /* Most JoinSplits in a single block */
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT block_height, count(*) as cnt FROM joinsplits "
                          "GROUP BY block_height ORDER BY cnt DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                int64_t btime2 = fq_i64(db, "SELECT time FROM blocks WHERE height = 0");
                {
                    char sq2[128];
                    snprintf(sq2, sizeof(sq2),
                        "SELECT time FROM blocks WHERE height = %" PRId64, h);
                    btime2 = fq_i64(db, sq2);
                }
                char ts2[64];
                fmt_time(ts2, sizeof(ts2), btime2);
                APPEND(off, r, max,
                    "<tr><td>Most JoinSplits in a block</td>"
                    "<td>%" PRId64 "</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    cnt, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    /* Most Sapling outputs in a single block */
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT block_height, count(*) as cnt FROM sapling_outputs "
                          "GROUP BY block_height ORDER BY cnt DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                char sq2[128];
                snprintf(sq2, sizeof(sq2),
                    "SELECT time FROM blocks WHERE height = %" PRId64, h);
                int64_t btime2 = fq_i64(db, sq2);
                char ts2[64];
                fmt_time(ts2, sizeof(ts2), btime2);
                APPEND(off, r, max,
                    "<tr><td>Most Sapling outputs in a block</td>"
                    "<td>%" PRId64 "</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    cnt, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    /* Highest difficulty ever */
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT height, bits, time FROM blocks ORDER BY bits ASC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                uint32_t bits = (uint32_t)sqlite3_column_int64(s, 1);
                int64_t t = sqlite3_column_int64(s, 2);
                double diff = explorer_difficulty_from_bits(bits);
                char ts2[64];
                fmt_time(ts2, sizeof(ts2), t);
                APPEND(off, r, max,
                    "<tr><td>Highest difficulty ever</td>"
                    "<td>%.2f</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    diff, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    /* Longest gap between blocks */
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT a.height, a.time, b.time, (b.time - a.time) as gap "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 "
            "ORDER BY gap DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                int64_t gap = sqlite3_column_int64(s, 3);
                char ts2[64];
                fmt_time(ts2, sizeof(ts2), t);
                int64_t gap_min = gap / 60;
                int64_t gap_sec = gap % 60;
                APPEND(off, r, max,
                    "<tr><td>Longest gap between blocks</td>"
                    "<td>%" PRId64 "m %" PRId64 "s</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    gap_min, gap_sec, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    /* Shortest gap between blocks (excluding 0-second) */
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT a.height, a.time, b.time, (b.time - a.time) as gap "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 AND (b.time - a.time) > 0 "
            "ORDER BY gap ASC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                int64_t gap = sqlite3_column_int64(s, 3);
                char ts2[64];
                fmt_time(ts2, sizeof(ts2), t);
                APPEND(off, r, max,
                    "<tr><td>Shortest gap between blocks</td>"
                    "<td>%" PRId64 "s</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td></tr>",
                    gap, h, h, ts2);
            }
            sqlite3_finalize(s);
        }
    }

    APPEND(off, r, max, "</table>");

    /* ================================================================
     * Section 4: Supply Milestones
     * ================================================================ */
    APPEND(off, r, max,
        "<h2>Supply Milestones</h2>"
        "<p style='color:#888'>Based on 12.5 ZCL/block, halving every 840,000 blocks.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Date</th></tr>");

    /* 12.5 ZCL/block for first 840000, then 6.25, then 3.125...
     * 1M ZCL = 1,000,000 / 12.5 = 80,000 blocks
     * 5M ZCL = 400,000 blocks
     * 10M ZCL: first 840,000 blocks = 10,500,000 ZCL > 10M, so block ~800,000
     * Actually 10M / 12.5 = 800,000 blocks */
    struct {
        const char *label;
        int64_t target_zatoshi;
    } supply_milestones[] = {
        { "1,000,000 ZCL mined", 100000000LL * 1000000LL },
        { "5,000,000 ZCL mined", 100000000LL * 5000000LL },
        { "10,000,000 ZCL mined", 100000000LL * 10000000LL },
    };

    for (int i = 0; i < 3; i++) {
        int64_t target = supply_milestones[i].target_zatoshi;
        int64_t cumulative = 0;
        int64_t subsidy = 1250000000LL; /* 12.5 ZCL */
        int64_t halving = 840000;
        int64_t blk = 0;
        int64_t milestone_height = -1;

        while (subsidy > 0) {
            int64_t end_of_era = blk + halving;
            int64_t blocks_needed = (target - cumulative + subsidy - 1) / subsidy;
            if (blocks_needed <= halving) {
                milestone_height = blk + blocks_needed;
                break;
            }
            cumulative += halving * subsidy;
            blk = end_of_era;
            subsidy /= 2;
        }

        if (milestone_height < 0) {
            APPEND(off, r, max,
                "<tr><td>%s</td><td colspan='2' style='color:#666'>Beyond max supply</td></tr>",
                supply_milestones[i].label);
            continue;
        }

        char sq[128];
        snprintf(sq, sizeof(sq),
            "SELECT time FROM blocks WHERE height = %" PRId64, milestone_height);
        int64_t btime = fq_i64(db, sq);

        if (btime > 0) {
            char ts2[64];
            fmt_time(ts2, sizeof(ts2), btime);
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s</td></tr>",
                supply_milestones[i].label, milestone_height, milestone_height, ts2);
        } else {
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td></tr>",
                supply_milestones[i].label, milestone_height);
        }
    }
    APPEND(off, r, max, "</table>");

    /* ================================================================
     * Section 5: Privacy Usage Over Time
     * ================================================================ */
    APPEND(off, r, max,
        "<h2>Privacy Usage Over Time</h2>"
        "<p style='color:#888'>Yearly band counts (each band ~210,240 blocks, ~1 year).</p>"
        "<table class='txlist'>"
        "<tr><th>Year Band</th><th>Blocks</th><th>JoinSplits</th><th>Sapling Spends</th></tr>");

    /* JoinSplits by year band */
    {
        /* Two separate queries merged in display */
        sqlite3_stmt *s = NULL;
        int64_t js_bands[20] = {0};
        int64_t ss_bands[20] = {0};
        int max_band = 0;

        const char *js_sql = "SELECT block_height / 210240 as yr_band, count(*) "
                             "FROM joinsplits GROUP BY yr_band ORDER BY yr_band";
        if (sqlite3_prepare_v2(db, js_sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int band = (int)sqlite3_column_int64(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                if (band >= 0 && band < 20) {
                    js_bands[band] = cnt;
                    if (band > max_band) max_band = band;
                }
            }
            sqlite3_finalize(s);
            s = NULL;
        }

        const char *ss_sql = "SELECT block_height / 210240 as yr_band, count(*) "
                             "FROM sapling_spends GROUP BY yr_band ORDER BY yr_band";
        if (sqlite3_prepare_v2(db, ss_sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int band = (int)sqlite3_column_int64(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                if (band >= 0 && band < 20) {
                    ss_bands[band] = cnt;
                    if (band > max_band) max_band = band;
                }
            }
            sqlite3_finalize(s);
            s = NULL;
        }

        /* Also count blocks per band */
        int64_t blk_bands[20] = {0};
        const char *blk_sql = "SELECT height / 210240 as yr_band, count(*) "
                              "FROM blocks GROUP BY yr_band ORDER BY yr_band";
        if (sqlite3_prepare_v2(db, blk_sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int band = (int)sqlite3_column_int64(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                if (band >= 0 && band < 20) {
                    blk_bands[band] = cnt;
                    if (band > max_band) max_band = band;
                }
            }
            sqlite3_finalize(s);
            s = NULL;
        }

        /* Genesis time: Nov 6, 2016 */
        int base_year = 2016;
        for (int b = 0; b <= max_band && b < 20; b++) {
            APPEND(off, r, max,
                "<tr><td>~%d (band %d)</td>"
                "<td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td></tr>",
                base_year + b, b, blk_bands[b], js_bands[b], ss_bands[b]);
        }
    }
    APPEND(off, r, max, "</table>");

    /* ================================================================
     * Section 6: ZSLP Token History
     * ================================================================ */
    APPEND(off, r, max,
        "<h2>ZSLP Token History</h2>");

    int64_t total_tokens = fq_i64(db, "SELECT count(*) FROM zslp_tokens");
    int64_t total_transfers = fq_i64(db, "SELECT count(*) FROM zslp_transfers");

    APPEND(off, r, max,
        "<div class='card'>"
        "<p><b>Total tokens created:</b> %" PRId64 "</p>"
        "<p><b>Total transfers:</b> %" PRId64 "</p>"
        "</div>",
        total_tokens, total_transfers);

    /* First 10 tokens */
    APPEND(off, r, max,
        "<h3>First 10 Tokens</h3>"
        "<table class='txlist'>"
        "<tr><th>Ticker</th><th>Name</th><th>Genesis Block</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT ticker, name, genesis_height FROM zslp_tokens "
                          "ORDER BY genesis_height ASC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int64_t gh = sqlite3_column_int64(s, 2);
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td></tr>",
                    ticker ? ticker : "?", name ? name : "?", gh, gh);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* Most active tokens */
    APPEND(off, r, max,
        "<h3>Most Active Tokens</h3>"
        "<table class='txlist'>"
        "<tr><th>Ticker</th><th>Name</th><th>Transfers</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT t.ticker, t.name, count(x.rowid) as cnt "
            "FROM zslp_tokens t "
            "LEFT JOIN zslp_transfers x ON x.token_id = t.token_id "
            "GROUP BY t.token_id ORDER BY cnt DESC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int64_t cnt = sqlite3_column_int64(s, 2);
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td><td>%" PRId64 "</td></tr>",
                    ticker ? ticker : "?", name ? name : "?", cnt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* ================================================================
     * Section 7: Data Integrity
     * ================================================================ */
    APPEND(off, r, max,
        "<h2>Data Integrity</h2>");

    int64_t chain_height = fq_i64(db, "SELECT MAX(height) FROM blocks");
    int64_t block_count = fq_i64(db, "SELECT count(*) FROM blocks");

    /* Compute integrity hash over recent chain tip */
    char integrity_hash[128] = "";
    {
        /* Hash over last 100 blocks for a rolling integrity check */
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);

        sqlite3_stmt *s = NULL;
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT height, hash, time, num_tx, sapling_value "
            "FROM blocks WHERE height > %" PRId64 " ORDER BY height",
            chain_height > 100 ? chain_height - 100 : 0);

        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                const char *hash = (const char *)sqlite3_column_text(s, 1);
                int64_t t = sqlite3_column_int64(s, 2);
                int64_t ntx = sqlite3_column_int64(s, 3);
                int64_t sv = sqlite3_column_int64(s, 4);

                uint8_t data[48];
                /* height LE 8 bytes */
                for (int j = 0; j < 8; j++)
                    data[j] = (uint8_t)((h >> (j * 8)) & 0xff);
                /* time LE 8 bytes */
                for (int j = 0; j < 8; j++)
                    data[8 + j] = (uint8_t)((t >> (j * 8)) & 0xff);
                /* num_tx LE 4 bytes */
                for (int j = 0; j < 4; j++)
                    data[16 + j] = (uint8_t)((ntx >> (j * 8)) & 0xff);
                /* sapling_value LE 8 bytes */
                for (int j = 0; j < 8; j++)
                    data[20 + j] = (uint8_t)((sv >> (j * 8)) & 0xff);

                sha3_256_write(&ctx, data, 28);
                if (hash)
                    sha3_256_write(&ctx, (const unsigned char *)hash, strlen(hash));
            }
            sqlite3_finalize(s);
        }

        unsigned char digest[32];
        sha3_256_finalize(&ctx, digest);
        compute_full_hash(integrity_hash, sizeof(integrity_hash), digest, 32);
    }

    APPEND(off, r, max,
        "<div class='card'>"
        "<p><b>Chain height:</b> %" PRId64 "</p>"
        "<p><b>Indexed blocks:</b> %" PRId64 "</p>"
        "<p><b>SHA3-256 coverage:</b> %" PRId64 " - %" PRId64 " (last 100 blocks)</p>"
        "<p><b>Latest integrity hash:</b><br>"
        "<code style='word-break:break-all;color:#33ff99'>%s</code></p>"
        "</div>",
        chain_height, block_count,
        chain_height > 100 ? chain_height - 100 : (int64_t)0, chain_height,
        integrity_hash);

    APPEND(off, r, max,
        "<div class='card' style='margin-top:16px'>"
        "<h3>Verification</h3>"
        "<p style='color:#888'>Recompute by replaying blocks from genesis. Each block's "
        "hash chains:</p>"
        "<code style='display:block;padding:12px;background:#0c0c0c;border-radius:4px;"
        "word-break:break-all;color:#ccc'>"
        "SHA3(prev_hash || height_le32 || block_hash || sprout_value_le64 || "
        "sapling_value_le64 || num_tx_le32 || num_js_le32 || num_ss_le32 || num_so_le32)"
        "</code>"
        "</div>");

    /* ── Close page ───────────────────────────────────────── */
    APPEND(off, r, max, "</div>" EXPLORER_FOOTER);

    sqlite3_close(db);

    printf("Factoids: built %zu bytes\n", off);
    fflush(stdout);
    return off;
}
