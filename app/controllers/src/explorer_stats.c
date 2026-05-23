/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer stats page — comprehensive blockchain statistics.
 * Extracted from explorer_controller.c for maintainability.
 *
 * Performance: All aggregate data gathered in ~6 consolidated queries
 * (single-pass scans) instead of dozens of individual queries.
 * Chart data uses batch SELECT with height ranges. */

#include "platform/time_compat.h"
#include "controllers/explorer_stats.h"
#include "controllers/explorer_internal.h"
#include "chain/chainparams.h"
#include "keys/key_io.h"
#include "models/hodl_wave.h"
#include "script/standard.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

/* sql_query_i64() provided by controllers/explorer_internal.h */
#define stats_q_i64 sql_query_i64

/* Generate CSS for tab sections — each section needs unique IDs */
static void stats_tab_css(char *r, size_t max, size_t *off,
                            const char *prefix, const char *color,
                            int num_tabs, const char *tab_ids[])
{
    if (!r || !off || !prefix || !color || !tab_ids) return;
    for (int i = 0; i < num_tabs; i++) {
        APPEND(*off, r, max,
            "#%s%s:checked ~ .tab-bar label[for=%s%s]"
            "{background:#111;color:%s;border-bottom-color:#111}",
            prefix, tab_ids[i], prefix, tab_ids[i], color);
    }
    for (int i = 0; i < num_tabs; i++) {
        APPEND(*off, r, max,
            "#%s%s:checked ~ #p-%s%s{display:block}",
            prefix, tab_ids[i], prefix, tab_ids[i]);
    }
}

/* ── Address encoding helper ─────────────────────────────── */

static bool stats_addr_encode(char *out, size_t outmax,
                              const struct tx_destination *dest)
{
    if (!out || outmax == 0 || !dest) LOG_FAIL("explorer_stats", "stats_addr_encode: NULL out, zero outmax, or NULL dest");
    const struct chain_params *cp = chain_params_get();
    if (!cp) LOG_FAIL("explorer_stats", "stats_addr_encode: chain_params_get returned NULL");
    size_t pk_len = 0, sh_len = 0;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sh_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sh_len);
    return encode_destination(dest, pk_pfx, pk_len, sh_pfx, sh_len, out, outmax);
}

/* ── Consolidated data structures ────────────────────────── */

struct shielded_stats {
    int64_t block_count;
    int64_t sum_pos;        /* total unshielded (z->t), positive */
    int64_t sum_neg;        /* total shielded (t->z), negative */
    int64_t first_height;
    int64_t last_height;
    int64_t first_time;
    int64_t last_time;
    int64_t peak_pos_height;
    int64_t peak_pos_value;
    int64_t peak_neg_height;
    int64_t peak_neg_value; /* negative */
};

/* Single-pass query for all sprout or sapling stats */
static void query_shielded_stats(sqlite3 *db, const char *col,
                                  struct shielded_stats *out)
{
    memset(out, 0, sizeof(*out));
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT count(*), "
        "COALESCE(SUM(CASE WHEN %s>0 THEN %s ELSE 0 END),0), "
        "COALESCE(SUM(CASE WHEN %s<0 THEN %s ELSE 0 END),0), "
        "MIN(height), MAX(height), "
        "MIN(time), MAX(time) "
        "FROM blocks WHERE %s != 0",
        col, col, col, col, col);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            out->block_count    = sqlite3_column_int64(s, 0);
            out->sum_pos        = sqlite3_column_int64(s, 1);
            out->sum_neg        = sqlite3_column_int64(s, 2);
            out->first_height   = sqlite3_column_int64(s, 3);
            out->last_height    = sqlite3_column_int64(s, 4);
            out->first_time     = sqlite3_column_int64(s, 5);
            out->last_time      = sqlite3_column_int64(s, 6);
        }
        sqlite3_finalize(s);
    }
    /* Peak positive */
    snprintf(sql, sizeof(sql),
        "SELECT height, %s FROM blocks WHERE %s > 0 "
        "ORDER BY %s DESC LIMIT 1", col, col, col);
    s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            out->peak_pos_height = sqlite3_column_int64(s, 0);
            out->peak_pos_value  = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }
    /* Peak negative */
    snprintf(sql, sizeof(sql),
        "SELECT height, %s FROM blocks WHERE %s < 0 "
        "ORDER BY %s ASC LIMIT 1", col, col, col);
    s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            out->peak_neg_height = sqlite3_column_int64(s, 0);
            out->peak_neg_value  = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }
}

/* Render shielded section HTML (sprout or sapling).
 * pos_is_shielding: true for Sprout (positive sprout_value = shielding),
 *                   false for Sapling (positive value_balance = unshielding). */
static void render_shielded_section(char *r, size_t max, size_t *off,
    const char *title, const char *desc, const struct shielded_stats *ss,
    int64_t nullifier_count, /* -1 to skip */
    sqlite3 *db, const char *col, int start_year, bool pos_is_shielding)
{
    char in_str[32], out_str[32], peak_str[32], peak_neg_str[32], net_str[32];
    /* Sign conventions differ: Sprout pos=shielding, Sapling pos=unshielding */
    int64_t total_shielded, total_unshielded, peak_shield_val, peak_unshield_val;
    int64_t peak_shield_h, peak_unshield_h;
    if (pos_is_shielding) {
        /* Sprout: positive = shielding, negative = unshielding */
        total_shielded = ss->sum_pos;
        total_unshielded = ss->sum_neg < 0 ? -ss->sum_neg : ss->sum_neg;
        peak_shield_val = ss->peak_pos_value;
        peak_shield_h = ss->peak_pos_height;
        peak_unshield_val = ss->peak_neg_value < 0 ? -ss->peak_neg_value : ss->peak_neg_value;
        peak_unshield_h = ss->peak_neg_height;
    } else {
        /* Sapling: positive = unshielding, negative = shielding */
        total_shielded = ss->sum_neg < 0 ? -ss->sum_neg : ss->sum_neg;
        total_unshielded = ss->sum_pos;
        peak_shield_val = ss->peak_neg_value < 0 ? -ss->peak_neg_value : ss->peak_neg_value;
        peak_shield_h = ss->peak_neg_height;
        peak_unshield_val = ss->peak_pos_value;
        peak_unshield_h = ss->peak_pos_height;
    }
    explorer_format_zcl(in_str, sizeof(in_str), total_shielded);
    explorer_format_zcl(out_str, sizeof(out_str), total_unshielded);
    explorer_format_zcl(peak_str, sizeof(peak_str), peak_shield_val);
    explorer_format_zcl(peak_neg_str, sizeof(peak_neg_str), peak_unshield_val);
    int64_t net = (int64_t)total_shielded - (int64_t)total_unshielded;
    explorer_format_zcl(net_str, sizeof(net_str), net);

    char first_ts[32] = "N/A", last_ts[32] = "N/A";
    if (ss->first_time > 0) explorer_format_time(first_ts, sizeof(first_ts), (uint32_t)ss->first_time);
    if (ss->last_time > 0) explorer_format_time(last_ts, sizeof(last_ts), (uint32_t)ss->last_time);

    APPEND(*off, r, max,
        "<h2>%s</h2>"
        "<p style='color:#888;margin:-4px 0 12px;font-size:14px'>%s</p>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Blocks with Activity</div></div>"
        "<div class='stat'><div class='num'>%s ZCL</div>"
        "<div class='lbl'>Net Pool Balance</div></div>",
        title, desc, ss->block_count, net_str);
    if (nullifier_count >= 0) {
        APPEND(*off, r, max,
            "<div class='stat'><div class='num'>%" PRId64 "</div>"
            "<div class='lbl'>Nullifiers</div></div>",
            nullifier_count);
    }
    APPEND(*off, r, max, "</div>");

    APPEND(*off, r, max,
        "<div class='card'><div class='grid'>"
        "<div class='label'>Total Shielded (t&rarr;z)</div>"
        "<div class='val amount'>%s ZCL</div>"
        "<div class='label'>Total Unshielded (z&rarr;t)</div>"
        "<div class='val amount'>%s ZCL</div>"
        "<div class='label'>First Active Block</div>"
        "<div class='val'><a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a>"
        " &mdash; %s</div>"
        "<div class='label'>Last Active Block</div>"
        "<div class='val'><a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a>"
        " &mdash; %s</div>",
        in_str, out_str,
        ss->first_height, ss->first_height, first_ts,
        ss->last_height, ss->last_height, last_ts);

    if (peak_shield_val > 0) {
        APPEND(*off, r, max,
            "<div class='label'>Largest Single Shielding</div>"
            "<div class='val'><a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a>"
            " &mdash; %s ZCL</div>",
            peak_shield_h, peak_shield_h, peak_str);
    }
    if (peak_unshield_val > 0) {
        APPEND(*off, r, max,
            "<div class='label'>Largest Single Unshielding</div>"
            "<div class='val'><a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a>"
            " &mdash; %s ZCL</div>",
            peak_unshield_h, peak_unshield_h, peak_neg_str);
    }
    APPEND(*off, r, max, "</div></div>");

    /* Yearly breakdown — single query with GROUP BY */
    APPEND(*off, r, max,
        "<h3>Activity by Year</h3>"
        "<table><tr><th>Year</th><th>Blocks</th>"
        "<th>Shielded (t&rarr;z)</th><th>Unshielded (z&rarr;t)</th>"
        "<th>Net Change</th></tr>");
    {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT CAST(strftime('%%Y', time, 'unixepoch') AS INTEGER) AS yr, "
            "count(*), "
            "COALESCE(SUM(CASE WHEN %s>0 THEN %s ELSE 0 END),0), "
            "COALESCE(SUM(CASE WHEN %s<0 THEN %s ELSE 0 END),0) "
            "FROM blocks WHERE %s != 0 AND time > 0 "
            "GROUP BY yr ORDER BY yr",
            col, col, col, col, col);
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int year = sqlite3_column_int(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                int64_t shield = sqlite3_column_int64(s, 2);
                int64_t unshield = sqlite3_column_int64(s, 3);
                if (cnt > 0 && year >= start_year) {
                    char sh[32], un[32], nt[32];
                    int64_t yr_shield, yr_unshield;
                    if (pos_is_shielding) {
                        yr_shield = shield;
                        yr_unshield = unshield < 0 ? -unshield : unshield;
                    } else {
                        yr_shield = unshield < 0 ? -unshield : unshield;
                        yr_unshield = shield;
                    }
                    explorer_format_zcl(sh, sizeof(sh), yr_shield);
                    explorer_format_zcl(un, sizeof(un), yr_unshield);
                    explorer_format_zcl(nt, sizeof(nt), yr_shield - yr_unshield);
                    APPEND(*off, r, max,
                        "<tr><td>%d</td><td>%" PRId64 "</td>"
                        "<td class='amount'>%s</td>"
                        "<td class='amount'>%s</td>"
                        "<td class='amount'>%s</td></tr>",
                        year, cnt, sh, un, nt);
                }
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(*off, r, max, "</table>");

    /* Top 20 by absolute value */
    APPEND(*off, r, max,
        "<h3>Top 20 Largest Transactions (by block)</h3>"
        "<table><tr><th>Block</th><th>Time</th><th>Value</th>"
        "<th>Direction</th></tr>");
    {
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT height, time, %s FROM blocks "
            "WHERE %s != 0 ORDER BY ABS(%s) DESC LIMIT 20",
            col, col, col);
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                int64_t v = sqlite3_column_int64(s, 2);
                char ts[32], vs[32];
                explorer_format_time(ts, sizeof(ts), (uint32_t)t);
                explorer_format_zcl(vs, sizeof(vs), v < 0 ? -v : v);
                APPEND(*off, r, max,
                    "<tr><td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td><td class='amount'>%s ZCL</td>"
                    "<td>%s</td></tr>",
                    h, h, ts, vs,
                    (pos_is_shielding ? v > 0 : v < 0)
                        ? "<span style='color:#33ff99'>Shielding</span>"
                        : "<span style='color:#ff6666'>Unshielding</span>");
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(*off, r, max, "</table>");
}

/* Emit a tabbed chart set */
static void render_tabbed_chart(char *r, size_t max, size_t *off,
    const char *heading, const char *tab_name, const char *color,
    double data[5][40], char labels[5][40][20],
    const char *y_label, const char *range_ids[])
{
    APPEND(*off, r, max,
        "<h2>%s</h2><div class='tabs'>", heading);
    for (int i = 0; i < 5; i++)
        APPEND(*off, r, max,
            "<input type='radio' name='%stab' id='%s%s'%s>",
            tab_name, tab_name, range_ids[i], i == 2 ? " checked" : "");
    APPEND(*off, r, max, "<div class='tab-bar'>");
    const char *tab_labels[] = {"24h","7 Days","30 Days","1 Year","All Time"};
    for (int i = 0; i < 5; i++)
        APPEND(*off, r, max,
            "<label for='%s%s'>%s</label>",
            tab_name, range_ids[i], tab_labels[i]);
    APPEND(*off, r, max, "</div>");
    for (int ri = 0; ri < 5; ri++) {
        APPEND(*off, r, max, "<div class='panel' id='p-%s%s'>", tab_name, range_ids[ri]);
        explorer_svg_line_chart(r, max, off, "", color, data[ri], labels[ri], 40, y_label);
        APPEND(*off, r, max, "</div>");
    }
    APPEND(*off, r, max, "</div>");
}

static size_t explorer_stats_build_verified_summary(uint8_t *r, size_t max,
                                                    sqlite3 *db)
{
    size_t off = 0;
    int64_t chain_height = stats_q_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    int64_t block_rows = stats_q_i64(db, "SELECT count(*) FROM blocks");
    int64_t tx_rows = stats_q_i64(db, "SELECT count(*) FROM transactions");
    int64_t utxo_count = stats_q_i64(db, "SELECT count(*) FROM utxos");
    int64_t utxo_value = stats_q_i64(db, "SELECT COALESCE(SUM(value),0) FROM utxos");
    int64_t dust = stats_q_i64(db, "SELECT count(*) FROM utxos WHERE value < 100000");
    int64_t addresses = stats_q_i64(db, "SELECT count(*) FROM addresses");
    int64_t nonzero_addresses = stats_q_i64(db,
        "SELECT count(*) FROM addresses WHERE balance > 0");
    int64_t supply = zcl_total_supply_zatoshi(chain_height);

    char supply_str[64], utxo_str[64];
    explorer_format_zcl(supply_str, sizeof(supply_str), supply);
    explorer_format_zcl(utxo_str, sizeof(utxo_str), utxo_value);

    APPEND(off, (char *)r, max, EXPLORER_HEADER("ZClassic Stats"));
    off += explorer_emit_nav((char *)r + off, max - off, "stats");
    APPEND(off, (char *)r, max,
        "<h1>ZClassic Stats</h1>"
        "<h2>Current State</h2>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Current Height</div></div>"
        "<div class='stat'><div class='num'>%s ZCL</div><div class='lbl'>Consensus Supply</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>UTXOs</div></div>"
        "</div>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s ZCL</div><div class='lbl'>Transparent UTXO Value</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Dust UTXOs</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Nonzero Addresses</div></div>"
        "</div>"
        "<h2>Index Coverage</h2>"
        "<table class='txlist'>"
        "<tr><th>Table</th><th>Rows</th></tr>"
        "<tr><td>blocks</td><td>%" PRId64 "</td></tr>"
        "<tr><td>transactions</td><td>%" PRId64 "</td></tr>"
        "<tr><td>utxos</td><td>%" PRId64 "</td></tr>"
        "<tr><td>addresses</td><td>%" PRId64 "</td></tr>"
        "</table>"
        "<p style='color:#888;font-size:0.9em;margin-top:14px'>"
        "Some historical charts (per-year transactions, difficulty over "
        "time, empty-block ratio) require a fully populated block-history "
        "index. The current datadir has a partial index pending backfill; "
        "those charts will appear once the indexer completes."
        "</p>"
        EXPLORER_FOOTER,
        chain_height, supply_str, utxo_count,
        utxo_str, dust, nonzero_addresses,
        block_rows, tx_rows, utxo_count, addresses);
    return off;
}

/* ── Shared structs for gather + emit helpers ── */

struct stats_ctx {
    /* Tip + difficulty */
    int     tip;
    double  diff;
    int64_t tip_time;
    int64_t genesis_time;
    int64_t chain_age_days;

    /* Block aggregates */
    int64_t total_blocks, total_block_txs, empty_blocks;
    int64_t max_tx_count, max_tx_block;
    uint32_t min_bits, max_bits;
    int64_t min_bits_height, max_bits_height;
    double  min_diff, max_diff;
    double  avg_tx_per_block;

    /* UTXO aggregates */
    int64_t total_supply, utxo_count_val;
    int64_t utxo_dust, utxo_small, utxo_medium, utxo_large, utxo_whale;
    int64_t utxo_max_value, utxo_min_height;
    int64_t utxo_coinbase_count, utxo_coinbase_value;
    double  utxo_avg;

    /* Tx counts */
    int64_t total_txs, coinbase_txs;

    /* Shielded pools */
    struct shielded_stats sprout, sapling;
    int64_t nullifier_count;

    /* Address stats */
    int64_t total_addresses, addr_nonzero;
    int64_t top10_balance, top100_balance;

    /* ZSLP / OP_RETURN */
    int64_t opret_count, slp_opret_count;
    int64_t zslp_token_count, zslp_transfer_count;

    /* Deep chain: tx I/O */
    int64_t total_outputs, total_inputs;
    int64_t p2pkh_outputs, p2sh_outputs;
    int64_t max_output_value, total_value_moved;

    /* Deep chain: shielded */
    int64_t total_joinsplits, total_vpub_old, total_vpub_new;
    int64_t total_sap_spends, total_sap_outputs;
    int64_t total_sprout_nulls, unique_sap_anchors;

    /* SHA3 integrity */
    int64_t integrity_count, integrity_min_h, integrity_max_h;
    char    integrity_latest_hash[130];

    /* Firsts & records */
    int64_t first_noncoinbase, first_joinsplit_h, first_sapling_h;
    int64_t first_opreturn_h, first_zslp_h;
    int64_t most_js_block, most_js_count;
    int64_t most_sap_out_block, most_sap_out_count;
    int64_t fastest_block_h, fastest_block_gap;
    int64_t slowest_block_h, slowest_block_gap;

    /* Derived */
    char    hr_str[64];
    char    supply_str[32];
    int     halvings, next_halving, blocks_until_halving;
    int64_t current_subsidy;
    double  pct_mined;
    int64_t t_query_ms;
};

struct stats_chart_data {
    double diff_data[5][40], hr_data[5][40];
    double sprout_c[5][40], sapling_c[5][40], txcount[5][40];
    char   labels[5][40][20];
};

/* ── Gather helpers (extracted from explorer_stats_build phase 1) ── */


/* Phase 1h: deep chain queries (tx_outputs, joinsplits, sapling_*,
 * sprout_nullifiers, view_integrity, firsts & records). Writes
 * directly into the supplied stats_ctx. */
static void gather_deep_chain_data(sqlite3 *db, struct stats_ctx *c)
{
    printf("Stats: querying deep chain data...\n"); fflush(stdout);

    /* Transaction I/O */
    c->total_outputs = stats_q_i64(db, "SELECT count(*) FROM tx_outputs");
    c->total_inputs  = stats_q_i64(db, "SELECT count(*) FROM tx_inputs");
    c->p2pkh_outputs = stats_q_i64(db, "SELECT count(*) FROM tx_outputs WHERE script_type=0");
    c->p2sh_outputs  = stats_q_i64(db, "SELECT count(*) FROM tx_outputs WHERE script_type=1");
    c->max_output_value = stats_q_i64(db, "SELECT MAX(value) FROM tx_outputs");
    c->total_value_moved = stats_q_i64(db, "SELECT COALESCE(SUM(value),0) FROM tx_outputs");

    /* Shielded deep dive (per-tx tables) */
    c->total_joinsplits   = stats_q_i64(db, "SELECT count(*) FROM joinsplits");
    c->total_vpub_old     = stats_q_i64(db, "SELECT COALESCE(SUM(vpub_old),0) FROM joinsplits");
    c->total_vpub_new     = stats_q_i64(db, "SELECT COALESCE(SUM(vpub_new),0) FROM joinsplits");
    c->total_sap_spends   = stats_q_i64(db, "SELECT count(*) FROM sapling_spends");
    c->total_sap_outputs  = stats_q_i64(db, "SELECT count(*) FROM sapling_outputs");
    c->total_sprout_nulls = stats_q_i64(db, "SELECT count(*) FROM sprout_nullifiers");
    c->unique_sap_anchors = stats_q_i64(db, "SELECT count(DISTINCT anchor) FROM sapling_spends");

    /* SHA3 integrity chain */
    c->integrity_count = stats_q_i64(db, "SELECT count(*) FROM view_integrity");
    c->integrity_min_h = stats_q_i64(db, "SELECT MIN(height) FROM view_integrity");
    c->integrity_max_h = stats_q_i64(db, "SELECT MAX(height) FROM view_integrity");
    snprintf(c->integrity_latest_hash, sizeof(c->integrity_latest_hash), "N/A");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT hex(sha3_hash) FROM view_integrity ORDER BY height DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const char *h = (const char *)sqlite3_column_text(s, 0);
                if (h) snprintf(c->integrity_latest_hash, sizeof(c->integrity_latest_hash), "%s", h);
            }
            sqlite3_finalize(s);
        }
    }

    /* Chain firsts & records */
    c->first_noncoinbase = stats_q_i64(db,
        "SELECT MIN(block_height) FROM transactions WHERE is_coinbase=0");
    struct explorer_first_privacy_heights first_privacy = {0};
    explorer_query_first_privacy_heights(db, &first_privacy);
    c->first_joinsplit_h = first_privacy.joinsplit_height;
    c->first_sapling_h = first_privacy.sapling_height;
    c->first_opreturn_h = stats_q_i64(db,
        "SELECT MIN(block_height) FROM op_returns");
    c->first_zslp_h = stats_q_i64(db,
        "SELECT MIN(genesis_height) FROM zslp_tokens");

    c->most_js_block = 0; c->most_js_count = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT block_height, count(*) as cnt FROM joinsplits "
                "GROUP BY block_height ORDER BY cnt DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->most_js_block = sqlite3_column_int64(s, 0);
                c->most_js_count = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    c->most_sap_out_block = 0; c->most_sap_out_count = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT block_height, count(*) as cnt FROM sapling_outputs "
                "GROUP BY block_height ORDER BY cnt DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->most_sap_out_block = sqlite3_column_int64(s, 0);
                c->most_sap_out_count = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    c->fastest_block_h = 0; c->fastest_block_gap = 0;
    c->slowest_block_h = 0; c->slowest_block_gap = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT b2.height, b2.time - b1.time as gap "
                "FROM blocks b1 JOIN blocks b2 ON b2.height = b1.height + 1 "
                "WHERE b1.time > 0 AND b2.time > b1.time "
                "ORDER BY gap ASC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->fastest_block_h   = sqlite3_column_int64(s, 0);
                c->fastest_block_gap = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
        s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT b2.height, b2.time - b1.time as gap "
                "FROM blocks b1 JOIN blocks b2 ON b2.height = b1.height + 1 "
                "WHERE b1.time > 0 AND b2.time > b1.time "
                "ORDER BY gap DESC LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                c->slowest_block_h   = sqlite3_column_int64(s, 0);
                c->slowest_block_gap = sqlite3_column_int64(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
}

/* Phase 1i: 40-bucket aggregates over 5 time ranges (24h/7d/30d/1y/all).
 * Single batch query per range. */
static void gather_chart_data(sqlite3 *db, int tip, double diff,
                              struct stats_chart_data *out)
{
    printf("Stats: computing chart data...\n"); fflush(stdout);
    struct { const char *label; const char *id; int blocks; } ranges[] = {
        {"24h", "24h", 576}, {"7d", "7d", 4032}, {"30d", "30d", 17280},
        {"1yr", "1y", 210240}, {"All", "all", tip},
    };

    for (int ri = 0; ri < 5; ri++) {
        int total = ranges[ri].blocks;
        if (total > tip) total = tip;
        int step = total / 40;
        if (step < 1) step = 1;
        int base = tip - total;
        if (base < 0) base = 0;

        /* Batch: get all 40 data points in one query using
         * (height - base) / step as bucket index */
        for (int i = 0; i < 40; i++) {
            out->diff_data[ri][i] = diff;
            out->hr_data[ri][i] = diff * 8192.0 / 150.0;
            out->sprout_c[ri][i] = 0;
            out->sapling_c[ri][i] = 0;
            out->txcount[ri][i] = 0;
        }

        /* Single query: aggregate by bucket */
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT (height - %d) / %d AS bucket, "
            "MAX(bits), "
            "COALESCE(SUM(sprout_value),0), "
            "COALESCE(SUM(sapling_value),0), "
            "COALESCE(SUM(num_tx),0) "
            "FROM blocks WHERE height > %d AND height <= %d "
            "GROUP BY bucket ORDER BY bucket",
            base, step, base, tip);
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int bucket = sqlite3_column_int(s, 0);
                if (bucket < 0 || bucket >= 40) continue;
                uint32_t bits = (uint32_t)sqlite3_column_int(s, 1);
                if (bits > 0) {
                    out->diff_data[ri][bucket] = explorer_difficulty_from_bits(bits);
                    out->hr_data[ri][bucket] = out->diff_data[ri][bucket] * 8192.0 / 150.0;
                }
                /* Sprout: positive=shielding, show as positive on chart (pool inflow)
                 * Sapling: negative=shielding, negate to show inflow as positive */
                out->sprout_c[ri][bucket]  = (double)sqlite3_column_int64(s, 2) / 1e8;
                out->sapling_c[ri][bucket] = -(double)sqlite3_column_int64(s, 3) / 1e8;
                out->txcount[ri][bucket]   = (double)sqlite3_column_int64(s, 4);
            }
            sqlite3_finalize(s);
        }
        for (int i = 0; i < 40; i++) {
            int h = base + (i + 1) * step;
            if (h > tip) h = tip;
            snprintf(out->labels[ri][i], sizeof(out->labels[ri][i]), "%d", h);
        }
    }
}

/* ── Section emit helpers ──────────────────────────────────────────
 *
 * Each helper appends one logical section of the stats HTML page.
 * The stats_ctx struct (defined at top of file) bundles every value
 * gathered during phase 1 so the render helpers don't need a fat
 * parameter list.
 */

static size_t emit_stats_header(uint8_t *r, size_t max, size_t off,
                                const struct stats_ctx *c)
{
    APPEND(off, r, max, EXPLORER_HEADER("ZClassic Deep Stats"));
    off += explorer_emit_nav((char *)r + off, max - off, "stats");
    APPEND(off, r, max,
        "<h1>ZClassic Blockchain Deep Statistics</h1>"
        "<p style='color:#888;margin:-8px 0 16px;font-size:15px'>"
        "Every statistic from %d days of blockchain history "
        "(Genesis: Nov 6 2016). Computed in %llds from indexed SQLite.</p>",
        (int)c->chain_age_days, (long long)c->t_query_ms);
    return off;
}

static size_t emit_section_1_network(uint8_t *r, size_t max, size_t off,
                                     const struct stats_ctx *c)
{
    APPEND(off, r, max,
        "<h2>Network Overview</h2>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%d</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.4f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Est. Hashrate</div></div>"
        "</div>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s ZCL</div><div class='lbl'>Circulating Supply</div></div>"
        "<div class='stat'><div class='num'>%.2f%%</div><div class='lbl'>%% of Max Supply</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Total UTXOs</div></div>"
        "</div>",
        c->tip, c->diff, c->hr_str,
        c->supply_str, c->pct_mined, c->utxo_count_val);
    return off;
}

static size_t emit_section_2_chain_history(uint8_t *r, size_t max, size_t off,
                                           const struct stats_ctx *c)
{
    char genesis_ts[32], tip_ts[32], subsidy_str[32];
    explorer_format_time(genesis_ts, sizeof(genesis_ts), (uint32_t)c->genesis_time);
    explorer_format_time(tip_ts, sizeof(tip_ts), (uint32_t)c->tip_time);
    explorer_format_zcl(subsidy_str, sizeof(subsidy_str), c->current_subsidy);
    APPEND(off, r, max,
        "<h2>Chain History</h2><div class='card'><div class='grid'>"
        "<div class='label'>Genesis Block</div><div class='val'>"
        "<a href='/explorer/block/0'>Block 0</a> &mdash; %s</div>"
        "<div class='label'>Latest Block</div><div class='val'>"
        "<a href='/explorer/block/%d'>Block %d</a> &mdash; %s</div>"
        "<div class='label'>Chain Age</div><div class='val'>%d days (%.1f years)</div>"
        "<div class='label'>Total Blocks</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Total Transactions</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Coinbase / Non-Coinbase</div><div class='val'>%" PRId64 " / %" PRId64 "</div>"
        "<div class='label'>Avg Txs/Block</div><div class='val'>%.2f</div>"
        "<div class='label'>Halvings Occurred</div><div class='val'>%d</div>"
        "<div class='label'>Current Block Reward</div><div class='val'>%s ZCL</div>"
        "<div class='label'>Next Halving</div><div class='val'>"
        "<a href='/explorer/block/%d'>Block %d</a> (%d blocks away)</div>"
        "</div></div>",
        genesis_ts, c->tip, c->tip, tip_ts,
        (int)c->chain_age_days, (double)c->chain_age_days / 365.25,
        c->total_blocks, c->total_txs, c->coinbase_txs, c->total_txs - c->coinbase_txs,
        c->avg_tx_per_block, c->halvings, subsidy_str,
        c->next_halving, c->next_halving, c->blocks_until_halving);
    return off;
}

static size_t emit_section_3_block_records(uint8_t *r, size_t max, size_t off,
                                           const struct stats_ctx *c)
{
    APPEND(off, r, max,
        "<h2>Block Records</h2><div class='card'><div class='grid'>"
        "<div class='label'>Most Transactions in Block</div>"
        "<div class='val'><a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a>"
        " &mdash; %" PRId64 " txs</div>"
        "<div class='label'>Empty Blocks (coinbase only)</div>"
        "<div class='val'>%" PRId64 " (%.1f%%)</div>"
        "<div class='label'>Lowest Difficulty</div>"
        "<div class='val'>%.6f at <a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a></div>"
        "<div class='label'>Highest Difficulty</div>"
        "<div class='val'>%.6f at <a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a></div>"
        "</div></div>",
        c->max_tx_block, c->max_tx_block, c->max_tx_count,
        c->empty_blocks, c->total_blocks > 0 ? (double)c->empty_blocks / c->total_blocks * 100.0 : 0,
        c->min_diff, c->min_bits_height, c->min_bits_height,
        c->max_diff, c->max_bits_height, c->max_bits_height);
    return off;
}

static size_t emit_section_6_utxo_distribution(uint8_t *r, size_t max, size_t off,
                                               const struct stats_ctx *c, sqlite3 *db)
{
    char max_val[32], avg_val[32], cb_val[32];
    explorer_format_zcl(max_val, sizeof(max_val), c->utxo_max_value);
    explorer_format_zcl(avg_val, sizeof(avg_val), (int64_t)c->utxo_avg);
    explorer_format_zcl(cb_val, sizeof(cb_val), c->utxo_coinbase_value);

    APPEND(off, r, max,
        "<h2>UTXO Set Distribution</h2>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total UTXOs</div></div>"
        "<div class='stat'><div class='num'>%s</div>"
        "<div class='lbl'>Largest UTXO</div></div>"
        "<div class='stat'><div class='num'>%s</div>"
        "<div class='lbl'>Average UTXO</div></div>"
        "</div>",
        c->utxo_count_val, max_val, avg_val);

    APPEND(off, r, max,
        "<div class='card'><h3 style='color:#4db8ff;margin:0 0 12px'>Size Breakdown</h3>"
        "<table><tr><th>Range</th><th>Count</th><th>%% of UTXOs</th></tr>"
        "<tr><td>Dust (&lt; 0.001 ZCL)</td><td>%" PRId64 "</td><td>%.1f%%</td></tr>"
        "<tr><td>Small (0.001 - 1 ZCL)</td><td>%" PRId64 "</td><td>%.1f%%</td></tr>"
        "<tr><td>Medium (1 - 10 ZCL)</td><td>%" PRId64 "</td><td>%.1f%%</td></tr>"
        "<tr><td>Large (10 - 100 ZCL)</td><td>%" PRId64 "</td><td>%.1f%%</td></tr>"
        "<tr><td>Whale (100+ ZCL)</td><td>%" PRId64 "</td><td>%.1f%%</td></tr>"
        "</table></div>",
        c->utxo_dust,   c->utxo_count_val > 0 ? (double)c->utxo_dust / c->utxo_count_val * 100 : 0,
        c->utxo_small,  c->utxo_count_val > 0 ? (double)c->utxo_small / c->utxo_count_val * 100 : 0,
        c->utxo_medium, c->utxo_count_val > 0 ? (double)c->utxo_medium / c->utxo_count_val * 100 : 0,
        c->utxo_large,  c->utxo_count_val > 0 ? (double)c->utxo_large / c->utxo_count_val * 100 : 0,
        c->utxo_whale,  c->utxo_count_val > 0 ? (double)c->utxo_whale / c->utxo_count_val * 100 : 0);

    /* Oldest UTXOs */
    APPEND(off, r, max,
        "<h3>Oldest Unspent UTXOs</h3>"
        "<table><tr><th>Block</th><th>Time</th><th>Value</th><th>TXID</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT u.height, b.time, u.value, hex(u.txid) "
                "FROM utxos u LEFT JOIN blocks b ON u.height = b.height "
                "ORDER BY u.height ASC LIMIT 20",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t h = sqlite3_column_int64(s, 0);
                int64_t t = sqlite3_column_int64(s, 1);
                if (t == 0 && h == 0) t = 1478403829; /* genesis */
                int64_t v = sqlite3_column_int64(s, 2);
                const char *txhex = (const char *)sqlite3_column_text(s, 3);
                char ts[32], vs[32], txid[65] = "", short_tx[18] = "";
                explorer_format_time(ts, sizeof(ts), (uint32_t)t);
                explorer_format_zcl(vs, sizeof(vs), v);
                if (txhex && strlen(txhex) == 64) {
                    for (int i = 0; i < 32; i++) {
                        txid[i*2]   = txhex[62-i*2];
                        txid[i*2+1] = txhex[63-i*2];
                    }
                    txid[64] = '\0';
                    snprintf(short_tx, sizeof(short_tx), "%.8s...", txid);
                }
                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td>%s</td><td class='amount'>%s ZCL</td>"
                    "<td class='hash'><a href='/explorer/tx/%s'>%s</a></td></tr>",
                    h, h, ts, vs, txid, short_tx);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* Top 20 largest UTXOs */
    APPEND(off, r, max,
        "<h3>Top 20 Largest Unspent Outputs</h3>"
        "<table><tr><th>Value</th><th>Block</th><th>TXID</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT value, height, hex(txid) FROM utxos "
                "ORDER BY value DESC LIMIT 20",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t v = sqlite3_column_int64(s, 0);
                int64_t h = sqlite3_column_int64(s, 1);
                const char *txhex = (const char *)sqlite3_column_text(s, 2);
                char vs[32], txid[65] = "", short_tx[18] = "";
                explorer_format_zcl(vs, sizeof(vs), v);
                if (txhex && strlen(txhex) == 64) {
                    for (int i = 0; i < 32; i++) {
                        txid[i*2]   = txhex[62-i*2];
                        txid[i*2+1] = txhex[63-i*2];
                    }
                    txid[64] = '\0';
                    snprintf(short_tx, sizeof(short_tx), "%.8s...", txid);
                }
                APPEND(off, r, max,
                    "<tr><td class='amount'>%s ZCL</td>"
                    "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
                    "<td class='hash'><a href='/explorer/tx/%s'>%s</a></td></tr>",
                    vs, h, h, txid, short_tx);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    APPEND(off, r, max,
        "<div class='card'><div class='grid'>"
        "<div class='label'>Unspent Coinbase UTXOs</div><div class='val'>%" PRId64 "</div>"
        "<div class='label'>Unspent Coinbase Value</div><div class='val amount'>%s ZCL</div>"
        "<div class='label'>Oldest UTXO From</div>"
        "<div class='val'><a href='/explorer/block/%" PRId64 "'>Block %" PRId64 "</a></div>"
        "</div></div>",
        c->utxo_coinbase_count, cb_val, c->utxo_min_height, c->utxo_min_height);
    return off;
}

static size_t emit_section_7_address_distribution(uint8_t *r, size_t max, size_t off,
                                                  const struct stats_ctx *c, sqlite3 *db)
{
    char top10_str[32], top100_str[32];
    explorer_format_zcl(top10_str, sizeof(top10_str), c->top10_balance);
    explorer_format_zcl(top100_str, sizeof(top100_str), c->top100_balance);

    APPEND(off, r, max,
        "<h2>Address Distribution</h2>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Addresses with UTXOs</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Non-Zero Balances</div></div>"
        "</div>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Top 10 Hold</div>"
        "<div class='val amount'>%s ZCL (%.1f%%)</div>"
        "<div class='label'>Top 100 Hold</div>"
        "<div class='val amount'>%s ZCL (%.1f%%)</div>"
        "</div></div>",
        c->total_addresses, c->addr_nonzero,
        top10_str, c->total_supply > 0 ? (double)c->top10_balance / c->total_supply * 100 : 0,
        top100_str, c->total_supply > 0 ? (double)c->top100_balance / c->total_supply * 100 : 0);

    /* Top 20 richest */
    APPEND(off, r, max,
        "<h3>Top 20 Richest Addresses</h3>"
        "<table><tr><th>#</th><th>Address</th><th>Balance</th>"
        "<th>UTXOs</th><th>%% of Supply</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT address_hash, balance, utxo_count, script_type "
                "FROM addresses ORDER BY balance DESC LIMIT 20",
                -1, &s, NULL) == SQLITE_OK && s) {
            int rank = 0;
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                rank++;
                const void *ah = sqlite3_column_blob(s, 0);
                int ah_len = sqlite3_column_bytes(s, 0);
                int64_t bal = sqlite3_column_int64(s, 1);
                int64_t uc = sqlite3_column_int64(s, 2);
                int st = sqlite3_column_int(s, 3);
                char bs[32], addr[64] = "unknown";
                explorer_format_zcl(bs, sizeof(bs), bal);
                if (ah && ah_len == 20) {
                    struct tx_destination dest;
                    memset(&dest, 0, sizeof(dest));
                    if (st == 2) {
                        dest.type = DEST_SCRIPT_ID;
                        memcpy(dest.id.script.hash.data, ah, 20);
                    } else {
                        dest.type = DEST_KEY_ID;
                        memcpy(dest.id.key.id.data, ah, 20);
                    }
                    stats_addr_encode(addr, sizeof(addr), &dest);
                }
                APPEND(off, r, max,
                    "<tr><td>%d</td>"
                    "<td class='hash'><a href='/explorer/address/%s'>%s</a></td>"
                    "<td class='amount'>%s ZCL</td><td>%" PRId64 "</td>"
                    "<td>%.2f%%</td></tr>",
                    rank, addr, addr, bs, uc,
                    c->total_supply > 0 ? (double)bal / c->total_supply * 100 : 0);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_8_zslp_opret(uint8_t *r, size_t max, size_t off,
                                        const struct stats_ctx *c)
{
    APPEND(off, r, max,
        "<h2>ZSLP Tokens &amp; OP_RETURN Data</h2>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>OP_RETURN Outputs</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>ZSLP OP_RETURNs</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Token Types</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>ZSLP Transfers</div></div>"
        "</div>",
        c->opret_count, c->slp_opret_count, c->zslp_token_count, c->zslp_transfer_count);
    if (c->zslp_token_count > 0)
        APPEND(off, r, max,
            "<p style='text-align:center'>"
            "<a href='/explorer/tokens' style='font-size:18px;font-weight:700'>"
            "View All ZSLP Tokens &rarr;</a></p>");
    return off;
}

static size_t emit_section_8b_tx_io(uint8_t *r, size_t max, size_t off,
                                    const struct stats_ctx *c)
{
    char max_out_str[32], total_moved_str[32];
    explorer_format_zcl(max_out_str, sizeof(max_out_str), c->max_output_value);
    explorer_format_zcl(total_moved_str, sizeof(total_moved_str), c->total_value_moved);
    double io_ratio = c->total_inputs > 0 ? (double)c->total_outputs / c->total_inputs : 0;

    APPEND(off, r, max,
        "<h2>Transaction I/O Deep Stats</h2>"
        "<p style='color:#888;margin:-4px 0 12px;font-size:14px'>"
        "Per-output and per-input data from the full chain index.</p>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total Outputs Ever</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total Inputs Ever</div></div>"
        "<div class='stat'><div class='num'>%.2f</div>"
        "<div class='lbl'>Output/Input Ratio</div></div>"
        "</div>",
        c->total_outputs, c->total_inputs, io_ratio);

    APPEND(off, r, max,
        "<div class='card'><div class='grid'>"
        "<div class='label'>P2PKH Outputs</div>"
        "<div class='val'>%" PRId64 " (%.1f%%)</div>"
        "<div class='label'>P2SH Outputs</div>"
        "<div class='val'>%" PRId64 " (%.1f%%)</div>"
        "<div class='label'>Other Output Types</div>"
        "<div class='val'>%" PRId64 "</div>"
        "<div class='label'>Largest Output Ever</div>"
        "<div class='val amount'>%s ZCL</div>"
        "<div class='label'>Total Value Ever Moved</div>"
        "<div class='val amount'>%s ZCL</div>"
        "</div></div>",
        c->p2pkh_outputs, c->total_outputs > 0 ? (double)c->p2pkh_outputs / c->total_outputs * 100 : 0,
        c->p2sh_outputs, c->total_outputs > 0 ? (double)c->p2sh_outputs / c->total_outputs * 100 : 0,
        c->total_outputs - c->p2pkh_outputs - c->p2sh_outputs,
        max_out_str, total_moved_str);
    return off;
}

static size_t emit_section_8c_shielded_detail(uint8_t *r, size_t max, size_t off,
                                              const struct stats_ctx *c)
{
    char vpub_old_str[32], vpub_new_str[32];
    explorer_format_zcl(vpub_old_str, sizeof(vpub_old_str), c->total_vpub_old);
    explorer_format_zcl(vpub_new_str, sizeof(vpub_new_str), c->total_vpub_new);
    double sap_ratio = c->total_sap_outputs > 0
        ? (double)c->total_sap_spends / c->total_sap_outputs : 0;

    APPEND(off, r, max,
        "<h2>Shielded Operations Detail</h2>"
        "<p style='color:#888;margin:-4px 0 12px;font-size:14px'>"
        "Per-transaction shielded data from joinsplits, sapling_spends, "
        "sapling_outputs, and sprout_nullifiers tables.</p>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total JoinSplits</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total Sapling Spends</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total Sapling Outputs</div></div>"
        "</div>",
        c->total_joinsplits, c->total_sap_spends, c->total_sap_outputs);

    APPEND(off, r, max,
        "<div class='card'><div class='grid'>"
        "<div class='label'>Sprout Nullifiers</div>"
        "<div class='val'>%" PRId64 "</div>"
        "<div class='label'>Total vpub_old (t&rarr;z via Sprout)</div>"
        "<div class='val amount'>%s ZCL</div>"
        "<div class='label'>Total vpub_new (z&rarr;t via Sprout)</div>"
        "<div class='val amount'>%s ZCL</div>"
        "<div class='label'>Unique Sapling Anchors</div>"
        "<div class='val'>%" PRId64 "</div>"
        "<div class='label'>Sapling Spend/Output Ratio</div>"
        "<div class='val'>%.4f</div>"
        "</div></div>",
        c->total_sprout_nulls,
        vpub_old_str, vpub_new_str,
        c->unique_sap_anchors, sap_ratio);
    return off;
}

static size_t emit_section_8d_integrity(uint8_t *r, size_t max, size_t off,
                                        const struct stats_ctx *c)
{
    char hash_short[20] = "N/A";
    if (c->integrity_latest_hash[0] != 'N') {
        snprintf(hash_short, sizeof(hash_short), "%.16s...", c->integrity_latest_hash);
    }

    APPEND(off, r, max,
        "<h2>SHA3-256 Integrity Chain</h2>"
        "<p style='color:#888;margin:-4px 0 12px;font-size:14px'>"
        "Cryptographic chain of block data hashes for tamper detection.</p>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div>"
        "<div class='lbl'>Total Checkpoints</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 " &ndash; %" PRId64 "</div>"
        "<div class='lbl'>Chain Coverage (Height)</div></div>"
        "</div>",
        c->integrity_count, c->integrity_min_h, c->integrity_max_h);

    APPEND(off, r, max,
        "<div class='card'><div class='grid'>"
        "<div class='label'>Latest SHA3 Hash</div>"
        "<div class='val' style='font-family:monospace;font-size:13px'>%s</div>"
        "</div>"
        "<p style='color:#888;margin:12px 0 0;font-size:13px;line-height:1.5'>"
        "Every block's data is chained via SHA3-256 hash. The integrity chain covers "
        "H(prev_hash || height || block_hash || sprout_value || sapling_value || num_tx "
        "|| num_joinsplits || num_sapling_spends || num_sapling_outputs). "
        "Verify any block by recomputing from genesis.</p></div>",
        c->integrity_latest_hash[0] != 'N' ? c->integrity_latest_hash : "N/A");
    return off;
}

static size_t emit_section_8e_firsts(uint8_t *r, size_t max, size_t off,
                                     const struct stats_ctx *c)
{
    APPEND(off, r, max,
        "<h2>Chain Firsts &amp; Records</h2>"
        "<p style='color:#888;margin:-4px 0 12px;font-size:14px'>"
        "Milestones and extremes from the full chain index.</p>"
        "<table><tr><th>Milestone</th><th>Block</th><th>Detail</th></tr>");

    if (c->first_noncoinbase > 0)
        APPEND(off, r, max,
            "<tr><td>First Non-Coinbase Transaction</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>First user-to-user transfer</td></tr>",
            c->first_noncoinbase, c->first_noncoinbase);

    if (c->first_joinsplit_h > 0)
        APPEND(off, r, max,
            "<tr><td>First JoinSplit (Sprout Shielding)</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>First shielded transaction</td></tr>",
            c->first_joinsplit_h, c->first_joinsplit_h);

    if (c->first_sapling_h > 0)
        APPEND(off, r, max,
            "<tr><td>First Sapling Spend</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>First Groth16 shielded spend</td></tr>",
            c->first_sapling_h, c->first_sapling_h);

    if (c->first_opreturn_h > 0)
        APPEND(off, r, max,
            "<tr><td>First OP_RETURN</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>First data-carrying output</td></tr>",
            c->first_opreturn_h, c->first_opreturn_h);

    if (c->first_zslp_h > 0)
        APPEND(off, r, max,
            "<tr><td>First ZSLP Token</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>First token genesis</td></tr>",
            c->first_zslp_h, c->first_zslp_h);

    if (c->most_js_count > 0)
        APPEND(off, r, max,
            "<tr><td>Most JoinSplits in Block</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%" PRId64 " JoinSplit operations</td></tr>",
            c->most_js_block, c->most_js_block, c->most_js_count);

    if (c->most_sap_out_count > 0)
        APPEND(off, r, max,
            "<tr><td>Most Sapling Outputs in Block</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%" PRId64 " Sapling notes created</td></tr>",
            c->most_sap_out_block, c->most_sap_out_block, c->most_sap_out_count);

    if (c->fastest_block_gap > 0)
        APPEND(off, r, max,
            "<tr><td>Fastest Block Time</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%" PRId64 " seconds</td></tr>",
            c->fastest_block_h, c->fastest_block_h, c->fastest_block_gap);

    if (c->slowest_block_gap > 0)
        APPEND(off, r, max,
            "<tr><td>Slowest Block Time</td>"
            "<td><a href='/explorer/block/%" PRId64 "'>%" PRId64 "</a></td>"
            "<td>%" PRId64 " seconds (%.1f hours)</td></tr>",
            c->slowest_block_h, c->slowest_block_h, c->slowest_block_gap,
            (double)c->slowest_block_gap / 3600.0);

    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_9_tx_volume_by_year(uint8_t *r, size_t max, size_t off,
                                               sqlite3 *db)
{
    APPEND(off, r, max,
        "<h2>Transaction Volume by Year</h2>"
        "<table><tr><th>Year</th><th>Blocks</th><th>Total Txs</th>"
        "<th>Avg Tx/Block</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT CAST(strftime('%Y', time, 'unixepoch') AS INTEGER) AS yr, "
                "count(*), COALESCE(SUM(num_tx),0) "
                "FROM blocks WHERE time > 0 GROUP BY yr ORDER BY yr",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int year = sqlite3_column_int(s, 0);
                int64_t blks = sqlite3_column_int64(s, 1);
                int64_t txs = sqlite3_column_int64(s, 2);
                if (blks > 0)
                    APPEND(off, r, max,
                        "<tr><td>%d</td><td>%" PRId64 "</td><td>%" PRId64 "</td>"
                        "<td>%.2f</td></tr>",
                        year, blks, txs, (double)txs / blks);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

static size_t emit_section_10_utxo_age(uint8_t *r, size_t max, size_t off,
                                       const struct stats_ctx *c, sqlite3 *db)
{
    APPEND(off, r, max,
        "<h2>UTXO Age Distribution</h2>"
        "<p style='color:#888;margin:-4px 0 12px;font-size:14px'>"
        "Grouped by creation block range.</p>"
        "<table><tr><th>Block Range</th><th>Age</th><th>UTXOs</th>"
        "<th>Value (ZCL)</th><th>%% of Supply</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT (height / 100000) * 100000 AS band, "
                "count(*), COALESCE(SUM(value),0) "
                "FROM utxos GROUP BY band ORDER BY band",
                -1, &s, NULL) == SQLITE_OK && s) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                int64_t band = sqlite3_column_int64(s, 0);
                int64_t cnt = sqlite3_column_int64(s, 1);
                int64_t val = sqlite3_column_int64(s, 2);
                if (cnt == 0) continue;
                char vs[32];
                explorer_format_zcl(vs, sizeof(vs), val);
                int64_t band_end = band + 100000;
                int mid = (int)((band + (band_end < c->tip ? band_end : c->tip)) / 2);
                int days = (int)(hodl_wave_age_seconds(mid, c->tip) / 86400);
                char age[32];
                if (days > 365) snprintf(age, sizeof(age), "~%.1fy", (double)days/365.25);
                else if (days > 30) snprintf(age, sizeof(age), "~%dmo", days/30);
                else snprintf(age, sizeof(age), "~%dd", days);

                APPEND(off, r, max,
                    "<tr><td>%" PRId64 "K-%" PRId64 "K</td><td>%s</td>"
                    "<td>%" PRId64 "</td><td class='amount'>%s</td>"
                    "<td>%.2f%%</td></tr>",
                    band/1000, band_end/1000, age, cnt, vs,
                    c->total_supply > 0 ? (double)val / c->total_supply * 100 : 0);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");
    return off;
}

/* ── Main stats builder ──────────────────────────────────── */

size_t explorer_stats_build(uint8_t *r, size_t buf_max, const char *datadir)
{
    if (!r || buf_max == 0 || !datadir)
        return 0;

    int64_t t_start_ms = (int64_t)platform_time_wall_time_t();
    size_t max = buf_max;
    size_t off = 0;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    int open_rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (open_rc != SQLITE_OK) {
        printf("Stats: failed to open %s: %s (rc=%d)\n",
               db_path, db ? sqlite3_errmsg(db) : "null", open_rc);
        fflush(stdout);
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 30000);

    /* ════════════════════════════════════════════════════════
     *  PHASE 1: Gather all data with minimal queries
     * ════════════════════════════════════════════════════════ */

    printf("Stats: phase 1 — gathering data...\n"); fflush(stdout);

    /* ── 1a: Tip block ── */
    int tip = 0;
    double diff = 0;
    int64_t tip_time = 0;
    {
        sqlite3_stmt *s = NULL;
        int prep_rc = sqlite3_prepare_v2(db,
                "SELECT height, bits, time FROM blocks ORDER BY height DESC LIMIT 1",
                -1, &s, NULL);
        if (prep_rc == SQLITE_OK && s) {
            int step_rc = AR_STEP_ROW_READONLY(s);
            if (step_rc == SQLITE_ROW) {
                tip = sqlite3_column_int(s, 0);
                uint32_t bits = (uint32_t)sqlite3_column_int(s, 1);
                tip_time = sqlite3_column_int64(s, 2);
                if (bits > 0)
                    diff = explorer_difficulty_from_bits(bits);
            } else {
                printf("Stats: tip query step failed: rc=%d %s\n",
                       step_rc, sqlite3_errmsg(db)); fflush(stdout);
            }
            sqlite3_finalize(s);
        } else {
            printf("Stats: tip query prepare failed: rc=%d %s\n",
                   prep_rc, sqlite3_errmsg(db)); fflush(stdout);
        }
    }
    if (tip <= 0) {
        printf("Stats: no blocks (tip=%d, db=%s)\n", tip, db_path);
        fflush(stdout);
        sqlite3_close(db);
        return 0;
    }
    if (!explorer_block_history_usable_for_height(db, tip)) {
        size_t len = explorer_stats_build_verified_summary(r, max, db);
        sqlite3_close(db);
        return len;
    }

    /* ── 1b: Single-pass block aggregates ── */
    int64_t total_blocks = 0, total_block_txs = 0, empty_blocks = 0;
    int64_t max_tx_count = 0, max_tx_block = 0;
    /* ZClassic genesis: Nov 6, 2016 00:03:49 UTC */
    int64_t genesis_time = 1478403829;
    uint32_t min_bits = 0, max_bits = 0;
    int64_t min_bits_height = 0, max_bits_height = 0;
    {
        sqlite3_stmt *s = NULL;
        /* Consolidated: count, sum(num_tx), count(empty), max(num_tx) */
        if (sqlite3_prepare_v2(db,
                "SELECT count(*), COALESCE(SUM(num_tx),0), "
                "SUM(CASE WHEN num_tx<=1 THEN 1 ELSE 0 END), "
                "MAX(num_tx) "
                "FROM blocks",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                total_blocks    = sqlite3_column_int64(s, 0);
                total_block_txs = sqlite3_column_int64(s, 1);
                empty_blocks    = sqlite3_column_int64(s, 2);
                max_tx_count    = sqlite3_column_int64(s, 3);
            }
            sqlite3_finalize(s);
        }
        /* Block with most txs */
        max_tx_block = stats_q_i64(db,
            "SELECT height FROM blocks ORDER BY num_tx DESC LIMIT 1");
        /* Difficulty extremes (bits DESC = lowest diff, bits ASC = highest diff) */
        s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT height, bits FROM blocks WHERE bits > 0 "
                "ORDER BY bits DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                min_bits_height = sqlite3_column_int64(s, 0);
                min_bits = (uint32_t)sqlite3_column_int(s, 1);
            }
            sqlite3_finalize(s);
        }
        s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT height, bits FROM blocks WHERE bits > 0 AND height > 0 "
                "ORDER BY bits ASC LIMIT 1", -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                max_bits_height = sqlite3_column_int64(s, 0);
                max_bits = (uint32_t)sqlite3_column_int(s, 1);
            }
            sqlite3_finalize(s);
        }
    }
    double min_diff = explorer_difficulty_from_bits(min_bits);
    double max_diff = explorer_difficulty_from_bits(max_bits);
    double avg_tx_per_block = total_blocks > 0
        ? (double)total_block_txs / total_blocks : 0;

    /* ── 1c: UTXO consolidated aggregate ── */
    int64_t total_supply = 0, utxo_count_val = 0;
    int64_t utxo_dust = 0, utxo_small = 0, utxo_medium = 0;
    int64_t utxo_large = 0, utxo_whale = 0;
    int64_t utxo_max_value = 0, utxo_min_height = 0;
    int64_t utxo_coinbase_count = 0, utxo_coinbase_value = 0;
    double utxo_avg = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT count(*), COALESCE(SUM(value),0), "
                "AVG(value), MAX(value), MIN(height), "
                "SUM(CASE WHEN value < 100000 THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN value >= 100000 AND value < 100000000 THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN value >= 100000000 AND value < 1000000000 THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN value >= 1000000000 AND value < 10000000000 THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN value >= 10000000000 THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN is_coinbase=1 THEN 1 ELSE 0 END), "
                "COALESCE(SUM(CASE WHEN is_coinbase=1 THEN value ELSE 0 END),0) "
                "FROM utxos",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                utxo_count_val      = sqlite3_column_int64(s, 0);
                total_supply        = sqlite3_column_int64(s, 1);
                utxo_avg            = sqlite3_column_double(s, 2);
                utxo_max_value      = sqlite3_column_int64(s, 3);
                utxo_min_height     = sqlite3_column_int64(s, 4);
                utxo_dust           = sqlite3_column_int64(s, 5);
                utxo_small          = sqlite3_column_int64(s, 6);
                utxo_medium         = sqlite3_column_int64(s, 7);
                utxo_large          = sqlite3_column_int64(s, 8);
                utxo_whale          = sqlite3_column_int64(s, 9);
                utxo_coinbase_count = sqlite3_column_int64(s, 10);
                utxo_coinbase_value = sqlite3_column_int64(s, 11);
            }
            sqlite3_finalize(s);
        }
    }

    /* ── 1d: Transaction count ── */
    int64_t total_txs = stats_q_i64(db, "SELECT count(*) FROM transactions");
    int64_t coinbase_txs = stats_q_i64(db,
        "SELECT count(*) FROM transactions WHERE is_coinbase=1");

    /* ── 1e: Sprout + Sapling — single-pass each ── */
    printf("Stats: querying shielded pools...\n"); fflush(stdout);
    struct shielded_stats sprout, sapling;
    query_shielded_stats(db, "sprout_value", &sprout);
    query_shielded_stats(db, "sapling_value", &sapling);
    int64_t nullifier_count = stats_q_i64(db, "SELECT count(*) FROM sapling_nullifiers");

    /* ── 1f: Address stats ── */
    int64_t total_addresses = stats_q_i64(db, "SELECT count(*) FROM addresses");
    int64_t addr_nonzero = stats_q_i64(db,
        "SELECT count(*) FROM addresses WHERE balance > 0");
    int64_t top10_balance = stats_q_i64(db,
        "SELECT COALESCE(SUM(balance),0) FROM "
        "(SELECT balance FROM addresses ORDER BY balance DESC LIMIT 10)");
    int64_t top100_balance = stats_q_i64(db,
        "SELECT COALESCE(SUM(balance),0) FROM "
        "(SELECT balance FROM addresses ORDER BY balance DESC LIMIT 100)");

    /* ── 1g: ZSLP / OP_RETURN ── */
    int64_t opret_count = stats_q_i64(db, "SELECT count(*) FROM op_returns");
    int64_t slp_opret_count = stats_q_i64(db,
        "SELECT count(*) FROM op_returns WHERE is_slp=1");
    int64_t zslp_token_count = stats_q_i64(db, "SELECT count(*) FROM zslp_tokens");
    int64_t zslp_transfer_count = stats_q_i64(db, "SELECT count(*) FROM zslp_transfers");

    /* ── 1h + 1i ── filled into ctx / chart_data by helpers below.
     * Declare ctx + chart_data here so the gather helpers can write
     * directly into them; PHASE 2 builds on top of these structures. */
    struct stats_ctx ctx = {0};
    struct stats_chart_data chart_data;
    gather_deep_chain_data(db, &ctx);
    gather_chart_data(db, tip, diff, &chart_data);

    /* ── Derived values ── */
    double hashrate = diff * 8192.0 / 150.0;
    char hr_str[64];
    if (hashrate > 1e9) snprintf(hr_str, sizeof(hr_str), "%.2f GH/s", hashrate / 1e9);
    else if (hashrate > 1e6) snprintf(hr_str, sizeof(hr_str), "%.2f MH/s", hashrate / 1e6);
    else if (hashrate > 1e3) snprintf(hr_str, sizeof(hr_str), "%.2f KH/s", hashrate / 1e3);
    else snprintf(hr_str, sizeof(hr_str), "%.0f H/s", hashrate);

    char supply_str[32];
    explorer_format_zcl(supply_str, sizeof(supply_str), total_supply);

    int64_t chain_age_days = 0;
    if (genesis_time > 0 && tip_time > genesis_time)
        chain_age_days = (tip_time - genesis_time) / 86400;

    /* Buttercup-aware halving calculation */
    int halvings;
    int next_halving;
    int blocks_until_halving;
    int64_t current_subsidy;
    if (tip >= BUTTERCUP_ACTIVATION_HEIGHT) {
        int era = (int)(((int64_t)tip - 1 - BUTTERCUP_ACTIVATION_HEIGHT) / POST_BC_HALVING);
        halvings = era + 3;
        current_subsidy = (BASE_SUBSIDY_SAT / 2) >> halvings;
        next_halving = (int)(BUTTERCUP_ACTIVATION_HEIGHT + 1 +
                             ((int64_t)(era + 1)) * POST_BC_HALVING);
        blocks_until_halving = next_halving - tip;
    } else {
        halvings = tip / PRE_BC_HALVING;
        next_halving = (halvings + 1) * PRE_BC_HALVING;
        blocks_until_halving = next_halving - tip;
        current_subsidy = BASE_SUBSIDY_SAT >> halvings;
    }
    /* Correct max supply using Buttercup-aware computation */
    int64_t max_supply_sat = zcl_total_supply_zatoshi(100000000LL);
    double pct_mined = (max_supply_sat > 0)
        ? (double)total_supply / (double)max_supply_sat * 100.0 : 0.0;

    int64_t t_query_ms = (int64_t)platform_time_wall_time_t() - t_start_ms;
    printf("Stats: phase 1 complete in %llds, building HTML...\n",
        (long long)t_query_ms); fflush(stdout);

    /* ════════════════════════════════════════════════════════
     *  PHASE 2: Render HTML
     * ════════════════════════════════════════════════════════ */

    /* Populate the non-deep-chain ctx fields here. The deep-chain
     * group (tx I/O, joinsplits, sapling, integrity, firsts/records)
     * was filled in PHASE 1 by gather_deep_chain_data(). */
    ctx.tip = tip; ctx.diff = diff; ctx.tip_time = tip_time;
    ctx.genesis_time = genesis_time; ctx.chain_age_days = chain_age_days;
    ctx.total_blocks = total_blocks; ctx.total_block_txs = total_block_txs;
    ctx.empty_blocks = empty_blocks;
    ctx.max_tx_count = max_tx_count; ctx.max_tx_block = max_tx_block;
    ctx.min_bits = min_bits; ctx.max_bits = max_bits;
    ctx.min_bits_height = min_bits_height; ctx.max_bits_height = max_bits_height;
    ctx.min_diff = min_diff; ctx.max_diff = max_diff;
    ctx.avg_tx_per_block = avg_tx_per_block;
    ctx.total_supply = total_supply; ctx.utxo_count_val = utxo_count_val;
    ctx.utxo_dust = utxo_dust; ctx.utxo_small = utxo_small;
    ctx.utxo_medium = utxo_medium; ctx.utxo_large = utxo_large;
    ctx.utxo_whale = utxo_whale;
    ctx.utxo_max_value = utxo_max_value; ctx.utxo_min_height = utxo_min_height;
    ctx.utxo_coinbase_count = utxo_coinbase_count;
    ctx.utxo_coinbase_value = utxo_coinbase_value;
    ctx.utxo_avg = utxo_avg;
    ctx.total_txs = total_txs; ctx.coinbase_txs = coinbase_txs;
    ctx.sprout = sprout; ctx.sapling = sapling;
    ctx.nullifier_count = nullifier_count;
    ctx.total_addresses = total_addresses; ctx.addr_nonzero = addr_nonzero;
    ctx.top10_balance = top10_balance; ctx.top100_balance = top100_balance;
    ctx.opret_count = opret_count; ctx.slp_opret_count = slp_opret_count;
    ctx.zslp_token_count = zslp_token_count;
    ctx.zslp_transfer_count = zslp_transfer_count;
    ctx.halvings = halvings; ctx.next_halving = next_halving;
    ctx.blocks_until_halving = blocks_until_halving;
    ctx.current_subsidy = current_subsidy;
    ctx.pct_mined = pct_mined;
    ctx.t_query_ms = t_query_ms;
    snprintf(ctx.hr_str, sizeof(ctx.hr_str), "%s", hr_str);
    snprintf(ctx.supply_str, sizeof(ctx.supply_str), "%s", supply_str);

    off = emit_stats_header(r, max, off, &ctx);
    off = emit_section_1_network(r, max, off, &ctx);
    off = emit_section_2_chain_history(r, max, off, &ctx);
    off = emit_section_3_block_records(r, max, off, &ctx);

    /* Section 4: Sprout */
    render_shielded_section((char *)r, max, &off,
        "Sprout Pool (JoinSplit Privacy)",
        "Sprout was ZClassic's original shielded pool using JoinSplit proofs. "
        "vpub_old = value entering Sprout pool from transparent (shielding), "
        "vpub_new = value leaving Sprout pool to transparent (unshielding).",
        &sprout, -1, db, "sprout_value", 2016, true);

    /* Section 5: Sapling */
    render_shielded_section((char *)r, max, &off,
        "Sapling Pool (Modern Privacy)",
        "Sapling uses Groth16 proofs. Activated at block 382168. "
        "Positive value_balance = unshielding (z&rarr;t), "
        "negative = shielding (t&rarr;z).",
        &sapling, nullifier_count, db, "sapling_value", 2018, false);

    off = emit_section_6_utxo_distribution(r, max, off, &ctx, db);
    off = emit_section_7_address_distribution(r, max, off, &ctx, db);
    off = emit_section_8_zslp_opret(r, max, off, &ctx);
    off = emit_section_8b_tx_io(r, max, off, &ctx);
    off = emit_section_8c_shielded_detail(r, max, off, &ctx);
    off = emit_section_8d_integrity(r, max, off, &ctx);
    off = emit_section_8e_firsts(r, max, off, &ctx);
    off = emit_section_9_tx_volume_by_year(r, max, off, db);
    off = emit_section_10_utxo_age(r, max, off, &ctx, db);

    /* HODL link */
    APPEND(off, r, max,
        "<div class='card' style='text-align:center'>"
        "<a href='/explorer/hodl' style='font-size:20px;font-weight:700'>"
        "View HODL Wave &rarr;</a>"
        "<p style='color:#888;margin:4px 0 0;font-size:14px'>"
        "Current transparent UTXO age distribution</p></div>");

    /* Tab CSS */
    APPEND(off, r, max,
        "<style>"
        ".tabs input{display:none}"
        ".tabs .tab-bar{display:flex;gap:0;margin:12px 0 0}"
        ".tabs label{padding:10px 20px;background:#1a1a1a;color:#888;"
        "cursor:pointer;font-size:16px;font-weight:600;border:1px solid #222;"
        "border-bottom:none;border-radius:8px 8px 0 0;transition:all 0.2s}"
        ".tabs label:hover{color:#fff;background:#222}"
        ".tabs .panel{display:none;background:#111;border:1px solid #222;"
        "border-radius:0 8px 8px 8px;padding:16px}");
    {
        const char *ids[] = {"24h","7d","30d","1y","all"};
        stats_tab_css((char *)r, max, &off, "d", "#4db8ff", 5, ids);
        stats_tab_css((char *)r, max, &off, "h", "#33ff99", 5, ids);
        stats_tab_css((char *)r, max, &off, "sv", "#ff9933", 5, ids);
        stats_tab_css((char *)r, max, &off, "sp", "#aa66ff", 5, ids);
        stats_tab_css((char *)r, max, &off, "tx", "#ff6699", 5, ids);
    }
    APPEND(off, r, max, "</style>");

    /* Charts */
    const char *rids[] = {"24h","7d","30d","1y","all"};
    render_tabbed_chart((char *)r, max, &off,
        "Difficulty", "d", "#4db8ff", chart_data.diff_data, chart_data.labels, "Difficulty", rids);
    render_tabbed_chart((char *)r, max, &off,
        "Hashrate", "h", "#33ff99", chart_data.hr_data, chart_data.labels, "H/s", rids);
    render_tabbed_chart((char *)r, max, &off,
        "Sprout Value Flow", "sv", "#ff9933", chart_data.sprout_c, chart_data.labels, "ZCL", rids);
    render_tabbed_chart((char *)r, max, &off,
        "Sapling Value Flow", "sp", "#aa66ff", chart_data.sapling_c, chart_data.labels, "ZCL", rids);
    render_tabbed_chart((char *)r, max, &off,
        "Transaction Volume", "tx", "#ff6699", chart_data.txcount, chart_data.labels, "Transactions", rids);

    APPEND(off, r, max, EXPLORER_FOOTER);

    sqlite3_close(db);
    printf("Stats: built %zu bytes (tip=%d) in %llds total\n",
        off, tip, (long long)((int64_t)platform_time_wall_time_t() - t_start_ms));
    fflush(stdout);
    return off;
}
