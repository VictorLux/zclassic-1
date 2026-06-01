/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Self-heal coordination for missing UTXOs and stuck-tip recovery. Split out
 * of process_block.c and then narrowed into recovery-source files.
 *
 * Contents:
 *   - s_utxo_* failure-tracking state
 *   - g_self_heal_* atomic counters + snapshot API
 *   - process_block_self_heal_scan_{depth_limit,enabled}
 *   - process_block_inject_missing_utxo (callable from core scan path)
 *   - process_block_is_missing_utxo_failure
 *   - process_block_note_utxo_failure
 *   - hot-loop / needs-reimport flag writers
 *   - ZCL_TESTING hooks */

#include "platform/time_compat.h"
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
#include "event/event.h"
#include "storage/utxo_reimport_flag.h"

#include "process_block_internal.h"

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
