/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer factoids page -- comprehensive "historian nerd" page with
 * SHA3 data receipts for every single fact. Queries SQLite read-only.
 *
 * 17 Sections: Genesis Story, Network Upgrade History, Mining Era Analysis,
 * Network Milestones, All-Time Records, Supply Milestones, Address Stats,
 * Privacy Usage Over Time (Sprout + Sapling + Shielding Volume),
 * ZSLP Token History, OP_RETURN Archaeology, Dust & UTXO Analysis,
 * Checkpoint History, Block Time Analysis, Transaction Archaeology,
 * Empty Blocks, Difficulty History, Data Integrity.
 *
 * Also provides explorer_factoids_build_json() for /api/factoids. */

#include "controllers/explorer_factoids.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "controllers/explorer_internal.h"
#include "crypto/sha3.h"
#include "util/template.h"
#include "views/wallet_templates_gen.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>

#define fq_i64 sql_query_i64

static void fq_text(sqlite3 *db, const char *sql, char *out, size_t outmax)
{
    sql_query_text(db, sql, out, outmax);
}

/* ── SHA3-256 receipt computation ─────────────────────────── */

static void compute_receipt(char *hex_out, size_t hex_max,
                            int64_t height, const char *block_hash,
                            const char *fact_name)
{
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

    for (size_t i = 0; i < 8 && i * 2 + 2 <= hex_max; i++)
        snprintf(hex_out + i * 2, hex_max - i * 2, "%02x", digest[i]);
}

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

/* Receipt from two int64s + label */
static void compute_receipt_i64(char *hex_out, size_t hex_max,
                                int64_t val1, int64_t val2,
                                const char *label)
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint8_t buf[16];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((val1 >> (i*8)) & 0xff);
    for (int i = 0; i < 8; i++) buf[8+i] = (uint8_t)((val2 >> (i*8)) & 0xff);
    sha3_256_write(&ctx, buf, 16);
    if (label) sha3_256_write(&ctx, (const unsigned char *)label, strlen(label));
    unsigned char digest[32];
    sha3_256_finalize(&ctx, digest);
    for (size_t i = 0; i < 8 && i * 2 + 2 <= hex_max; i++)
        snprintf(hex_out + i * 2, hex_max - i * 2, "%02x", digest[i]);
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
    zcl_format_zcl(buf, max, zatoshi);
}

static void fmt_comma(char *buf, size_t max, int64_t val)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%" PRId64, val);
    size_t len = strlen(raw);
    size_t start = (raw[0] == '-') ? 1 : 0;
    size_t digits = len - start;
    size_t commas = (digits > 0) ? (digits - 1) / 3 : 0;
    size_t total = len + commas;
    if (total >= max) { snprintf(buf, max, "%s", raw); return; }
    buf[total] = '\0';
    size_t src = len;
    size_t dst = total;
    int cnt = 0;
    while (src > start) {
        src--;
        dst--;
        buf[dst] = raw[src];
        cnt++;
        if (cnt == 3 && src > start) { dst--; buf[dst] = ','; cnt = 0; }
    }
    if (start) buf[0] = '-';
}

/* ── Shared: get block hash + time at height ─────────────── */

/* Estimate block time from height using Buttercup spacing.
 * Pre-707000: 150s/block, Post-707000: 75s/block.
 * Used as fallback when block isn't in the SQLite index. */
static int64_t estimate_block_time(int64_t height)
{
    const int64_t genesis_time = ZCL_EXPLORER_GENESIS_TIME;
    const int64_t buttercup_height = 707000;
    const int64_t pre_spacing = 150;
    const int64_t post_spacing = 75;

    if (height <= 0) return genesis_time;
    if (height <= buttercup_height)
        return genesis_time + height * pre_spacing;
    return genesis_time + buttercup_height * pre_spacing +
           (height - buttercup_height) * post_spacing;
}

static void get_block_at(sqlite3 *db, int64_t height,
                         char *hash_out, size_t hmax,
                         int64_t *time_out)
{
    hash_out[0] = '\0';
    *time_out = 0;

    /* Genesis block (height 0) is not in the SQLite index — use constants */
    if (height == 0) {
        snprintf(hash_out, hmax,
            ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX);
        *time_out = ZCL_EXPLORER_GENESIS_TIME;
        return;
    }

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT hex(hash), time FROM blocks WHERE height = %" PRId64, height);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            const char *h = (const char *)sqlite3_column_text(s, 0);
            if (h) snprintf(hash_out, hmax, "%s", h);
            *time_out = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }

    /* Fallback: if block not in index or time=0, estimate from height */
    if (*time_out == 0 && height > 0)
        *time_out = estimate_block_time(height);
}

/* Get block time from SQLite, fallback to height-based estimate */
static int64_t get_block_time(sqlite3 *db, int64_t height)
{
    char sql[128];
    snprintf(sql, sizeof(sql),
        "SELECT time FROM blocks WHERE height = %" PRId64, height);
    int64_t t = fq_i64(db, sql);
    if (t <= 0 && height > 0)
        t = estimate_block_time(height);
    return t;
}

static size_t explorer_factoids_build_verified_summary(uint8_t *buf,
                                                       size_t buf_max,
                                                       sqlite3 *db,
                                                       int64_t chain_height,
                                                       const char *degraded_reason)
{
    size_t off = 0;
    char *r = (char *)buf;
    size_t max = buf_max;

    int64_t utxo_count = fq_i64(db, "SELECT count(*) FROM utxos");
    int64_t utxo_value = fq_i64(db, "SELECT COALESCE(SUM(value),0) FROM utxos");
    int64_t utxo_max_height = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    int64_t block_rows = fq_i64(db, "SELECT count(*) FROM blocks");
    int64_t tx_rows = fq_i64(db, "SELECT count(*) FROM transactions");
    int64_t supply = compute_supply_at_height(chain_height);

    char supply_fmt[64], utxo_fmt[64], rcpt[32] = "";
    fmt_zcl(supply_fmt, sizeof(supply_fmt), supply);
    fmt_zcl(utxo_fmt, sizeof(utxo_fmt), utxo_value);
    compute_receipt_i64(rcpt, sizeof(rcpt), chain_height, supply,
                        "verified_factoids_summary");

    APPEND(off, r, max, EXPLORER_HEADER("ZClassic Factoids"));
    off += explorer_emit_nav((char *)r + off, max - off, "factoids");
    APPEND(off, r, max,
        "<div class='content'>"
        "<h1>ZClassic Factoids</h1>"
        "<div class='card' style='border-color:#775522'>"
        "<h2>Explorer history index rebuilding</h2>"
        "<p style='color:#bbb'>The block-history table failed sanity checks, "
        "so this page is intentionally not serving archaeology facts derived "
        "from that table. This prevents impossible timestamps, bogus records, "
        "and misleading receipts from being published as verified data.</p>"
        "<p style='color:#888'>Reason: <code>%s</code></p>"
        "<p style='color:#888'>Verified live explorer data is shown below. "
        "Block-history sections will return after the explorer block index is "
        "rebuilt from canonical block data.</p>"
        "</div>"
        "<h2>Verified Current State</h2>"
        "<table class='txlist'>"
        "<tr><th>Field</th><th>Value</th></tr>"
        "<tr><td>Current chain height</td><td>%" PRId64 "</td></tr>"
        "<tr><td>Highest UTXO height</td><td>%" PRId64 "</td></tr>"
        "<tr><td>Consensus supply at height</td><td>%s ZCL</td></tr>"
        "<tr><td>Transparent UTXOs</td><td>%" PRId64 "</td></tr>"
        "<tr><td>Transparent UTXO value</td><td>%s ZCL</td></tr>"
        "<tr><td>Block rows in explorer index</td><td>%" PRId64 "</td></tr>"
        "<tr><td>Transaction rows in explorer index</td><td>%" PRId64 "</td></tr>"
        "<tr><td>SHA3 receipt</td><td><code>%s</code></td></tr>"
        "</table>",
        degraded_reason ? degraded_reason : "unknown",
        chain_height, utxo_max_height, supply_fmt, utxo_count, utxo_fmt,
        block_rows, tx_rows, rcpt);

    APPEND(off, r, max,
        "<h2>Consensus Constants</h2>"
        "<table class='txlist'>"
        "<tr><th>Fact</th><th>Value</th></tr>"
        "<tr><td>Genesis hash</td><td><code style='word-break:break-all'>"
        ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX
        "</code></td></tr>"
        "<tr><td>Genesis timestamp</td><td>2016-11-06 03:43:49 UTC</td></tr>"
        "<tr><td>Mainnet P2P port</td><td>8033</td></tr>"
        "<tr><td>Equihash parameters</td><td>N=200, K=9</td></tr>"
        "<tr><td>Buttercup activation height</td><td>707000</td></tr>"
        "</table>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── Section emit helpers ──────────────────────────────────────────
 *
 * Each helper appends one logical section of the factoids HTML page
 * starting at `off` and returns the new offset.
 */

static size_t emit_section_1_genesis(uint8_t *buf, size_t cap, size_t off,
                                     sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='genesis'>1. Genesis Story</h2>");

    int64_t genesis_time = ZCL_EXPLORER_GENESIS_TIME;

    /* Genesis (height 0) is not in the SQLite index — use known constants */
    char genesis_hash[128] =
        ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX;
    char genesis_coinbase[128] = "";
    /* Try DB first, fall back to constant */
    fq_text(db, "SELECT hex(hash) FROM blocks WHERE height = 0",
            genesis_hash, sizeof(genesis_hash));
    if (!genesis_hash[0])
        snprintf(genesis_hash, sizeof(genesis_hash),
            ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX);
    fq_text(db, "SELECT hex(txid) FROM transactions WHERE block_height = 0 AND is_coinbase = 1 LIMIT 1",
            genesis_coinbase, sizeof(genesis_coinbase));
    if (!genesis_coinbase[0])
        snprintf(genesis_coinbase, sizeof(genesis_coinbase),
            "427DBF0AE8E079C6527EA1CB308C6E3C98FA5435F4D715D31176EA00CF2B6119");

    char tstr[64];
    fmt_time(tstr, sizeof(tstr), genesis_time);

    char gen_receipt[32] = "";
    compute_receipt(gen_receipt, sizeof(gen_receipt), 0, genesis_hash, "Genesis");

    APPEND(off, r, max,
        "<div class='card'>"
        "<h3>Block 0: The Beginning</h3>"
        "<p style='color:#888'>ZClassic launched on November 6, 2016 as a fork of Zcash "
        "with no founder's reward tax and the same Equihash (200,9) proof-of-work. "
        "Community-driven from day one.</p>"
        "<table>"
        "<tr><td><b>Genesis Hash</b></td><td><code style='word-break:break-all'>"
        "<a href='/explorer/block/0'>%.64s</a></code></td></tr>"
        "<tr><td><b>Timestamp</b></td><td>%s (Unix: 1478403829)</td></tr>"
        "<tr><td><b>Genesis Coinbase</b></td><td><code style='word-break:break-all'>"
        "<a href='/explorer/tx/%.64s'>%.16s...</a></code></td></tr>"
        "<tr><td><b>Message Start</b></td><td><code>0x24 0xe9 0x27 0x64</code></td></tr>"
        "<tr><td><b>Default Port</b></td><td>8033 (mainnet), 18033 (testnet)</td></tr>"
        "<tr><td><b>BIP44 Coin Type</b></td><td>147</td></tr>"
        "<tr><td><b>Address Prefix (t1)</b></td><td><code>0x1C 0xB8</code></td></tr>"
        "<tr><td><b>Equihash Params</b></td><td>N=200, K=9 (memory-hard, ASIC-resistant)</td></tr>"
        "<tr><td><b>PoW Limit</b></td><td><code>0x0007ffff...fff</code></td></tr>"
        "<tr><td><b>SHA3 Receipt</b></td><td><code>%s</code></td></tr>"
        "</table></div>",
        genesis_hash, tstr, genesis_coinbase, genesis_coinbase, gen_receipt);

    /* First 10 blocks table */
    APPEND(off, r, max,
        "<h3>First 10 Blocks</h3>"
        "<table class='txlist'>"
        "<tr><th>Height</th><th>Time</th><th>Block Hash</th><th>SHA3 Receipt</th></tr>");

    /* Genesis (height 0) is not in SQLite — add manually */
    {
        char rcpt0[32] = "";
        compute_receipt(rcpt0, sizeof(rcpt0), 0, genesis_hash, "first10");
        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/0'>0</a></td>"
            "<td>2016-11-06 03:43:49 UTC</td>"
            "<td><code style='word-break:break-all'>%.16s...</code></td>"
            "<td><code>%s</code></td></tr>",
            genesis_hash, rcpt0);
    }
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT height, time, hex(hash) FROM blocks WHERE height >= 1 AND height < 10 ORDER BY height";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                const char *hash = (const char *)sqlite3_column_text(s, 2);
                char ts[64], rcpt[32] = "";
                fmt_time(ts, sizeof(ts), t);
                compute_receipt(rcpt, sizeof(rcpt), h, hash ? hash : "", "first10");
                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td>"
                    "<td><code style='word-break:break-all'>%.16s...</code></td>"
                    "<td><code>%s</code></td></tr>",
                    h, h, ts, hash ? hash : "?", rcpt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_2_upgrades(uint8_t *buf, size_t cap, size_t off,
                                      sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='upgrades'>2. Network Upgrade History</h2>"
        "<p style='color:#888'>Every consensus upgrade with activation height, "
        "protocol version, branch ID, and purpose. Each secured with SHA3 receipt.</p>"
        "<table class='txlist'>"
        "<tr><th>Upgrade</th><th>Height</th><th>Date</th>"
        "<th>Proto</th><th>Branch ID</th><th>Purpose</th><th>SHA3</th></tr>");

    struct {
        const char *name;
        int64_t height;
        int proto;
        const char *branch_id;
        const char *purpose;
    } upgrades[] = {
        { "Sprout (Base)", 0, 170002, "0x00000000",
          "Initial ZClassic network launch" },
        { "Overwinter", 476969, 170005, "0x5ba81b19",
          "Transaction format v3, replay protection, expiry" },
        { "Sapling", 476969, 170007, "0x76b809bb",
          "Shielded transactions (Groth16 proofs, 100x faster)" },
        { "Bubbles", 585318, 170009, "0x821a451c",
          "ZClassic-specific protocol enhancements" },
        { "DiffAdj (Bubbly)", 585322, 170010, "0x930b540d",
          "Difficulty adjustment algorithm refinement" },
        { "Buttercup", 707000, 170011, "0x930b540d",
          "Block time 150s\xe2\x86\x92" "75s, halving doubled, subsidy adjusted" },
    };
    int n_upgrades = (int)(sizeof(upgrades) / sizeof(upgrades[0]));

    for (int i = 0; i < n_upgrades; i++) {
        char bhash[128] = "";
        int64_t btime = 0;
        get_block_at(db, upgrades[i].height, bhash, sizeof(bhash), &btime);

        char ts[64], rcpt[32] = "";
        fmt_time(ts, sizeof(ts), btime);
        compute_receipt(rcpt, sizeof(rcpt), upgrades[i].height, bhash, upgrades[i].name);

        if (btime > 0 || upgrades[i].height == 0) {
            APPEND(off, r, max,
                "<tr><td><b>%s</b></td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s</td><td>%d</td><td><code>%s</code></td>"
                "<td>%s</td><td><code>%s</code></td></tr>",
                upgrades[i].name, upgrades[i].height, upgrades[i].height,
                ts, upgrades[i].proto, upgrades[i].branch_id,
                upgrades[i].purpose, rcpt);
        } else {
            APPEND(off, r, max,
                "<tr><td><b>%s</b></td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td>"
                "<td>%d</td><td><code>%s</code></td>"
                "<td>%s</td><td><code>--</code></td></tr>",
                upgrades[i].name, upgrades[i].height,
                upgrades[i].proto, upgrades[i].branch_id,
                upgrades[i].purpose);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_3_mining_eras(uint8_t *buf, size_t cap, size_t off,
                                         int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='mining-eras'>3. Mining Era Analysis</h2>"
        "<p style='color:#888'>Block reward schedule showing the Buttercup transition "
        "at block 707,000 which halved block time and shifted the halving schedule.</p>"
        "<table class='txlist'>"
        "<tr><th>Era</th><th>Block Range</th><th>Subsidy/Block</th>"
        "<th>Target Spacing</th><th>Blocks</th><th>Total Emission</th><th>SHA3</th></tr>");

    /* Pre-Buttercup eras */
    {
        int64_t era_start = 0;
        int64_t pre_bc_end = chain_height < BUTTERCUP_ACTIVATION_HEIGHT
                           ? chain_height : BUTTERCUP_ACTIVATION_HEIGHT;
        int era_num = 0;
        int64_t subsidy = BASE_SUBSIDY_SAT;

        while (era_start < pre_bc_end && subsidy > 0 && era_num < 10) {
            int64_t next = ((era_start / PRE_BC_HALVING) + 1) * PRE_BC_HALVING;
            int64_t end = next < pre_bc_end ? next : pre_bc_end;
            int64_t count = end - era_start;

            char sub_str[64], emit_str[64], rcpt[32] = "";
            fmt_zcl(sub_str, sizeof(sub_str), subsidy);
            int64_t emission = count * subsidy;
            fmt_zcl(emit_str, sizeof(emit_str), emission);
            char blk_str[32];
            fmt_comma(blk_str, sizeof(blk_str), count);
            compute_receipt_i64(rcpt, sizeof(rcpt), era_start, end, "mining_era");

            APPEND(off, r, max,
                "<tr><td>Pre-BC #%d</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a>"
                " \xe2\x80\x93 <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s ZCL</td><td>150s</td><td>%s</td><td>%s ZCL</td>"
                "<td><code>%s</code></td></tr>",
                era_num, era_start, era_start, end - 1, end - 1,
                sub_str, blk_str, emit_str, rcpt);

            era_start = end;
            era_num++;
            int halvings = (int)(era_start / PRE_BC_HALVING);
            subsidy = (halvings >= 64) ? 0 : (BASE_SUBSIDY_SAT >> halvings);
        }
    }

    /* Post-Buttercup eras */
    if (chain_height > BUTTERCUP_ACTIVATION_HEIGHT) {
        int64_t era_offset = 0;
        int64_t remaining = chain_height - BUTTERCUP_ACTIVATION_HEIGHT;
        int64_t bc_base = BASE_SUBSIDY_SAT / 2;  /* 6.25 ZCL */
        int era_num = 0;

        while (remaining > 0 && era_num < 10) {
            int halvings_raw = (era_offset > 0)
                ? (int)((era_offset - 1) / POST_BC_HALVING) : 0;
            int halvings = halvings_raw + 3;
            if (halvings >= 64) break;
            int64_t era_subsidy = bc_base >> halvings;
            if (era_subsidy <= 0) break;

            int64_t next_boundary = ((int64_t)(halvings_raw + 1)) * POST_BC_HALVING + 1;
            int64_t blocks_in_era = next_boundary - era_offset;
            if (blocks_in_era > remaining) blocks_in_era = remaining;

            int64_t abs_start = BUTTERCUP_ACTIVATION_HEIGHT + era_offset;
            int64_t abs_end = abs_start + blocks_in_era;

            char sub_str[64], emit_str[64], rcpt[32] = "";
            fmt_zcl(sub_str, sizeof(sub_str), era_subsidy);
            int64_t emission = blocks_in_era * era_subsidy;
            fmt_zcl(emit_str, sizeof(emit_str), emission);
            char blk_str[32];
            fmt_comma(blk_str, sizeof(blk_str), blocks_in_era);
            compute_receipt_i64(rcpt, sizeof(rcpt), abs_start, abs_end, "mining_era_bc");

            APPEND(off, r, max,
                "<tr><td>Post-BC #%d (halv=%d)</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a>"
                " \xe2\x80\x93 <a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s ZCL</td><td>75s</td><td>%s</td><td>%s ZCL</td>"
                "<td><code>%s</code></td></tr>",
                era_num, halvings, abs_start, abs_start, abs_end - 1, abs_end - 1,
                sub_str, blk_str, emit_str, rcpt);

            remaining -= blocks_in_era;
            era_offset += blocks_in_era;
            era_num++;
        }
    }

    /* Total supply row */
    {
        int64_t total_supply = compute_supply_at_height(chain_height);
        char total_str[64], rcpt[32] = "";
        fmt_zcl(total_str, sizeof(total_str), total_supply);
        compute_receipt_i64(rcpt, sizeof(rcpt), chain_height, total_supply, "total_supply");
        APPEND(off, r, max,
            "<tr style='border-top:2px solid #33ff99'>"
            "<td colspan='4'><b>Total Mined Supply</b></td>"
            "<td></td><td><b>%s ZCL</b></td>"
            "<td><code>%s</code></td></tr>",
            total_str, rcpt);
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_4_milestones(uint8_t *buf, size_t cap, size_t off,
                                        sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='milestones'>4. Network Milestones</h2>"
        "<p style='color:#888'>Key firsts in the chain's history.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Time</th><th>SHA3 Receipt</th></tr>");

    struct milestone {
        const char *name;
        const char *sql;
        int64_t fixed_height;
        int64_t known_height;  /* authoritative fallback when SQL table is empty */
    };

    /* known_height values are immutable blockchain facts verified against
     * zclassicd getblock RPC on 2026-03-25.  They serve as fallbacks when
     * Phase B indexing tables (joinsplits, sapling_spends, sapling_outputs,
     * op_returns) haven't been populated yet. */
    struct milestone milestones[] = {
        { "Genesis (Nov 6, 2016)", NULL, 0, -1 },
        { "First non-coinbase tx",
          "SELECT MIN(block_height) FROM transactions WHERE is_coinbase = 0", -1, -1 },
        { "First Sprout JoinSplit",
          "SELECT MIN(block_height) FROM joinsplits", -1, 241 },
        { "Overwinter + Sapling activation", NULL, 476969, -1 },
        { "First Sapling shielded spend",
          "SELECT MIN(block_height) FROM sapling_spends", -1, 477214 },
        { "First Sapling shielded output",
          "SELECT MIN(block_height) FROM sapling_outputs", -1, 477214 },
        { "First OP_RETURN",
          "SELECT MIN(block_height) FROM op_returns", -1, 649950 },
        { "Bubbles upgrade activation", NULL, 585318, -1 },
        { "DiffAdj (Bubbly) activation", NULL, 585322, -1 },
        { "First ZSLP token genesis",
          "SELECT MIN(genesis_height) FROM zslp_tokens", -1, -1 },
        { "Buttercup activation (75s blocks)", NULL, 707000, -1 },
        { "First halving (pre-Buttercup)", NULL, 840000, -1 },
        { "Block 1,000,000", NULL, 1000000, -1 },
        { "Block 2,000,000", NULL, 2000000, -1 },
        { "Block 3,000,000", NULL, 3000000, -1 },
    };

    int n_milestones = (int)(sizeof(milestones) / sizeof(milestones[0]));
    for (int i = 0; i < n_milestones; i++) {
        int64_t height = milestones[i].fixed_height;
        if (milestones[i].sql)
            height = fq_i64(db, milestones[i].sql);

        /* If SQL returned nothing (table empty), fall back to known height */
        if (height <= 0 && milestones[i].known_height > 0)
            height = milestones[i].known_height;

        if (height <= 0 && i > 0) {
            /* Distinguish "not yet reached" from "index not yet built" —
             * if chain is past Sapling activation and a table is empty,
             * the data exists on-chain but hasn't been indexed yet. */
            bool index_pending = (milestones[i].sql != NULL &&
                                  chain_height > 500000);
            APPEND(off, r, max,
                "<tr><td>%s</td><td colspan='3' style='color:#666'>%s</td></tr>",
                milestones[i].name,
                index_pending ? "Pending index rebuild" : "Not yet reached");
            continue;
        }
        if (height > chain_height && milestones[i].fixed_height >= 0) {
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td>"
                "<td><code>--</code></td></tr>",
                milestones[i].name, height);
            continue;
        }

        char bhash[128] = "";
        int64_t btime = 0;
        get_block_at(db, height, bhash, sizeof(bhash), &btime);

        char receipt[32] = "", ts[64];
        compute_receipt(receipt, sizeof(receipt), height, bhash, milestones[i].name);
        fmt_time(ts, sizeof(ts), btime);

        APPEND(off, r, max,
            "<tr><td>%s</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%s</td>"
            "<td><code>%s</code></td></tr>",
            milestones[i].name, height, height, ts, receipt);
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_5_records(uint8_t *buf, size_t cap, size_t off,
                                     sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max, "<h2 id='records'>5. All-Time Records</h2>"
        "<table class='txlist'>"
        "<tr><th>Record</th><th>Value</th><th>Block</th><th>Time</th><th>SHA3</th></tr>");

    /* Record helper macro */
    #define RECORD_ROW(label, val_fmt, val_args, height_val, time_val) do { \
        char _ts[64], _rcpt[32] = ""; \
        fmt_time(_ts, sizeof(_ts), time_val); \
        compute_receipt(_rcpt, sizeof(_rcpt), height_val, "", label); \
        APPEND(off, r, max, \
            "<tr><td>" label "</td><td>" val_fmt "</td>" \
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>" \
            "<td>%s</td><td><code>%s</code></td></tr>", \
            val_args, (int64_t)(height_val), (int64_t)(height_val), _ts, _rcpt); \
    } while(0)

    /* Largest transparent output */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT u.value, u.height, b.time FROM utxos u "
                          "JOIN blocks b ON u.height = b.height "
                          "ORDER BY u.value DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), row.v0);
            RECORD_ROW("Largest transparent output",
                "%s ZCL", vstr, row.v1, row.v2);
        }
    }

    /* Most transactions in a single block */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT b.height, b.num_tx, b.time FROM blocks b "
                          "ORDER BY b.num_tx DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            RECORD_ROW("Most transactions in a block",
                "%" PRId64 " tx", row.v1, row.v0, row.v2);
        }
    }

    /* Most JoinSplits in a single block */
    {
        struct sql_row_i64_2 row;
        const char *sql = "SELECT block_height, count(*) as cnt FROM joinsplits "
                          "GROUP BY block_height ORDER BY cnt DESC LIMIT 1";
        if (sql_query_row_i64_2(db, sql, &row)) {
            int64_t t = get_block_time(db, row.v0);
            RECORD_ROW("Most JoinSplits in a block",
                "%" PRId64, row.v1, row.v0, t);
        }
    }

    /* Most Sapling outputs in a single block */
    {
        struct sql_row_i64_2 row;
        const char *sql = "SELECT block_height, count(*) as cnt FROM sapling_outputs "
                          "GROUP BY block_height ORDER BY cnt DESC LIMIT 1";
        if (sql_query_row_i64_2(db, sql, &row)) {
            int64_t t = get_block_time(db, row.v0);
            RECORD_ROW("Most Sapling outputs in a block",
                "%" PRId64, row.v1, row.v0, t);
        }
    }

    /* Highest difficulty ever */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT height, bits, time FROM blocks "
                          "WHERE bits > 0 ORDER BY bits ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            double diff = explorer_difficulty_from_bits((uint32_t)row.v1);
            char dstr[64];
            snprintf(dstr, sizeof(dstr), "%.2f", diff);
            RECORD_ROW("Highest difficulty",
                "%s", dstr, row.v0, row.v2);
        }
    }

    /* Longest gap between blocks */
    {
        struct sql_row_i64_3 row;
        const char *sql =
            "SELECT a.height, a.time, (b.time - a.time) as gap "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 "
            "ORDER BY gap DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char gstr[64];
            snprintf(gstr, sizeof(gstr), "%" PRId64 "m %" PRId64 "s",
                     row.v2 / 60, row.v2 % 60);
            RECORD_ROW("Longest block gap",
                "%s", gstr, row.v0, row.v1);
        }
    }

    /* Shortest gap between blocks */
    {
        struct sql_row_i64_3 row;
        const char *sql =
            "SELECT a.height, a.time, (b.time - a.time) as gap "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 AND (b.time - a.time) > 0 "
            "ORDER BY gap ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char gstr[64];
            snprintf(gstr, sizeof(gstr), "%" PRId64 "s", row.v2);
            RECORD_ROW("Shortest block gap",
                "%s", gstr, row.v0, row.v1);
        }
    }

    /* Largest single shielding (t→z): negative sapling_value = value entering pool */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT height, sapling_value, time FROM blocks "
                          "WHERE sapling_value < 0 ORDER BY sapling_value ASC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), -row.v1);
            RECORD_ROW("Largest single-block shielding (t\xe2\x86\x92z)",
                "%s ZCL", vstr, row.v0, row.v2);
        }
    }

    /* Largest single unshielding (z→t): positive sapling_value = value leaving pool */
    {
        struct sql_row_i64_3 row;
        const char *sql = "SELECT height, sapling_value, time FROM blocks "
                          "WHERE sapling_value > 0 ORDER BY sapling_value DESC LIMIT 1";
        if (sql_query_row_i64_3(db, sql, &row)) {
            char vstr[64];
            fmt_zcl(vstr, sizeof(vstr), row.v1);
            RECORD_ROW("Largest single-block unshielding (z\xe2\x86\x92t)",
                "%s ZCL", vstr, row.v0, row.v2);
        }
    }

    #undef RECORD_ROW
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_6_supply(uint8_t *buf, size_t cap, size_t off,
                                    sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='supply'>6. Supply Milestones</h2>"
        "<p style='color:#888'>Buttercup-aware calculation: pre-707000 at 12.5 ZCL/block, "
        "post-707000 at 0.78125 ZCL/block (6.25 &gt;&gt; 3), with 1.68M-block halvings.</p>"
        "<table class='txlist'>"
        "<tr><th>Milestone</th><th>Block</th><th>Date</th><th>SHA3</th></tr>");

    /* Find block heights where supply reaches milestones via binary search */
    struct { const char *label; int64_t target_sat; } supply_milestones[] = {
        { "1,000,000 ZCL mined",  ZATOSHI_PER_ZCL * 1000000LL },
        { "5,000,000 ZCL mined",  ZATOSHI_PER_ZCL * 5000000LL },
        { "8,837,500 ZCL (pre-Buttercup max)", ZATOSHI_PER_ZCL * 8837500LL },
        { "10,000,000 ZCL mined", ZATOSHI_PER_ZCL * 10000000LL },
        { "10,500,000 ZCL mined", ZATOSHI_PER_ZCL * 10500000LL },
    };

    for (int i = 0; i < (int)(sizeof(supply_milestones)/sizeof(supply_milestones[0])); i++) {
        int64_t target = supply_milestones[i].target_sat;

        /* Binary search for the height where supply >= target */
        int64_t lo = 0, hi = 100000000LL; /* 100M blocks max */
        if (hi > chain_height + 50000000LL) hi = chain_height + 50000000LL;
        int64_t milestone_height = -1;

        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            int64_t supply = compute_supply_at_height(mid);
            if (supply >= target) {
                milestone_height = mid;
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }

        if (milestone_height < 0 || milestone_height > 100000000LL) {
            APPEND(off, r, max,
                "<tr><td>%s</td><td colspan='3' style='color:#666'>Beyond max supply</td></tr>",
                supply_milestones[i].label);
            continue;
        }

        char bhash[128] = "";
        int64_t btime = 0;
        if (milestone_height <= chain_height) {
            get_block_at(db, milestone_height, bhash, sizeof(bhash), &btime);
        }

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), milestone_height, target, "supply_milestone");

        if (btime > 0) {
            char ts[64];
            fmt_time(ts, sizeof(ts), btime);
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                "<td>%s</td><td><code>%s</code></td></tr>",
                supply_milestones[i].label, milestone_height, milestone_height, ts, rcpt);
        } else {
            APPEND(off, r, max,
                "<tr><td>%s</td>"
                "<td>%" PRId64 "</td>"
                "<td style='color:#666'>Not yet reached</td>"
                "<td><code>%s</code></td></tr>",
                supply_milestones[i].label, milestone_height, rcpt);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_7_addresses(uint8_t *buf, size_t cap, size_t off,
                                       sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='addresses'>7. Address Statistics</h2>");

    struct explorer_address_stats address_stats = {0};
    explorer_query_address_stats(db, &address_stats);
    int64_t addr_total = address_stats.total;
    int64_t addr_nonzero = address_stats.nonzero;
    int64_t addr_over_1 = fq_i64(db, "SELECT count(*) FROM addresses WHERE balance >= 100000000");
    int64_t addr_over_100 = fq_i64(db, "SELECT count(*) FROM addresses WHERE balance >= 10000000000");

    {
        char t_str[32], nz_str[32], o1_str[32], o100_str[32];
        fmt_comma(t_str, sizeof(t_str), addr_total);
        fmt_comma(nz_str, sizeof(nz_str), addr_nonzero);
        fmt_comma(o1_str, sizeof(o1_str), addr_over_1);
        fmt_comma(o100_str, sizeof(o100_str), addr_over_100);

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), addr_total, addr_nonzero, "addr_stats");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total unique addresses seen:</b> %s</p>"
            "<p><b>Addresses with balance &gt; 0:</b> %s</p>"
            "<p><b>Addresses with \xe2\x89\xa5 1 ZCL:</b> %s</p>"
            "<p><b>Addresses with \xe2\x89\xa5 100 ZCL:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            t_str, nz_str, o1_str, o100_str, rcpt);
    }

    /* Top 10 richest addresses */
    APPEND(off, r, max,
        "<h3>Top 10 Richest Addresses</h3>"
        "<table class='txlist'>"
        "<tr><th>#</th><th>Address Hash</th><th>Balance</th>"
        "<th>UTXOs</th><th>First Seen</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT hex(address_hash), balance, utxo_count, first_seen_height "
            "FROM addresses ORDER BY balance DESC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            int rank = 1;
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *ah = (const char *)sqlite3_column_text(s, 0);
                int64_t bal = sqlite3_column_int64(s, 1);
                int64_t uc = sqlite3_column_int64(s, 2);
                int64_t fsh = sqlite3_column_int64(s, 3);
                char bstr[64];
                fmt_zcl(bstr, sizeof(bstr), bal);
                APPEND(off, r, max,
                    "<tr><td>%d</td><td><code>%.16s...</code></td>"
                    "<td>%s ZCL</td><td>%" PRId64 "</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td></tr>",
                    rank, ah ? ah : "?", bstr, uc, fsh, fsh);
                rank++;
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_8_privacy(uint8_t *buf, size_t cap, size_t off,
                                     sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='privacy'>8. Privacy Usage Over Time</h2>"
        "<p style='color:#888'>Shielded operations by calendar year "
        "(from actual block timestamps). Includes Sprout JoinSplits, "
        "Sapling spends, Sapling outputs, and net shielding volume.</p>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Blocks</th><th>JoinSplits</th>"
        "<th>Sapling Spends</th><th>Sapling Outputs</th>"
        "<th>Net Shielded (ZCL)</th></tr>");

    {
        int64_t blk_yrs[20] = {0}, js_yrs[20] = {0};
        int64_t ss_yrs[20] = {0}, so_yrs[20] = {0};
        int64_t sv_yrs[20] = {0};  /* net sapling value */
        int max_yr = 2016;
        sqlite3_stmt *s = NULL;

        /* Blocks per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER), "
                "count(*) FROM blocks WHERE time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                int idx = yr - 2016;
                if (idx >= 0 && idx < 20) {
                    blk_yrs[idx] = sqlite3_column_int64(s, 1);
                    if (yr > max_yr) max_yr = yr;
                }
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* JoinSplits per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER), "
                "count(*) FROM joinsplits j "
                "JOIN blocks b ON j.block_height = b.height "
                "WHERE b.time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    js_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* Sapling spends per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER), "
                "count(*) FROM sapling_spends sp "
                "JOIN blocks b ON sp.block_height = b.height "
                "WHERE b.time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    ss_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* Sapling outputs per year */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER), "
                "count(*) FROM sapling_outputs so "
                "JOIN blocks b ON so.block_height = b.height "
                "WHERE b.time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    so_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        /* Net shielding volume per year. Convention (matches
         * explorer_query_privacy_stats + getblockchaininfo valuePools):
         * net_shielded = -SUM(sapling_value) — negative raw values are
         * coins entering the shielded pool, so the negation makes the
         * column positive when shielding > unshielding for the year. */
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER), "
                "-SUM(sapling_value) FROM blocks "
                "WHERE sapling_value != 0 AND time > 0 GROUP BY 1 ORDER BY 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int idx = sqlite3_column_int(s, 0) - 2016;
                if (idx >= 0 && idx < 20)
                    sv_yrs[idx] = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s); s = NULL;
        }

        for (int yr = 2016; yr <= max_yr && yr < 2036; yr++) {
            int idx = yr - 2016;
            if (blk_yrs[idx] == 0) continue;
            char sv_str[64];
            fmt_zcl(sv_str, sizeof(sv_str), sv_yrs[idx]);
            APPEND(off, r, max,
                "<tr><td>%d</td><td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td><td>%" PRId64 "</td>"
                "<td>%" PRId64 "</td><td>%s</td></tr>",
                yr, blk_yrs[idx], js_yrs[idx], ss_yrs[idx],
                so_yrs[idx], sv_str);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_9_zslp(uint8_t *buf, size_t cap, size_t off,
                                  sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='zslp'>9. ZSLP Token History</h2>");

    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);
    int64_t total_tokens = token_stats.token_count;
    int64_t total_transfers = token_stats.transfer_count;

    {
        char tk_str[32], xf_str[32], rcpt[32] = "";
        fmt_comma(tk_str, sizeof(tk_str), total_tokens);
        fmt_comma(xf_str, sizeof(xf_str), total_transfers);
        compute_receipt_i64(rcpt, sizeof(rcpt), total_tokens, total_transfers, "zslp_summary");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total tokens created:</b> %s</p>"
            "<p><b>Total transfers:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            tk_str, xf_str, rcpt);
    }

    /* First 10 tokens */
    APPEND(off, r, max,
        "<h3>First 10 Tokens</h3>"
        "<table class='txlist'>"
        "<tr><th>Ticker</th><th>Name</th><th>Decimals</th>"
        "<th>Genesis Block</th><th>Token ID</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql = "SELECT ticker, name, decimals, genesis_height, hex(token_id) "
                          "FROM zslp_tokens ORDER BY genesis_height ASC LIMIT 10";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int dec = sqlite3_column_int(s, 2);
                int64_t gh = sqlite3_column_int64(s, 3);
                const char *tid = (const char *)sqlite3_column_text(s, 4);
                APPEND(off, r, max,
                    "<tr><td>%s</td><td>%s</td><td>%d</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td><code>%.16s...</code></td></tr>",
                    ticker ? ticker : "?", name ? name : "?", dec,
                    gh, gh, tid ? tid : "?");
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
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
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
    return off;
}

static size_t emit_section_10_opreturn(uint8_t *buf, size_t cap, size_t off,
                                       sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='opreturn'>10. OP_RETURN Archaeology</h2>");

    struct explorer_op_return_stats op_return_stats = {0};
    explorer_query_op_return_stats(db, &op_return_stats);
    int64_t total_opret = op_return_stats.total;
    int64_t slp_opret = op_return_stats.zslp;
    int64_t nonslp_opret = total_opret - slp_opret;
    int64_t first_opret = fq_i64(db, "SELECT MIN(block_height) FROM op_returns");
    int64_t first_nonslp = fq_i64(db,
        "SELECT MIN(block_height) FROM op_returns WHERE is_slp = 0");

    {
        char tot_str[32], slp_str[32], non_str[32], rcpt[32] = "";
        fmt_comma(tot_str, sizeof(tot_str), total_opret);
        fmt_comma(slp_str, sizeof(slp_str), slp_opret);
        fmt_comma(non_str, sizeof(non_str), nonslp_opret);
        compute_receipt_i64(rcpt, sizeof(rcpt), total_opret, first_opret, "opreturn_stats");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total OP_RETURN outputs:</b> %s</p>"
            "<p><b>ZSLP OP_RETURNs:</b> %s</p>"
            "<p><b>Non-ZSLP OP_RETURNs:</b> %s</p>"
            "<p><b>First OP_RETURN at block:</b> "
            "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>",
            tot_str, slp_str, non_str, first_opret, first_opret);

        if (first_nonslp > 0) {
            APPEND(off, r, max,
                "<p><b>First non-ZSLP OP_RETURN:</b> "
                "<a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></p>",
                first_nonslp, first_nonslp);
        }
        APPEND(off, r, max,
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>", rcpt);
    }
    return off;
}

static size_t emit_section_11_dust(uint8_t *buf, size_t cap, size_t off,
                                   sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='dust'>11. Dust &amp; UTXO Analysis</h2>");

    struct explorer_utxo_stats utxo_stats = {0};
    explorer_query_utxo_stats(db, &utxo_stats);
    int64_t utxo_total = utxo_stats.count;
    int64_t dust_1000 = fq_i64(db, "SELECT count(*) FROM utxos WHERE value < 100000");
    int64_t dust_10000 = fq_i64(db, "SELECT count(*) FROM utxos WHERE value < 1000000");
    int64_t utxo_value_total = utxo_stats.total_value_sat;
    int64_t coinbase_utxos = fq_i64(db,
        "SELECT count(*) FROM utxos WHERE is_coinbase = 1");

    {
        char u_str[32], d1_str[32], d2_str[32], cb_str[32], val_str[64], rcpt[32] = "";
        fmt_comma(u_str, sizeof(u_str), utxo_total);
        fmt_comma(d1_str, sizeof(d1_str), dust_1000);
        fmt_comma(d2_str, sizeof(d2_str), dust_10000);
        fmt_comma(cb_str, sizeof(cb_str), coinbase_utxos);
        fmt_zcl(val_str, sizeof(val_str), utxo_value_total);
        compute_receipt_i64(rcpt, sizeof(rcpt), utxo_total, utxo_value_total, "utxo_analysis");

        double dust_pct = utxo_total > 0
            ? (double)dust_1000 * 100.0 / (double)utxo_total : 0.0;

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Total UTXOs:</b> %s</p>"
            "<p><b>Total UTXO value:</b> %s ZCL</p>"
            "<p><b>Dust (&lt;0.001 ZCL):</b> %s (%.1f%% of UTXO set)</p>"
            "<p><b>Dust (&lt;0.01 ZCL):</b> %s</p>"
            "<p><b>Unspent coinbase outputs:</b> %s</p>"
            "<p><b>SHA3 Receipt:</b> <code>%s</code></p>"
            "</div>",
            u_str, val_str, d1_str, dust_pct, d2_str, cb_str, rcpt);
    }
    return off;
}

static size_t emit_section_12_checkpoints(uint8_t *buf, size_t cap, size_t off,
                                          sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='checkpoints'>12. Checkpoint History</h2>"
        "<p style='color:#888'>Hardcoded consensus checkpoints — "
        "blocks that all nodes must agree on.</p>"
        "<table class='txlist'>"
        "<tr><th>Height</th><th>Date</th><th>Block Hash</th><th>SHA3</th></tr>");

    {
        /* Checkpoint data: heights + authoritative hashes from zclassicd.
         * These are immutable blockchain facts — no SQLite dependency. */
        struct { int64_t height; const char *hash; } checkpoints[] = {
            { 0,       "0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602" },
            { 30000,   "000000005c2ad200c3c7c8e627f67b306659efca1268c9bb014335fdadc0c392" },
            { 160000,  "000000065093005a1a46ee95d6d66c2b07008220ca64dd3b3a93bbd1945480c0" },
            { 468200,  "000000009bd5548c851c2b237894d6807a53bf1e2808402545e27a995ae4f3c3" },
            { 2013514, "000019679aa2ea97a3f18bd9265bc91a09929ea0b1acc0fc5ef77cdf3cf906e7" },
            { 2879438, "000007e8fccb9e4831c7d7376a283b016ead6166491f951f4f083dbe366992b2" },
            { 3054000, "000005aa8e8c321cf364788e81b94619434b0dc1a85e658a022b44f23eb85662" },
        };
        int n_cp = (int)(sizeof(checkpoints) / sizeof(checkpoints[0]));
        for (int i = 0; i < n_cp; i++) {
            int64_t btime = 0;
            char bhash_unused[128] = "";
            get_block_at(db, checkpoints[i].height, bhash_unused, sizeof(bhash_unused), &btime);

            char height_s[32], ts[64], rcpt[32] = "";
            char hash_short[20];
            snprintf(height_s, sizeof(height_s), "%" PRId64, checkpoints[i].height);
            fmt_time(ts, sizeof(ts), btime);
            compute_receipt(rcpt, sizeof(rcpt), checkpoints[i].height,
                            checkpoints[i].hash, "checkpoint");
            snprintf(hash_short, sizeof(hash_short), "%.16s", checkpoints[i].hash);

            struct template_var vars[] = {
                { "height",     height_s },
                { "time",       checkpoints[i].height <= chain_height ? ts : "<span style='color:#666'>Not yet reached</span>" },
                { "hash_short", hash_short },
                { "receipt",    checkpoints[i].height <= chain_height ? rcpt : "--" },
            };
            off += template_render(TMPL_CHECKPOINT_ROW,
                                   vars, sizeof(vars)/sizeof(vars[0]),
                                   (char *)r + off, max - off);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_13_blocktimes(uint8_t *buf, size_t cap, size_t off,
                                         sqlite3 *db, int64_t chain_height)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='blocktimes'>13. Block Time Analysis</h2>"
        "<p style='color:#888'>Pre-Buttercup target: 150s. "
        "Post-Buttercup target: 75s. Actual times from chain data.</p>");

    /* Pre-Buttercup stats */
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT AVG(b.time - a.time), "
            "MIN(CASE WHEN b.time - a.time > 0 THEN b.time - a.time END), "
            "MAX(b.time - a.time), "
            "count(*) "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 AND a.height < 707000";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                double avg = sqlite3_column_double(s, 0);
                int64_t mn = sqlite3_column_int64(s, 1);
                int64_t mx = sqlite3_column_int64(s, 2);
                int64_t cnt = sqlite3_column_int64(s, 3);
                char rcpt[32] = "";
                compute_receipt_i64(rcpt, sizeof(rcpt), (int64_t)avg, cnt,
                                    "blocktime_pre_bc");
                APPEND(off, r, max,
                    "<div class='card'>"
                    "<h3>Pre-Buttercup (blocks 0\xe2\x80\x93" "706,999, target 150s)</h3>"
                    "<table>"
                    "<tr><td><b>Mean block time:</b></td><td>%.1fs</td></tr>"
                    "<tr><td><b>Shortest gap:</b></td><td>%" PRId64 "s</td></tr>"
                    "<tr><td><b>Longest gap:</b></td><td>%" PRId64 "s (%.1f min)</td></tr>"
                    "<tr><td><b>Block pairs analyzed:</b></td><td>%" PRId64 "</td></tr>"
                    "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
                    "</table></div>",
                    avg, mn, mx, (double)mx / 60.0, cnt, rcpt);
            }
            sqlite3_finalize(s);
        }
    }

    /* Post-Buttercup stats */
    if (chain_height > BUTTERCUP_ACTIVATION_HEIGHT) {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT AVG(b.time - a.time), "
            "MIN(CASE WHEN b.time - a.time > 0 THEN b.time - a.time END), "
            "MAX(b.time - a.time), "
            "count(*) "
            "FROM blocks a JOIN blocks b ON b.height = a.height + 1 "
            "WHERE a.time > 0 AND b.time > 0 AND a.height >= 707000";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                double avg = sqlite3_column_double(s, 0);
                int64_t mn = sqlite3_column_int64(s, 1);
                int64_t mx = sqlite3_column_int64(s, 2);
                int64_t cnt = sqlite3_column_int64(s, 3);
                char rcpt[32] = "";
                compute_receipt_i64(rcpt, sizeof(rcpt), (int64_t)avg, cnt,
                                    "blocktime_post_bc");
                APPEND(off, r, max,
                    "<div class='card'>"
                    "<h3>Post-Buttercup (blocks 707,000+, target 75s)</h3>"
                    "<table>"
                    "<tr><td><b>Mean block time:</b></td><td>%.1fs</td></tr>"
                    "<tr><td><b>Shortest gap:</b></td><td>%" PRId64 "s</td></tr>"
                    "<tr><td><b>Longest gap:</b></td><td>%" PRId64 "s (%.1f min)</td></tr>"
                    "<tr><td><b>Block pairs analyzed:</b></td><td>%" PRId64 "</td></tr>"
                    "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
                    "</table></div>",
                    avg, mn, mx, (double)mx / 60.0, cnt, rcpt);
            }
            sqlite3_finalize(s);
        }
    }
    return off;
}

static size_t emit_section_14_transactions(uint8_t *buf, size_t cap, size_t off,
                                           sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='transactions'>14. Transaction Archaeology</h2>");

    {
        struct explorer_transaction_stats transaction_stats = {0};
        explorer_query_transaction_stats(db, &transaction_stats);
        struct explorer_op_return_stats op_return_stats = {0};
        explorer_query_op_return_stats(db, &op_return_stats);
        int64_t total_txs = transaction_stats.total;
        int64_t coinbase_txs = transaction_stats.coinbase;
        int64_t non_coinbase = total_txs - coinbase_txs;
        int64_t total_inputs = transaction_stats.inputs;
        int64_t total_outputs = transaction_stats.outputs;
        int64_t total_opret = op_return_stats.total;

        char tx_str[32], cb_str[32], nc_str[32], in_str[32], out_str[32], op_str[32];
        fmt_comma(tx_str, sizeof(tx_str), total_txs);
        fmt_comma(cb_str, sizeof(cb_str), coinbase_txs);
        fmt_comma(nc_str, sizeof(nc_str), non_coinbase);
        fmt_comma(in_str, sizeof(in_str), total_inputs);
        fmt_comma(out_str, sizeof(out_str), total_outputs);
        fmt_comma(op_str, sizeof(op_str), total_opret);

        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), total_txs, total_inputs, "tx_archaeology");

        APPEND(off, r, max,
            "<div class='card'>"
            "<table>"
            "<tr><td><b>Total transactions:</b></td><td>%s</td></tr>"
            "<tr><td><b>Coinbase transactions:</b></td><td>%s</td></tr>"
            "<tr><td><b>Non-coinbase transactions:</b></td><td>%s</td></tr>"
            "<tr><td><b>Total transparent inputs:</b></td><td>%s</td></tr>"
            "<tr><td><b>Total transparent outputs:</b></td><td>%s</td></tr>"
            "<tr><td><b>Total OP_RETURN outputs:</b></td><td>%s</td></tr>"
            "<tr><td><b>Avg outputs/tx:</b></td><td>%.2f</td></tr>"
            "<tr><td><b>SHA3:</b></td><td><code>%s</code></td></tr>"
            "</table></div>",
            tx_str, cb_str, nc_str, in_str, out_str, op_str,
            total_txs > 0 ? (double)total_outputs / (double)total_txs : 0.0,
            rcpt);
    }

    /* Transactions per year */
    APPEND(off, r, max,
        "<h3>Transactions Per Year</h3>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Total Tx</th><th>Coinbase</th>"
        "<th>Non-Coinbase</th><th>Avg Tx/Block</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT CAST(strftime('%Y', b.time, 'unixepoch') AS INTEGER) AS yr, "
            "count(*) AS total, "
            "SUM(CASE WHEN t.is_coinbase = 1 THEN 1 ELSE 0 END) AS cb "
            "FROM transactions t "
            "JOIN blocks b ON t.block_height = b.height "
            "WHERE b.time > 0 GROUP BY yr ORDER BY yr";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                int64_t total = sqlite3_column_int64(s, 1);
                int64_t cb = sqlite3_column_int64(s, 2);
                int64_t nc = total - cb;
                double avg = cb > 0 ? (double)total / (double)cb : 0.0;
                APPEND(off, r, max,
                    "<tr><td>%d</td><td>%" PRId64 "</td>"
                    "<td>%" PRId64 "</td><td>%" PRId64 "</td>"
                    "<td>%.2f</td></tr>",
                    yr, total, cb, nc, avg);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_15_empty_blocks(uint8_t *buf, size_t cap, size_t off,
                                           sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='empty'>15. Empty Blocks Analysis</h2>"
        "<p style='color:#888'>Blocks with only a coinbase transaction "
        "(num_tx = 1, no user transactions).</p>");

    {
        int64_t empty_total = fq_i64(db,
            "SELECT count(*) FROM blocks WHERE num_tx <= 1");
        int64_t total_blocks_2 = fq_i64(db, "SELECT count(*) FROM blocks");
        double empty_pct = total_blocks_2 > 0
            ? (double)empty_total * 100.0 / (double)total_blocks_2 : 0.0;

        char em_str[32], tb_str[32], rcpt[32] = "";
        fmt_comma(em_str, sizeof(em_str), empty_total);
        fmt_comma(tb_str, sizeof(tb_str), total_blocks_2);
        compute_receipt_i64(rcpt, sizeof(rcpt), empty_total, total_blocks_2,
                            "empty_blocks");

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Empty blocks (coinbase only):</b> %s of %s (%.1f%%)</p>"
            "<p><b>SHA3:</b> <code>%s</code></p>"
            "</div>",
            em_str, tb_str, empty_pct, rcpt);
    }

    /* Empty blocks per year */
    APPEND(off, r, max,
        "<h3>Empty Blocks Per Year</h3>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Empty Blocks</th><th>Total Blocks</th><th>%%</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER) AS yr, "
            "SUM(CASE WHEN num_tx <= 1 THEN 1 ELSE 0 END) AS empty, "
            "count(*) AS total "
            "FROM blocks WHERE time > 0 GROUP BY yr ORDER BY yr";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                int64_t empty = sqlite3_column_int64(s, 1);
                int64_t total = sqlite3_column_int64(s, 2);
                double pct = total > 0 ? (double)empty * 100.0 / (double)total : 0.0;
                APPEND(off, r, max,
                    "<tr><td>%d</td><td>%" PRId64 "</td>"
                    "<td>%" PRId64 "</td><td>%.1f%%</td></tr>",
                    yr, empty, total, pct);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_16_difficulty(uint8_t *buf, size_t cap, size_t off,
                                         sqlite3 *db)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='difficulty'>16. Difficulty History</h2>"
        "<p style='color:#888'>Peak difficulty per calendar year.</p>"
        "<table class='txlist'>"
        "<tr><th>Year</th><th>Peak Difficulty</th><th>Block</th><th>SHA3</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        const char *sql =
            "SELECT CAST(strftime('%Y', b1.time, 'unixepoch') AS INTEGER) AS yr, "
            "MIN(b1.bits) AS min_bits, "
            "(SELECT b2.height FROM blocks b2 "
            " WHERE b2.bits = MIN(b1.bits) "
            " AND CAST(strftime('%Y', b2.time, 'unixepoch') AS INTEGER) = "
            "     CAST(strftime('%Y', b1.time, 'unixepoch') AS INTEGER) "
            " LIMIT 1) AS peak_height "
            "FROM blocks b1 "
            "WHERE b1.time > 0 AND b1.bits > 0 "
            "GROUP BY yr ORDER BY yr";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int yr = sqlite3_column_int(s, 0);
                uint32_t bits = (uint32_t)sqlite3_column_int64(s, 1);
                int64_t ph = sqlite3_column_int64(s, 2);
                double diff = explorer_difficulty_from_bits(bits);
                char dstr[64], rcpt[32] = "";
                snprintf(dstr, sizeof(dstr), "%.4f", diff);
                compute_receipt_i64(rcpt, sizeof(rcpt), ph, (int64_t)bits,
                                    "difficulty_peak");
                APPEND(off, r, max,
                    "<tr><td>%d</td><td>%s</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td><code>%s</code></td></tr>",
                    yr, dstr, ph, ph, rcpt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_17_integrity(uint8_t *buf, size_t cap, size_t off,
                                        sqlite3 *db, int64_t chain_height,
                                        int64_t block_count)
{
    char *r = (char *)buf;
    size_t max = cap;

    APPEND(off, r, max,
        "<h2 id='integrity'>17. Data Integrity</h2>");

    struct explorer_transaction_stats integrity_tx_stats = {0};
    explorer_query_transaction_stats(db, &integrity_tx_stats);
    int64_t tx_count = integrity_tx_stats.total;

    char integrity_hash[128] = "";
    {
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);

        sqlite3_stmt *s = NULL;
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT height, hash, time, num_tx, sapling_value, "
            "COALESCE(sprout_value, 0) "
            "FROM blocks WHERE height > %" PRId64 " ORDER BY height",
            chain_height > 100 ? chain_height - 100 : (int64_t)0);

        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                const char *hash = (const char *)sqlite3_column_text(s, 1);
                int64_t t = sqlite3_column_int64(s, 2);
                int64_t ntx = sqlite3_column_int64(s, 3);
                int64_t sv = sqlite3_column_int64(s, 4);
                int64_t spv = sqlite3_column_int64(s, 5);

                /* Pack: height(8) + time(8) + num_tx(4) + sapling_value(8) +
                 *       sprout_value(8) = 36 bytes */
                uint8_t data[36];
                for (int j = 0; j < 8; j++)
                    data[j] = (uint8_t)((h >> (j * 8)) & 0xff);
                for (int j = 0; j < 8; j++)
                    data[8 + j] = (uint8_t)((t >> (j * 8)) & 0xff);
                for (int j = 0; j < 4; j++)
                    data[16 + j] = (uint8_t)((ntx >> (j * 8)) & 0xff);
                for (int j = 0; j < 8; j++)
                    data[20 + j] = (uint8_t)((sv >> (j * 8)) & 0xff);
                for (int j = 0; j < 8; j++)
                    data[28 + j] = (uint8_t)((spv >> (j * 8)) & 0xff);

                sha3_256_write(&ctx, data, 36);
                if (hash)
                    sha3_256_write(&ctx, (const unsigned char *)hash, strlen(hash));
            }
            sqlite3_finalize(s);
        }

        unsigned char digest[32];
        sha3_256_finalize(&ctx, digest);
        for (int i = 0; i < 32 &&
             (size_t)(i * 2 + 2) <= sizeof(integrity_hash); i++) {
            snprintf(integrity_hash + i * 2,
                     sizeof(integrity_hash) - (size_t)(i * 2),
                     "%02x", digest[i]);
        }
    }

    {
        char blk_str[32], tx_str[32];
        fmt_comma(blk_str, sizeof(blk_str), block_count);
        fmt_comma(tx_str, sizeof(tx_str), tx_count);

        APPEND(off, r, max,
            "<div class='card'>"
            "<p><b>Chain height:</b> %" PRId64 "</p>"
            "<p><b>Indexed blocks:</b> %s</p>"
            "<p><b>Indexed transactions:</b> %s</p>"
            "<p><b>SHA3-256 coverage:</b> blocks %" PRId64 " \xe2\x80\x93 %" PRId64
            " (last 100)</p>"
            "<p><b>Integrity hash:</b><br>"
            "<code style='word-break:break-all;color:#33ff99'>%s</code></p>"
            "</div>",
            chain_height, blk_str, tx_str,
            chain_height > 100 ? chain_height - 100 : (int64_t)0, chain_height,
            integrity_hash);
    }

    APPEND(off, r, max,
        "<div class='card' style='margin-top:16px'>"
        "<h3>How to Verify</h3>"
        "<p style='color:#888'>Recompute by replaying blocks from genesis. "
        "Each block's hash chains:</p>"
        "<code style='display:block;padding:12px;background:#0c0c0c;border-radius:4px;"
        "word-break:break-all;color:#ccc'>"
        "rolling_SHA3 += (height_le64 || time_le64 || num_tx_le32 || "
        "sapling_value_le64 || sprout_value_le64 || block_hash_hex_string)"
        "</code>"
        "<p style='color:#888;margin-top:8px'>36 bytes of packed integers + "
        "variable-length hex hash string per block, fed sequentially into SHA3-256.</p>"
        "<p style='color:#888;margin-top:12px'>Milestone receipts: "
        "<code>SHA3(height_le64 || block_hash_hex || fact_name_ascii)</code> "
        "\xe2\x80\x94 first 16 hex chars.</p>"
        "<p style='color:#888'>Record receipts: "
        "<code>SHA3(val1_le64 || val2_le64 || label_ascii)</code> "
        "\xe2\x80\x94 first 16 hex chars.</p>"
        "</div>");
    return off;
}

/* ── Build the factoids page (HTML) ──────────────────────── */

size_t explorer_factoids_build(uint8_t *buf, size_t buf_max, const char *datadir)
{
    if (!buf || buf_max < 1024 || !datadir) return 0;

    sqlite3 *db = NULL;
    if (!explorer_open_readonly_db(datadir, &db)) {
        return 0;
    }

    struct explorer_chain_stats chain_stats = {0};
    explorer_query_chain_stats(db, &chain_stats);
    int64_t chain_height = chain_stats.height;
    int64_t utxo_tip = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    if (utxo_tip > chain_height)
        chain_height = utxo_tip;

    struct explorer_history_validation history;
    explorer_validate_block_history(db, chain_height, &history);
    if (!history.usable) {
        size_t len = explorer_factoids_build_verified_summary(
            buf, buf_max, db, chain_height, history.reason);
        sqlite3_close(db);
        return len;
    }

    size_t off = 0;
    char *r = (char *)buf;
    size_t max = buf_max;

    /* ── HTTP header + HTML head ──────────────────────────── */
    APPEND(off, r, max, EXPLORER_HEADER("ZClassic Historian Factoids"));
    off += explorer_emit_nav((char *)r + off, max - off, "factoids");
    APPEND(off, r, max,
        "<div class='content'>"
        "<h1>ZClassic Historian Factoids</h1>"
        "<p style='color:#888'>Deep chain archaeology with SHA3-256 data receipts. "
        "Every fact hashed: <code>SHA3(height_le64 || block_hash || fact_name)</code>. "
        "First 16 hex chars shown. Independently verifiable from raw chain data.</p>"
        "<p style='color:#555;font-size:0.85em'>Chain height: %" PRId64
        " | All timestamps UTC | All hashes big-endian display order</p>",
        chain_height);

    off = emit_section_1_genesis(buf, buf_max, off, db);
    off = emit_section_2_upgrades(buf, buf_max, off, db);
    off = emit_section_3_mining_eras(buf, buf_max, off, chain_height);
    off = emit_section_4_milestones(buf, buf_max, off, db, chain_height);
    off = emit_section_5_records(buf, buf_max, off, db);
    off = emit_section_6_supply(buf, buf_max, off, db, chain_height);
    off = emit_section_7_addresses(buf, buf_max, off, db);
    off = emit_section_8_privacy(buf, buf_max, off, db);
    off = emit_section_9_zslp(buf, buf_max, off, db);
    off = emit_section_10_opreturn(buf, buf_max, off, db);
    off = emit_section_11_dust(buf, buf_max, off, db);
    off = emit_section_12_checkpoints(buf, buf_max, off, db, chain_height);
    off = emit_section_13_blocktimes(buf, buf_max, off, db, chain_height);
    off = emit_section_14_transactions(buf, buf_max, off, db);
    off = emit_section_15_empty_blocks(buf, buf_max, off, db);
    off = emit_section_16_difficulty(buf, buf_max, off, db);
    off = emit_section_17_integrity(buf, buf_max, off, db, chain_height,
                                    chain_stats.blocks);

    /* ── Table of Contents (anchor links) ────────────────────── */
    APPEND(off, r, max,
        "<div class='card' style='margin-top:16px'>"
        "<h3>Quick Navigation</h3>"
        "<p>"
        "<a href='#genesis'>1. Genesis</a> | "
        "<a href='#upgrades'>2. Upgrades</a> | "
        "<a href='#mining-eras'>3. Mining Eras</a> | "
        "<a href='#milestones'>4. Milestones</a> | "
        "<a href='#records'>5. Records</a> | "
        "<a href='#supply'>6. Supply</a> | "
        "<a href='#addresses'>7. Addresses</a> | "
        "<a href='#privacy'>8. Privacy</a> | "
        "<a href='#zslp'>9. ZSLP</a> | "
        "<a href='#opreturn'>10. OP_RETURN</a> | "
        "<a href='#dust'>11. UTXO</a> | "
        "<a href='#checkpoints'>12. Checkpoints</a> | "
        "<a href='#blocktimes'>13. Block Times</a> | "
        "<a href='#transactions'>14. Transactions</a> | "
        "<a href='#empty'>15. Empty Blocks</a> | "
        "<a href='#difficulty'>16. Difficulty</a> | "
        "<a href='#integrity'>17. Integrity</a>"
        "</p></div>");

    /* ── Close page ───────────────────────────────────────── */
    APPEND(off, r, max, "</div>" EXPLORER_FOOTER);

    sqlite3_close(db);

    printf("Factoids: built %zu bytes, 17 sections\n", off);
    fflush(stdout);
    return off;
}

/* ================================================================
 * JSON API: /api/factoids
 * ================================================================ */

size_t explorer_factoids_build_json(uint8_t *buf, size_t buf_max,
                                     const char *datadir)
{
    if (!buf || buf_max < 512 || !datadir) return 0;

    sqlite3 *db = NULL;
    if (!explorer_open_readonly_db(datadir, &db)) {
        return 0;
    }

    struct explorer_chain_stats chain_stats = {0};
    explorer_query_chain_stats(db, &chain_stats);
    int64_t chain_height = chain_stats.height;
    int64_t utxo_tip = fq_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    if (utxo_tip > chain_height)
        chain_height = utxo_tip;

    size_t off = 0;
    char *r = (char *)buf;
    size_t max = buf_max;

    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n");

    struct explorer_history_validation history;
    explorer_validate_block_history(db, chain_height, &history);
    if (!history.usable) {
        int64_t block_rows = fq_i64(db, "SELECT count(*) FROM blocks");
        int64_t tx_rows = fq_i64(db, "SELECT count(*) FROM transactions");
        int64_t utxo_count = fq_i64(db, "SELECT count(*) FROM utxos");
        int64_t utxo_value = fq_i64(db, "SELECT COALESCE(SUM(value),0) FROM utxos");
        int64_t supply = compute_supply_at_height(chain_height);
        APPEND(off, r, max,
            "{\"history_index_usable\":false,"
            "\"unsafe_sections_suppressed\":true,"
            "\"reason\":\"%s\","
            "\"chain_height\":%" PRId64 ","
            "\"supply\":{\"total_sat\":%" PRId64 ",\"total_zcl\":%.8f},"
            "\"utxo\":{\"count\":%" PRId64 ",\"value_sat\":%" PRId64 ",\"value_zcl\":%.8f},"
            "\"index\":{\"blocks\":%" PRId64 ",\"transactions\":%" PRId64 "},"
            "\"consensus\":{\"genesis_hash\":\"%s\","
            "\"genesis_time\":%" PRId64 ",\"mainnet_port\":8033,\"buttercup_height\":707000}}",
            history.reason, chain_height,
            supply, (double)supply / (double)ZATOSHI_PER_ZCL,
            utxo_count, utxo_value, (double)utxo_value / (double)ZATOSHI_PER_ZCL,
            block_rows, tx_rows, ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX,
            (int64_t)ZCL_EXPLORER_GENESIS_TIME);
        sqlite3_close(db);
        return off;
    }

    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);

    APPEND(off, r, max, "{\"chain_height\":%" PRId64, chain_height);

    /* Genesis — height 0 may not be in SQLite, use constants as fallback */
    {
        char gh[128] = ZCL_EXPLORER_GENESIS_HASH_DISPLAY_HEX, gc[128] = "";
        fq_text(db, "SELECT hex(txid) FROM transactions WHERE block_height = 0 AND is_coinbase = 1 LIMIT 1",
                gc, sizeof(gc));
        if (!gc[0]) snprintf(gc, sizeof(gc),
            "427DBF0AE8E079C6527EA1CB308C6E3C98FA5435F4D715D31176EA00CF2B6119");
        char rcpt[32] = "";
        compute_receipt(rcpt, sizeof(rcpt), 0, gh, "Genesis");
        APPEND(off, r, max,
            ",\"genesis\":{\"hash\":\"%.64s\",\"timestamp\":%" PRId64 ","
            "\"coinbase_txid\":\"%.64s\",\"sha3\":\"%s\"}",
            gh, (int64_t)ZCL_EXPLORER_GENESIS_TIME, gc, rcpt);
    }

    /* Network upgrades */
    APPEND(off, r, max, ",\"upgrades\":[");
    {
        struct { const char *name; int64_t h; int proto; const char *bid; } ups[] = {
            {"Sprout",0,170002,"0x00000000"},
            {"Overwinter",476969,170005,"0x5ba81b19"},
            {"Sapling",476969,170007,"0x76b809bb"},
            {"Bubbles",585318,170009,"0x821a451c"},
            {"DiffAdj",585322,170010,"0x930b540d"},
            {"Buttercup",707000,170011,"0x930b540d"},
        };
        for (int i = 0; i < 6; i++) {
            char bh[128] = ""; int64_t bt = 0;
            get_block_at(db, ups[i].h, bh, sizeof(bh), &bt);
            char rcpt[32] = "";
            compute_receipt(rcpt, sizeof(rcpt), ups[i].h, bh, ups[i].name);
            if (i > 0) APPEND(off, r, max, ",");
            APPEND(off, r, max,
                "{\"name\":\"%s\",\"height\":%" PRId64 ",\"time\":%" PRId64
                ",\"protocol\":%d,\"branch_id\":\"%s\",\"sha3\":\"%s\"}",
                ups[i].name, ups[i].h, bt, ups[i].proto, ups[i].bid, rcpt);
        }
    }
    APPEND(off, r, max, "]");

    /* Supply */
    {
        int64_t supply = compute_supply_at_height(chain_height);
        char rcpt[32] = "";
        compute_receipt_i64(rcpt, sizeof(rcpt), chain_height, supply, "total_supply");
        APPEND(off, r, max,
            ",\"supply\":{\"total_sat\":%" PRId64 ",\"total_zcl\":%.8f,\"sha3\":\"%s\"}",
            supply, (double)supply / (double)ZATOSHI_PER_ZCL, rcpt);
    }

    /* Address stats */
    {
        struct explorer_address_stats address_stats = {0};
        explorer_query_address_stats(db, &address_stats);
        APPEND(off, r, max,
            ",\"addresses\":{\"total\":%" PRId64 ",\"nonzero\":%" PRId64 "}",
            address_stats.total, address_stats.nonzero);
    }

    /* Privacy stats */
    {
        struct explorer_privacy_stats privacy_stats = {0};
        explorer_query_privacy_stats(db, &privacy_stats);
        APPEND(off, r, max,
            ",\"privacy\":{\"joinsplits\":%" PRId64 ",\"sapling_spends\":%" PRId64
            ",\"sapling_outputs\":%" PRId64 ",\"net_shielded_sat\":%" PRId64 "}",
            privacy_stats.joinsplits, privacy_stats.sapling_spends,
            privacy_stats.sapling_outputs, privacy_stats.net_shielded_sat);
    }

    /* ZSLP */
    {
        APPEND(off, r, max,
            ",\"zslp\":{\"tokens\":%" PRId64 ",\"transfers\":%" PRId64 "}",
            token_stats.token_count, token_stats.transfer_count);
    }

    /* UTXO stats */
    {
        struct explorer_utxo_stats utxo_stats = {0};
        explorer_query_utxo_stats(db, &utxo_stats);
        APPEND(off, r, max,
            ",\"utxo\":{\"count\":%" PRId64 ",\"dust_under_0001\":%" PRId64
            ",\"total_value_sat\":%" PRId64 "}",
            utxo_stats.count, utxo_stats.dust_under_0001,
            utxo_stats.total_value_sat);
    }

    /* OP_RETURN stats */
    {
        struct explorer_op_return_stats op_return_stats = {0};
        explorer_query_op_return_stats(db, &op_return_stats);
        APPEND(off, r, max,
            ",\"op_returns\":{\"total\":%" PRId64 ",\"zslp\":%" PRId64
            ",\"other\":%" PRId64 "}",
            op_return_stats.total, op_return_stats.zslp,
            op_return_stats.total - op_return_stats.zslp);
    }

    /* Transaction stats */
    {
        struct explorer_transaction_stats transaction_stats = {0};
        explorer_query_transaction_stats(db, &transaction_stats);
        APPEND(off, r, max,
            ",\"transactions\":{\"total\":%" PRId64 ",\"coinbase\":%" PRId64
            ",\"inputs\":%" PRId64 ",\"outputs\":%" PRId64
            ",\"empty_blocks\":%" PRId64 "}",
            transaction_stats.total, transaction_stats.coinbase,
            transaction_stats.inputs, transaction_stats.outputs,
            transaction_stats.empty_blocks);
    }

    /* Integrity hash */
    {
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        sqlite3_stmt *s = NULL;
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT height, hash, time, num_tx, sapling_value, "
            "COALESCE(sprout_value, 0) "
            "FROM blocks WHERE height > %" PRId64 " ORDER BY height",
            chain_height > 100 ? chain_height - 100 : (int64_t)0);
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                const char *hash = (const char *)sqlite3_column_text(s, 1);
                int64_t t = sqlite3_column_int64(s, 2);
                int64_t ntx = sqlite3_column_int64(s, 3);
                int64_t sv = sqlite3_column_int64(s, 4);
                int64_t spv = sqlite3_column_int64(s, 5);
                uint8_t data[36];
                for (int j = 0; j < 8; j++) data[j] = (uint8_t)((h >> (j*8)) & 0xff);
                for (int j = 0; j < 8; j++) data[8+j] = (uint8_t)((t >> (j*8)) & 0xff);
                for (int j = 0; j < 4; j++) data[16+j] = (uint8_t)((ntx >> (j*8)) & 0xff);
                for (int j = 0; j < 8; j++) data[20+j] = (uint8_t)((sv >> (j*8)) & 0xff);
                for (int j = 0; j < 8; j++) data[28+j] = (uint8_t)((spv >> (j*8)) & 0xff);
                sha3_256_write(&ctx, data, 36);
                if (hash) sha3_256_write(&ctx, (const unsigned char *)hash, strlen(hash));
            }
            sqlite3_finalize(s);
        }
        unsigned char digest[32];
        sha3_256_finalize(&ctx, digest);
        char ih[128] = "";
        compute_full_hash(ih, sizeof(ih), digest, 32);
        APPEND(off, r, max,
            ",\"integrity\":{\"blocks\":%" PRId64 ",\"hash\":\"%s\"}", chain_height, ih);
    }

    APPEND(off, r, max, "}");

    sqlite3_close(db);
    return off;
}
