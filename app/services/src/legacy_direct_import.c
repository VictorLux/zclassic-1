/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_direct_import.c — see header.
 *
 * Pipeline:
 *   1. bilr_open(legacy/blocks/index) + bilr_load_height_map.
 *   2. bmr_open(legacy/blocks).
 *   3. SHA3 spot-check K=3 random windows from mmap'd payloads.
 *      Fail → continue with full validation and no elevated evidence.
 *   4. Set g_body_pull_active = 1 (per-block I/O deferrals kick in).
 *   5. For h in [from_h+1 .. legacy_tip]:
 *        a. payload = bmr_get_payload(map[h].nFile, map[h].nDataPos).
 *        b. block_deserialize from payload bytes (zero-copy stream).
 *        c. process_new_block(...) — accept + activate.
 *   6. Disarm g_body_pull_active. block_index sync flush is implicit
 *      on next sync write (the LevelDB memtable keeps the writes
 *      live; if the process crashes, recovery rewinds to coins.db).
 *   7. wallet_rescan if `wallet` is non-NULL.
 */

#include "platform/time_compat.h"
#include "services/legacy_direct_import.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "consensus/validation.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "services/legacy_bootstrap_importer.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"        /* g_body_pull_active */
#include "wallet/wallet.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LDI_SPOTCHECK_K 3

static int64_t ldi_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool legacy_direct_import_range_blocking(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct chain_params *params,
    struct wallet *wallet,
    const char *our_datadir,
    const char *legacy_datadir,
    int from_height,
    struct ldi_result *out)
{
    struct ldi_result r = (struct ldi_result){0};
    r.final_tip = -1;
    r.legacy_tip = -1;
    if (!ms || !coins_tip || !params || !our_datadir || !legacy_datadir) {
        if (out) *out = r;
        LOG_FAIL("legacy_direct_import",
                 "bad args ms=%p coins=%p params=%p ourdir=%p legdir=%p",
                 (void *)ms, (void *)coins_tip, (const void *)params,
                 (const void *)our_datadir, (const void *)legacy_datadir);
    }

    char idx_dir[1024];
    char blk_dir[1024];
    snprintf(idx_dir, sizeof(idx_dir), "%s/blocks/index", legacy_datadir);
    snprintf(blk_dir, sizeof(blk_dir), "%s/blocks", legacy_datadir);

    /* ── Open legacy blocks/index ─────────────────────────── */
    int64_t t_open = ldi_now_ms();
    struct legacy_bootstrap_height_map_result hmap;
    if (!legacy_bootstrap_load_height_map(idx_dir, NULL,
                                          "legacy_direct_import", &hmap)) {
        if (out) *out = r;
        return false;
    }
    struct legacy_block_loc *map = hmap.map;
    size_t map_count = hmap.map_count;
    int legacy_tip = hmap.tip_height;
    r.legacy_tip = legacy_tip;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] legacy tip h=%d (map_count=%zu, "
            "load took %" PRId64 " ms)\n",
            legacy_tip, map_count, ldi_now_ms() - t_open);

    if (from_height < 0)
        from_height = active_chain_height(&ms->chain_active);
    if (from_height < 0) from_height = 0;
    if (from_height >= legacy_tip) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] already at/past legacy tip "
                "(from=%d legacy=%d) — nothing to do\n",
                from_height, legacy_tip);
        bilr_free_height_map(map);
        r.ok = true;
        r.final_tip = active_chain_height(&ms->chain_active);
        if (out) *out = r;
        return true;
    }

    /* ── Open mmap reader ─────────────────────────────────── */
    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(blk_dir, &bmr)) {
        bilr_free_height_map(map);
        if (out) *out = r;
        return false;
    }

    /* ── SHA3 spot-check source blocks; proofs still validate normally ── */
    if (legacy_bootstrap_spotcheck_sha3_windows(
            bmr, map, map_count, legacy_tip, LDI_SPOTCHECK_K,
            "legacy_direct_import", NULL, false)) {
        r.source_checked = true;
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] SHA3 source spotcheck passed; "
                "proof validation remains enabled\n");
    } else {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] WARNING: SHA3 spotcheck did not "
                "pass; continuing with full validation\n");
    }

    /* ── Arm body-pull I/O deferral ───────────────────────── */
    atomic_store(&g_body_pull_active, 1);

    /* ── Height walk + ingest ─────────────────────────────── */
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] starting walk: [%d+1 .. %d] "
            "(%d blocks)\n",
            from_height, legacy_tip, legacy_tip - from_height);

    int64_t t_walk = ldi_now_ms();
    int last_log_h = from_height;
    int64_t t_last_log = t_walk;
    bool ok = true;

    for (int h = from_height + 1; h <= legacy_tip; h++) {
        if (thread_registry_shutdown_requested()) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_direct_import] shutdown requested at h=%d\n", h);
            ok = false;
            break;
        }

        const struct legacy_block_loc *loc = &map[(size_t)h];
        if (loc->height < 0) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_direct_import] h=%d MISSING in legacy index "
                    "(gap in blocks/index/) — aborting\n", h);
            ok = false;
            break;
        }

        /* Skip if we already have this block on disk. */
        zcl_mutex_lock(&ms->cs_main);
        struct block_index *bi =
            block_map_find(&ms->map_block_index, &loc->hash);
        bool have_data = bi && (bi->nStatus & BLOCK_HAVE_DATA);
        bool failed    = bi && (bi->nStatus & BLOCK_FAILED_MASK);
        zcl_mutex_unlock(&ms->cs_main);
        if (have_data) {
            r.skipped_have_data++;
            continue;
        }
        if (failed) {
            r.skipped_failed++;
            continue;
        }

        size_t plen = 0;
        const uint8_t *payload =
            bmr_get_payload(bmr, loc->nFile, loc->nDataPos, &plen);
        if (!payload || plen == 0) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_direct_import] h=%d mmap fetch failed "
                    "(nFile=%d nDataPos=%u)\n",
                    h, loc->nFile, loc->nDataPos);
            ok = false;
            break;
        }

        struct byte_stream s;
        stream_init_from_data(&s, payload, plen);
        struct block block;
        block_init(&block);
        bool deser_ok = block_deserialize(&block, &s);
        stream_free(&s);
        if (!deser_ok) {
            block_free(&block);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_direct_import] h=%d block_deserialize "
                    "failed\n", h);
            ok = false;
            break;
        }

        struct validation_state vs;
        memset(&vs, 0, sizeof(vs));
        bool pn_ok = process_new_block(&vs, ms, coins_tip, params,
                                        &block, true, our_datadir);
        block_free(&block);
        if (!pn_ok) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_direct_import] h=%d process_new_block "
                    "FAILED: %s\n",
                    h, vs.reject_reason[0] ? vs.reject_reason : "(unknown)");
            ok = false;
            break;
        }
        r.applied++;

        /* Heartbeat every 1000 blocks OR every 2 seconds. */
        int64_t now = ldi_now_ms();
        if (h - last_log_h >= 1000 || (now - t_last_log) >= 2000) {
            int64_t elapsed = now - t_walk;
            double rate = elapsed > 0
                ? (double)r.applied * 1000.0 / (double)elapsed
                : 0.0;
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[legacy_direct_import] applied=%d h=%d rate=%.1f "
                    "bps (target=%d)\n",
                    r.applied, h, rate, legacy_tip);
            last_log_h = h;
            t_last_log = now;
        }
    }

    int64_t t_walk_end = ldi_now_ms();
    double total_secs = (double)(t_walk_end - t_walk) / 1000.0;
    double avg_rate = total_secs > 0.0
        ? (double)r.applied / total_secs : 0.0;

    /* ── Disarm body-pull mode ─────────────────────────────── */
    atomic_store(&g_body_pull_active, 0);

    /* ── Cleanup readers ──────────────────────────────────── */
    bmr_close(bmr);
    bilr_free_height_map(map);

    r.final_tip = active_chain_height(&ms->chain_active);
    r.ok = ok;

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] walk %s: applied=%d "
            "skipped_have=%d skipped_failed=%d elapsed=%.1fs "
            "rate=%.1f bps final_tip=%d\n",
            ok ? "complete" : "ABORTED",
            r.applied, r.skipped_have_data, r.skipped_failed,
            total_secs, avg_rate, r.final_tip);

    /* ── Auto-rescan wallet ───────────────────────────────── */
    if (ok && wallet && r.applied > 0) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] starting wallet rescan "
                "[%d..%d]...\n", from_height + 1, r.final_tip);
        int64_t t_rescan = ldi_now_ms();
        int hits = wallet_rescan(wallet, &ms->chain_active,
                                  from_height + 1, r.final_tip,
                                  our_datadir);
        double secs = (double)(ldi_now_ms() - t_rescan) / 1000.0;
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] wallet rescan complete: "
                "%d hits in %.1fs\n", hits, secs);
    }

    if (out) *out = r;
    return ok;
}
