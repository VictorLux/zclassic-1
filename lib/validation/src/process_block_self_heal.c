/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Self-heal logic for missing UTXOs and stuck-tip recovery. Split out
 * of process_block.c — pure code motion from the original file.
 *
 * Contents:
 *   - s_utxo_* failure-tracking state
 *   - g_self_heal_* atomic counters + snapshot API
 *   - process_block_self_heal_scan_{depth_limit,enabled}
 *   - process_block_inject_missing_utxo (callable from core scan path)
 *   - process_block_recover_missing_utxo_from_legacy_rpc
 *   - process_block_recover_missing_utxo_from_sqlite_tx_index
 *   - process_block_is_missing_utxo_failure
 *   - process_block_note_utxo_failure
 *   - hot-loop / needs-reimport flag writers
 *   - ZCL_TESTING hooks */

#include "platform/time_compat.h"
#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/connect_block.h"
/* AUTHORITATIVE recovery retry routes through the reducer (cursor move +
 * reducer_kick) instead of legacy disconnect_tip — same app-layer
 * controller reach process_block_revalidate.c/process_block_invalidate.c
 * already take. Inline marker keeps the lib_layering baseline flat. */
#include "validation/chainstate.h"
#include "services/chain_activation_controller.h"  // lib-layer-ok:self-heal-reducer-retry
#include "config/runtime.h"
#include "coins/utxo_commitment.h"
#include "core/serialize.h"
#include "core/core_io.h"
#include "event/event.h"
#include "models/tx_index.h"
#include "rpc/legacy_rpc_client.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"
#include "storage/block_index_db.h"
#include "storage/utxo_reimport_flag.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "process_block_internal.h"

bool chain_restore_block_is_consensus_backed_on_disk(
    const struct block_index *tip,
    const char *datadir);

/* ── self-heal state shared with core ─────────────────────────── */
int s_utxo_fail_count = 0;
int s_utxo_fail_height = -1;
int s_utxo_hot_loop_reported_height = -1;
int s_utxo_activation_paused_height = -1;

_Atomic uint64_t g_self_heal_tx_index_hits;
_Atomic uint64_t g_self_heal_scan_hits;
_Atomic uint64_t g_self_heal_scan_exhausted;
_Atomic uint64_t g_self_heal_scan_blocks_checked_total;

/* ── self-heal scan tunables ──────────────────────────────────── */
int process_block_self_heal_scan_depth_limit(void)
{
    const char *depth_env = getenv("ZCL_SELF_HEAL_SCAN_DEPTH");
    char *end = NULL;
    long depth_limit;

    if (!depth_env || depth_env[0] == '\0')
        return SELF_HEAL_SCAN_DEFAULT_DEPTH;

    depth_limit = strtol(depth_env, &end, 10);
    if (end == depth_env || *end != '\0' ||
        depth_limit <= 0 || depth_limit > INT_MAX)
        return SELF_HEAL_SCAN_DEFAULT_DEPTH;

    if (depth_limit < SELF_HEAL_SCAN_DEFAULT_DEPTH)
        return SELF_HEAL_SCAN_DEFAULT_DEPTH;

    return (int)depth_limit;
}

bool process_block_self_heal_scan_enabled(void)
{
    const char *scan_env = getenv("ZCL_SELF_HEAL_SCAN_ENABLE");
    if (!scan_env || scan_env[0] == '\0')
        return false;
    return strcmp(scan_env, "1") == 0 ||
           strcmp(scan_env, "true") == 0 ||
           strcmp(scan_env, "yes") == 0;
}

void process_block_self_heal_stats_snapshot(
    struct self_heal_scan_stats *out)
{
    if (!out) return;
    out->tx_index_hits =
        atomic_load_explicit(&g_self_heal_tx_index_hits,
                             memory_order_relaxed);
    out->scan_hits =
        atomic_load_explicit(&g_self_heal_scan_hits,
                             memory_order_relaxed);
    out->scan_exhausted =
        atomic_load_explicit(&g_self_heal_scan_exhausted,
                             memory_order_relaxed);
    out->scan_blocks_checked_total =
        atomic_load_explicit(&g_self_heal_scan_blocks_checked_total,
                             memory_order_relaxed);
}

/* ── UTXO injection helper ────────────────────────────────────── */
bool process_block_inject_missing_utxo(
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    const struct transaction *tx,
    int height,
    const char *source,
    int retry_no)
{
    if (!coins_tip || !txid || !tx || height < 0 || !source)
        return false;
    if (missing_vout >= tx->num_vout) {
        char hex[65];
        uint256_get_hex(txid, hex);
        fprintf(stderr, "[self-heal] %s found tx %s at h=%d but vout=%u "
                "is out of range (outputs=%zu)\n",
                source, hex, height, missing_vout, tx->num_vout);
        return false;
    }

    struct coins_cache_entry *entry =
        coins_view_cache_modify_new(coins_tip, txid);
    if (!entry)
        return false;

    coins_from_transaction(&entry->coins, tx, height);
    entry->flags = COINS_CACHE_DIRTY;

    char hex[65];
    uint256_get_hex(txid, hex);
    printf("[self-heal] Recovered UTXO %s:%u from %s h=%d — retry %d\n",
           hex, missing_vout, source, height, retry_no);
    fflush(stdout);
    return true;
}

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

bool process_block_recover_missing_utxo_from_sqlite_tx_index(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    const char *datadir,
    int retry_no)
{
    struct node_db *ndb = process_block_node_db_internal();
    if (!ms || !coins_tip || !txid || !datadir || !ndb)
        return false;

    struct db_tx_index dbtx;
    memset(&dbtx, 0, sizeof(dbtx));
    bool used_reversed = false;
    if (!db_tx_find_native_or_reversed(ndb, txid->data, &dbtx,
                                       &used_reversed))
        return false;

    struct uint256 block_hash;
    memcpy(block_hash.data, dbtx.block_hash, sizeof(block_hash.data));
    struct block_index *src_idx =
        block_map_find(&ms->map_block_index, &block_hash);
    if (!src_idx) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, "[self-heal] SQLite tx index hit for %s but "
                "source block is absent from block_map (height=%d)\n",
                txhex, dbtx.block_height);
        return false;
    }

    if (src_idx->nHeight != dbtx.block_height) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[self-heal] SQLite tx index height mismatch for %s: "
                "db=%d index=%d; using hash-verified block index\n",
                txhex, dbtx.block_height, src_idx->nHeight);
    }

    if (!chain_restore_block_is_consensus_backed_on_disk(src_idx, datadir)) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, "[self-heal] SQLite tx index hit for %s points to "
                "non-verified disk block h=%d file=%d pos=%u\n",
                txhex, src_idx->nHeight, src_idx->nFile, src_idx->nDataPos);
        return false;
    }

    struct block src_block;
    block_init(&src_block);
    if (!read_block_from_disk_index(&src_block, src_idx, datadir)) {
        block_free(&src_block);
        return false;
    }

    bool recovered = false;
    if (dbtx.tx_index >= 0 && (size_t)dbtx.tx_index < src_block.num_vtx &&
        uint256_eq(&src_block.vtx[dbtx.tx_index].hash, txid)) {
        recovered = process_block_inject_missing_utxo(
            coins_tip, txid, missing_vout, &src_block.vtx[dbtx.tx_index],
            src_idx->nHeight, "SQLite tx index", retry_no);
    } else {
        for (size_t ti = 0; ti < src_block.num_vtx; ti++) {
            if (!uint256_eq(&src_block.vtx[ti].hash, txid))
                continue;
            recovered = process_block_inject_missing_utxo(
                coins_tip, txid, missing_vout, &src_block.vtx[ti],
                src_idx->nHeight, "SQLite tx index scan", retry_no);
            break;
        }
    }

    if (recovered && used_reversed) {
        char txhex[65];
        uint256_get_hex(txid, txhex);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[self-heal] SQLite tx index recovered %s using %s lookup "
                "after local block/tx hash verification\n",
                txhex, "reversed");
    }

    block_free(&src_block);
    return recovered;
}

/* ── bounded backward chain-scan recovery ─────────────────────── */
bool process_block_recover_missing_utxo_from_chain_scan(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t vout,
    const char *datadir,
    int retry_no)
{
    char hex[65];
    uint256_get_hex(txid, hex);

    /* ── Scan fallback ( surgical coordinator
     *    commit 2026-04-22 05:11, pre-landed ahead of
     *    Agent-2's RED/factoring row).
     *
     * The tx index can be empty for this tx because
     * LDB fast-sync imports UTXOs but doesn't
     * populate block_tree_db's tx-offset entries.
     * Before surrendering the block as
     * BLOCK_FAILED_VALID, walk the active chain
     * backward a bounded number of blocks and
     * search each for the missing txid.  If found,
     * inject its outputs into the coins cache AND
     * backfill the tx_index entry so the next
     * spend of the same tx is O(log N).
     *
     * 2026-05-10 stalls: live imports have needed
     * UTXOs 150k-200k blocks behind tip after
     * partial chainstate recovery.  Default to a
     * deep bounded scan and keep
     * ZCL_SELF_HEAL_SCAN_DEPTH as an operator
     * override for deeper exceptional repairs.
     * Lower values are ignored because they make
     * the live recovery path fail open into a
     * restart loop. */
    int tip_h = active_chain_height(&ms->chain_active);
    int depth_limit = process_block_self_heal_scan_depth_limit();
    int scan_stop = (tip_h - depth_limit < 0) ? 0 : tip_h - depth_limit;

    bool recovered = false;
    bool scan_hit = false;
    int scan_blocks_checked = 0;
    int scan_hit_height = -1;
    for (int h = tip_h; h >= scan_stop && !scan_hit; h--) {
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA))
            continue;
        scan_blocks_checked++;
        struct block scan_b;
        block_init(&scan_b);
        if (!read_block_from_disk_index(&scan_b, bi, datadir)) {
            block_free(&scan_b);
            continue;
        }
        for (size_t ti = 0; ti < scan_b.num_vtx; ti++) {
            if (!uint256_eq(&scan_b.vtx[ti].hash, txid))
                continue;
            if (process_block_inject_missing_utxo(
                    coins_tip, txid, vout,
                    &scan_b.vtx[ti], h,
                    "verified chain scan", retry_no)) {
                scan_hit = true;
                scan_hit_height = h;
                /* Backfill tx_index — on the next
                 * spend of this tx we take the
                 * fast O(log N) path instead of
                 * re-scanning.  Not fatal if the
                 * write fails; the recovery still
                 * happened. */
                struct disk_tx_pos tx_new;
                disk_tx_pos_init(&tx_new);
                tx_new.block_pos.nFile = bi->nFile;
                tx_new.block_pos.nPos = bi->nDataPos;
                (void)block_tree_db_write_tx_index(
                    g_active_block_tree, txid, &tx_new, 1);
            }
            break;
        }
        block_free(&scan_b);
    }

    if (scan_hit) {
        atomic_fetch_add_explicit(
            &g_self_heal_scan_hits, 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &g_self_heal_scan_blocks_checked_total,
            (uint64_t)scan_blocks_checked,
            memory_order_relaxed);
        printf("[self-heal] RECOVERED UTXO %s via "
               "chain scan (hit_h=%d, depth=%d, "
               "blocks_checked=%d) — retry %d\n",
               hex, scan_hit_height,
               tip_h - scan_hit_height,
               scan_blocks_checked, retry_no);
        fflush(stdout);
        event_emitf(EV_SELF_HEAL_SCAN_HIT, 0,
            "tx=%s h=%d depth=%d",
            hex, scan_hit_height,
            tip_h - scan_hit_height);
        recovered = true;
    } else {
        atomic_fetch_add_explicit(
            &g_self_heal_scan_exhausted, 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &g_self_heal_scan_blocks_checked_total,
            (uint64_t)scan_blocks_checked,
            memory_order_relaxed);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[self-heal] scan exhausted "
            "(tx=%s, tip_h=%d, depth_limit=%d, "
            "blocks_checked=%d) — no match\n",
            hex, tip_h, depth_limit,
            scan_blocks_checked);
        event_emitf(EV_SELF_HEAL_SCAN_EXHAUSTED, 0,
            "tx=%s tip_h=%d depth=%d",
            hex, tip_h, depth_limit);
    }

    return recovered;
}

/* ── needs_reimport flag + hot-loop exit ──────────────────────── */
static void process_block_maybe_write_needs_reimport_flag(int height,
                                                          const char *datadir)
{
    if (s_utxo_fail_count < 3 || !datadir)
        return;

    /* Storage layout + on-disk format owned by the
     * utxo_reimport_flag primitive (lib/storage/). */
    (void)utxo_reimport_flag_set(datadir);
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
        "CRITICAL: %d UTXO failures at h=%d — "
        "wrote needs_reimport flag.\n",
        s_utxo_fail_count,
        height);
}

static void process_block_maybe_trigger_hot_loop_exit(int height,
                                                      const char *datadir)
{
    if (s_utxo_fail_count < 10 || !datadir)
        return;

    if (s_utxo_hot_loop_reported_height == height)
        return;

    char marker_path[512];
    snprintf(marker_path, sizeof(marker_path),
             "%s/last_reimport_attempted", datadir);
    struct stat mst;
    time_t now_s = platform_time_wall_time_t();
    bool reimport_recent =
        (stat(marker_path, &mst) == 0 &&
         now_s - mst.st_mtime < 600);

    if (reimport_recent) {
        event_emitf(EV_BOOT_ACTIVATE, 0,
            "FATAL_HOT_LOOP_STUCK h=%d fails=%d "
            "reimport_age_sec=%ld",
            height,
            s_utxo_fail_count,
            (long)(now_s - mst.st_mtime));
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "CRITICAL: %d UTXO failures at h=%d "
            "but reimport was attempted %lds ago "
            "and did NOT heal the UTXO set. NOT "
            "auto-restarting (would bootloop). "
            "Operator intervention required — "
            "inspect `zcl_events`, `node.log`, "
            "and consider rolling the tip back "
            "to before the missing-input height "
            "and resyncing from P2P.\n",
            s_utxo_fail_count,
            height,
            (long)(now_s - mst.st_mtime));
        fflush(stderr);
        s_utxo_activation_paused_height = height;
    } else {
        event_emitf(EV_BOOT_ACTIVATE, 0,
            "FATAL_HOT_LOOP h=%d fails=%d "
            "reimport=1",
            height,
            s_utxo_fail_count);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "CRITICAL: %d consecutive UTXO "
            "failures at h=%d — requesting "
            "clean shutdown so systemd restart "
            "picks up needs_reimport flag.\n",
            s_utxo_fail_count,
            height);
        fflush(stderr);
        g_shutdown_requested = 1;
    }

    s_utxo_hot_loop_reported_height = height;
}

int process_block_get_utxo_activation_paused_height(void)
{
    return s_utxo_activation_paused_height;
}

void process_block_clear_utxo_activation_pause_range(int scan_start,
                                                     int scan_end)
{
    if (scan_start <= 0 || scan_end < scan_start)
        return;
    if (s_utxo_activation_paused_height < scan_start ||
        s_utxo_activation_paused_height > scan_end)
        return;

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
        "[recovery] clearing UTXO activation pause at h=%d after "
        "successful repair scan [%d,%d]\n",
        s_utxo_activation_paused_height, scan_start, scan_end);
    s_utxo_activation_paused_height = -1;
    s_utxo_hot_loop_reported_height = -1;
    if (s_utxo_fail_height >= scan_start && s_utxo_fail_height <= scan_end) {
        s_utxo_fail_height = -1;
        s_utxo_fail_count = 0;
    }
}

bool process_block_is_missing_utxo_failure(
    const struct validation_state *state)
{
    return state && state->reject_reason[0] &&
           strcmp(state->reject_reason,
                  "bad-txns-inputs-missingorspent") == 0;
}

void process_block_note_utxo_failure(struct main_state *ms,
                                     struct coins_view_cache *coins_tip,
                                     int height,
                                     const char *datadir)
{
    /* coins_tip retained in the signature (public API / test injection); the
     * UTXO unwind now goes through the reducer's inverse-delta machinery via
     * reducer_kick, not the legacy coins-view disconnect path. */
    (void)coins_tip;
    if (height == s_utxo_fail_height)
        s_utxo_fail_count++;
    else {
        s_utxo_fail_height = height;
        s_utxo_fail_count = 1;
        s_utxo_hot_loop_reported_height = -1;
        s_utxo_activation_paused_height = -1;
    }

    int durable_utxo_max_h =
        app_runtime_node_db_utxo_max_height(process_block_node_db_internal());

    if (durable_utxo_max_h > height + 10) {
        if (s_utxo_fail_count == 1 || s_utxo_fail_count == 5) {
            event_emitf(EV_BOOT_ACTIVATE, 0,
                "HISTORIC_UTXO_REPLAY_REFUSED h=%d utxo_max=%d fails=%d",
                height, durable_utxo_max_h, s_utxo_fail_count);
            fprintf(stderr,
                "[recovery] refusing destructive reimport loop: missing input "
                "at h=%d but durable UTXOs reach h=%d; activation should use "
                "the snapshot/coins anchor instead of replaying history.\n",
                height, durable_utxo_max_h);
        }
        if (s_utxo_fail_count >= 5)
            s_utxo_activation_paused_height = height;
        return;
    }

    if (s_utxo_fail_count >= 5) {
        /* After 5 failures at the same height, try disconnecting the tip
         * to retry from the previous UTXO state.  This is best-effort:
         * if undo data is unavailable the reimport flag path below is the
         * durable recovery route. */
        struct block_index *tip = ms ? active_chain_tip(&ms->chain_active)
                                     : NULL;
        if (tip && tip->pprev) {
            /* The STAGE owns the coins.db / UTXO unwind. Drive the stage-side
             * unwind exactly as the live reorg path does — move the
             * active-chain cursor DOWN one (a pure cursor move, no legacy
             * coins write), then kick the reducer so its inverse-delta
             * machinery rewinds the stage cursors and re-walks. The stage
             * holds its own inverse-delta rows, not the legacy undo file. */
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[recovery] %d UTXO failures at h=%d — stage-unwinding tip "
                "h=%d to retry (reducer-authoritative)\n",
                s_utxo_fail_count, height, tip->nHeight);
            if (active_chain_move_window_tip(&ms->chain_active, tip->pprev)) {
                (void)reducer_kick(boot_activation_controller());
                s_utxo_fail_count = 0;
                s_utxo_fail_height = -1;
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[recovery] Stage-unwound tip — retrying from h=%d\n",
                    active_chain_height(&ms->chain_active));
            }
        } else {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[recovery] UTXO mismatch at h=%d: inputs missing. "
                "Chain tip and UTXO set are out of sync.\n"
                "[recovery] Restart with -reimport-utxos or delete "
                "chainstate/ to force fresh import.\n",
                height);
        }
    }

    process_block_maybe_write_needs_reimport_flag(height, datadir);
    process_block_maybe_trigger_hot_loop_exit(height, datadir);
}

#ifdef ZCL_TESTING
void process_block_test_set_utxo_fail_state(int height, int count)
{
    s_utxo_fail_height = height;
    s_utxo_fail_count = count;
    s_utxo_hot_loop_reported_height = -1;
    s_utxo_activation_paused_height = -1;
}

int process_block_test_get_utxo_fail_count(void)
{
    return s_utxo_fail_count;
}

int process_block_test_get_utxo_activation_paused_height(void)
{
    return s_utxo_activation_paused_height;
}

void process_block_test_set_utxo_activation_paused_height(int height)
{
    s_utxo_activation_paused_height = height;
}

void process_block_test_trigger_hot_loop_check(int height,
                                               const char *datadir)
{
    process_block_maybe_write_needs_reimport_flag(height, datadir);
    process_block_maybe_trigger_hot_loop_exit(height, datadir);
}

void process_block_test_note_utxo_failure(int height, const char *datadir)
{
    process_block_note_utxo_failure(NULL, NULL, height, datadir);
}
#endif
