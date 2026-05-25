/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_cold_import.c — see header.
 *
 * The pipeline does not call process_new_block or activate_best_chain
 * even once. It writes durable state directly:
 *
 *   - blk*.dat hardlinks (or copy fallback on EXDEV)
 *   - block-index records into our LevelDB
 *   - chainstate UTXOs into our coins.db
 *   - pending cold-import anchor metadata for CSR publication after
 *     boot has loaded the imported block index
 *
 * The normal boot then loads our LevelDB and chain_restore picks up
 * the pending anchor through CSR, populating active_chain by walking pprev.
 * bg_validation re-verifies every block bit-exact over the next hours.
 */

#include "platform/time_compat.h"
#include "services/legacy_cold_import.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/sha3_windows.h"
#include "core/random.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "services/legacy_bootstrap_importer.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/ldb_snapshot.h"
#include "models/database.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "coins/coins_view.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Refuse to cold-import when our active tip is at or above this. The
 * threshold is intentionally generous — a fresh genesis-only install
 * has height 0, an aborted previous import might leave 1-100. */
#define LCI_REFUSE_ABOVE_TIP 1000

#define LCI_SPOTCHECK_K 5
#define LCI_STAGE_SUBDIR "cold_import_ldb_snapshot"

static int64_t lci_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void lci_hex32(const uint8_t hash[32], char out[65])
{
    static const char hexdigits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hexdigits[hash[i] >> 4];
        out[i * 2 + 1] = hexdigits[hash[i] & 0x0f];
    }
    out[64] = '\0';
}

static void lci_log_window_loc(const struct legacy_block_loc *map,
                               size_t map_count,
                               int h)
{
    if (h < 0 || (size_t)h >= map_count)
        return;
    const struct legacy_block_loc *loc = &map[(size_t)h];
    if (loc->height < 0) {
        fprintf(stderr, // obs-ok:legacy-map-diagnostic
                "[cold_import] selected map h=%d: missing\n", h);
        return;
    }

    bool prev_ok = true;
    if (h > 0 && (size_t)(h - 1) < map_count &&
        map[(size_t)(h - 1)].height >= 0)
        prev_ok = uint256_eq(&loc->hashPrev, &map[(size_t)(h - 1)].hash);

    char hash_hex[65], prev_hex[65];
    lci_hex32(loc->hash.data, hash_hex);
    lci_hex32(loc->hashPrev.data, prev_hex);
    fprintf(stderr, // obs-ok:legacy-map-diagnostic
            "[cold_import] selected map h=%d hash=%.16s prev=%.16s "
            "file=%d pos=%u status=0x%x prev_ok=%d\n",
            h, hash_hex, prev_hex, loc->nFile, loc->nDataPos,
            loc->nStatus, prev_ok ? 1 : 0);
}

static void lci_log_window_map_diagnostics(
    const struct legacy_block_loc *map,
    size_t map_count,
    int start,
    int end)
{
    fprintf(stderr, // obs-ok:legacy-map-diagnostic
            "[cold_import] selected map diagnostic for h=%d..%d\n",
            start, end);

    for (int h = start; h <= end && h < start + 3; h++)
        lci_log_window_loc(map, map_count, h);
    int tail_start = end - 2;
    if (tail_start < start + 3)
        tail_start = start + 3;
    for (int h = tail_start; h <= end; h++)
        lci_log_window_loc(map, map_count, h);

    for (int h = start; h <= end; h++) {
        if (h <= 0 || (size_t)h >= map_count)
            continue;
        const struct legacy_block_loc *loc = &map[(size_t)h];
        const struct legacy_block_loc *prev = &map[(size_t)(h - 1)];
        if (loc->height < 0 || prev->height < 0 ||
            !uint256_eq(&loc->hashPrev, &prev->hash)) {
            fprintf(stderr, // obs-ok:legacy-map-diagnostic
                    "[cold_import] first selected-map break in window "
                    "near h=%d\n", h);
            lci_log_window_loc(map, map_count, h - 1);
            lci_log_window_loc(map, map_count, h);
            return;
        }
    }
    fprintf(stderr, // obs-ok:legacy-map-diagnostic
            "[cold_import] selected map has no parent break inside "
            "h=%d..%d\n", start, end);
}

/* Hash one SHA3 window using payloads from mmap. */
static bool lci_compute_window_hash(struct blocks_mmap *bmr,
                                    const struct legacy_block_loc *map,
                                    size_t map_count,
                                    size_t wi,
                                    uint8_t out[32])
{
    if (wi >= g_sha3_windows_count) return false;
    int start = g_sha3_windows[wi].start_height;
    int end   = start + SHA3_WINDOW_SIZE - 1;
    if (end < 0 || (size_t)end >= map_count) return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    for (int h = start; h <= end; h++) {
        const struct legacy_block_loc *loc = &map[(size_t)h];
        if (loc->height < 0) return false;
        size_t len = 0;
        const uint8_t *bytes =
            bmr_get_payload(bmr, loc->nFile, loc->nDataPos, &len);
        if (!bytes || len == 0) return false;
        sha3_256_write(&ctx, bytes, len);
    }

    uint8_t digest[32];
    sha3_256_finalize(&ctx, digest);
    memcpy(out, digest, 32);
    return true;
}

static bool lci_verify_window_logged(struct blocks_mmap *bmr,
                                     const struct legacy_block_loc *map,
                                     size_t map_count,
                                     size_t wi)
{
    uint8_t actual[32];
    int start = wi < g_sha3_windows_count ?
        g_sha3_windows[wi].start_height : -1;
    int end = start >= 0 ? start + SHA3_WINDOW_SIZE - 1 : -1;
    if (!lci_compute_window_hash(bmr, map, map_count, wi, actual)) {
        fprintf(stderr,
                "[cold_import] spotcheck FAILED at window %zu "
                "(h=%d..%d): unable to compute source digest\n",
                wi, start, end);
        lci_log_window_map_diagnostics(map, map_count, start, end);
        return false;
    }
    if (memcmp(actual, g_sha3_windows[wi].hash, 32) != 0) {
        char expected_hex[65], actual_hex[65];
        lci_hex32(g_sha3_windows[wi].hash, expected_hex);
        lci_hex32(actual, actual_hex);
        fprintf(stderr,
                "[cold_import] spotcheck FAILED at window %zu "
                "(h=%d..%d): expected=%s actual=%s — refusing to import\n",
                wi, start, end, expected_hex, actual_hex);
        lci_log_window_map_diagnostics(map, map_count, start, end);
        return false;
    }
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] spotcheck OK: w=%zu h=%d..%d\n",
            wi, start, end);
    return true;
}

static bool lci_spotcheck(struct blocks_mmap *bmr,
                          const struct legacy_block_loc *map,
                          size_t map_count,
                          int legacy_tip,
                          int k)
{
    if (g_sha3_windows_count == 0) return false;
    size_t max_w = g_sha3_windows_count;
    if (legacy_tip > 0) {
        size_t covered = (size_t)(legacy_tip + 1) / SHA3_WINDOW_SIZE;
        if (covered < max_w) max_w = covered;
    }
    if (max_w == 0) return false;
    if ((size_t)k > max_w) k = (int)max_w;

    size_t picked[16];
    if (k > (int)(sizeof(picked) / sizeof(picked[0])))
        k = (int)(sizeof(picked) / sizeof(picked[0]));

    const char *debug_window = getenv("ZCL_COLD_IMPORT_DEBUG_WINDOW");
    if (debug_window && debug_window[0]) {
        char *endp = NULL;
        errno = 0;
        unsigned long long forced = strtoull(debug_window, &endp, 10);
        if (errno || !endp || *endp != '\0' || forced >= max_w) {
            fprintf(stderr,
                    "[cold_import] invalid ZCL_COLD_IMPORT_DEBUG_WINDOW=%s "
                    "(valid range: 0..%zu)\n",
                    debug_window, max_w - 1);
            return false;
        }
        fprintf(stderr, // obs-ok:operator-requested-diagnostic
                "[cold_import] debug spotcheck window %llu requested\n",
                forced);
        if (!lci_verify_window_logged(bmr, map, map_count, (size_t)forced))
            return false;
    }

    unsigned char rand_buf[16 * 4];
    GetRandBytes(rand_buf, sizeof(rand_buf));
    for (int i = 0; i < k; i++) {
        uint32_t r = (uint32_t)rand_buf[i*4]
                   | ((uint32_t)rand_buf[i*4+1] << 8)
                   | ((uint32_t)rand_buf[i*4+2] << 16)
                   | ((uint32_t)rand_buf[i*4+3] << 24);
        picked[i] = (size_t)(r % max_w);
    }

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] SHA3 spotcheck: K=%d windows over [0..%zu) "
            "(legacy_tip=%d)\n", k, max_w, legacy_tip);
    for (int i = 0; i < k; i++) {
        if (!lci_verify_window_logged(bmr, map, map_count, picked[i]))
            return false;
    }
    return true;
}

bool legacy_cold_import_blocking(
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    const char *our_datadir,
    const char *legacy_datadir,
    struct lci_cold_result *out)
{
    struct lci_cold_result r = (struct lci_cold_result){0};
    r.legacy_tip = -1;
    if (out) *out = r;

    if (!ms || !cvs || !ndb || !ndb->open || !btdb ||
        !our_datadir || !legacy_datadir) {
        LOG_FAIL("legacy_cold_import", "bad args");
    }

    int our_tip = active_chain_height(&ms->chain_active);
    if (our_tip > LCI_REFUSE_ABOVE_TIP) {
        fprintf(stderr,
                "[cold_import] REFUSING: our active_tip=%d > %d. "
                "Cold-import is for empty datadirs; use -fastimport for "
                "warm catch-up.\n",
                our_tip, LCI_REFUSE_ABOVE_TIP);
        return false;
    }

    char blk_dir[1024];
    snprintf(blk_dir, sizeof(blk_dir), "%s/blocks", legacy_datadir);
    char our_blocks[1024];
    snprintf(our_blocks, sizeof(our_blocks), "%s/blocks", our_datadir);

    int64_t t_start = lci_now_ms();

    /* Snapshot legacy LevelDBs before reading them. The source zclassicd may
     * still be running and holding LOCK; the snapshot helper hardlinks
     * immutable SST files and gives us independent read-only LOCK contexts. */
    char stage_dir[1100], idx_dir[1200], cs_dir[1200];
    if (!legacy_bootstrap_make_stage_dir(our_datadir, LCI_STAGE_SUBDIR,
                                         stage_dir, sizeof(stage_dir),
                                         "cold_import")) {
        fprintf(stderr,
                "[cold_import] cannot create stage dir under %s\n",
                our_datadir);
        return false;
    }
    int64_t t_snap = lci_now_ms();
    if (!legacy_bootstrap_snapshot_leveldbs(legacy_datadir, stage_dir,
                                            idx_dir, sizeof(idx_dir),
                                            cs_dir, sizeof(cs_dir),
                                            "cold_import"))
        return false;
    fprintf(stderr, // obs-ok:cold-import-progress
            "[cold_import] LevelDB snapshots took %" PRId64 " ms\n",
            lci_now_ms() - t_snap);

    struct uint256 cs_best_for_map;
    void *cs_probe = NULL;
    if (!chainstate_legacy_open(cs_dir, &cs_probe)) {
        fprintf(stderr,
                "[cold_import] chainstate_legacy_open %s failed\n", cs_dir);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    if (!chainstate_legacy_get_best_block(cs_probe, &cs_best_for_map)) {
        chainstate_legacy_close(cs_probe);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        fprintf(stderr,
                "[cold_import] chainstate best block unavailable\n");
        return false;
    }
    chainstate_legacy_close(cs_probe);

    /* ── Build height map ──────────────────────────────────── */
    struct bilr *bilr = NULL;
    if (!bilr_open(idx_dir, &bilr)) {
        fprintf(stderr,
                "[cold_import] bilr_open %s failed\n", idx_dir);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    struct legacy_block_loc *map = NULL;
    size_t map_count = 0;
    if (!bilr_load_height_map_for_tip(bilr, &cs_best_for_map,
                                      &map, &map_count)) {
        bilr_close(bilr);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    int legacy_tip = (int)map_count - 1;
    while (legacy_tip > 0 && map[(size_t)legacy_tip].height < 0)
        legacy_tip--;
    r.legacy_tip = legacy_tip;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] legacy tip h=%d (map size=%zu)\n",
            legacy_tip, map_count);
    bilr_close(bilr);
    bilr = NULL;

    /* ── SHA3 spot-check ──────────────────────────────────── */
    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(blk_dir, &bmr)) {
        bilr_free_height_map(map);
        bilr_close(bilr);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    bool evidence_ok = lci_spotcheck(bmr, map, map_count,
                                   legacy_tip, LCI_SPOTCHECK_K);
    bmr_close(bmr);
    if (!evidence_ok) {
        bilr_free_height_map(map);
        bilr_close(bilr);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        fprintf(stderr,
                "[cold_import] aborting due to spotcheck failure\n");
        return false;
    }
    r.evidence_armed = true;

    /* ── Hardlink blk*.dat files ─────────────────────────── */
    int64_t t_link = lci_now_ms();
    int64_t linked = legacy_bootstrap_link_blk_files(blk_dir, our_blocks,
                                                     "cold_import");
    if (linked < 0) {
        bilr_free_height_map(map);
        bilr_close(bilr);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    r.blk_files_linked = linked;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] blk linking took %" PRId64 " ms\n",
            lci_now_ms() - t_link);

    /* ── Bulk-copy block_index into our LevelDB ──────────── */
    int64_t t_bi = lci_now_ms();
    struct uint256 legacy_tip_hash;
    int32_t legacy_tip_h = -1;
    int64_t bi_written = legacy_bootstrap_copy_block_index(
        idx_dir, btdb, &legacy_tip_hash, &legacy_tip_h,
        "legacy_cold_import.bulk_copy", "cold_import");
    bilr_free_height_map(map);
    bilr_close(bilr);
    if (bi_written < 0) {
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    r.block_index_writes = bi_written;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] block_index copy: %" PRId64 " entries "
            "in %" PRId64 " ms (best h=%d)\n",
            bi_written, lci_now_ms() - t_bi, legacy_tip_h);

    /* ── Bulk-import chainstate UTXOs ─────────────────────── */
    int64_t t_cs = lci_now_ms();

    enum { BATCH = 5000 };
    struct legacy_bootstrap_chainstate_import_result cs_import;
    if (!legacy_bootstrap_import_chainstate_utxos(
            cs_dir, cvs, BATCH, NULL, "cold_import", &cs_import)) {
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    r.utxos_imported = cs_import.inserted;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] chainstate: %" PRId64 " UTXOs from "
            "%" PRId64 " records in %" PRId64 " ms\n",
            cs_import.inserted, cs_import.records, lci_now_ms() - t_cs);

    /* ── Record an unpublished anchor for post-index CSR publication ── */
    if (cs_import.got_best_block) {
        bool pending_ok =
            node_db_state_set(ndb, "cold_import_pending_coins_best_block",
                              cs_import.best_block.data, 32) &&
            node_db_state_set(ndb, "cold_import_pending_coins_best_height",
                              &legacy_tip_h, sizeof(legacy_tip_h)) &&
            node_db_state_set(ndb, "cold_import_pending_utxo_count",
                              &cs_import.inserted,
                              sizeof(cs_import.inserted));
        if (!pending_ok) {
            fprintf(stderr,
                    "[cold_import] failed to persist pending CSR anchor\n");
            ldb_snapshot_destroy(idx_dir);
            ldb_snapshot_destroy(cs_dir);
            return false;
        }
        char hex[65] = {0};
        for (int i = 0; i < 32; i++)
            snprintf(hex + i*2, 3, "%02x",
                     cs_import.best_block.data[31 - i]);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[cold_import] pending CSR anchor recorded %s h=%d\n",
                hex, legacy_tip_h);
    } else {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[cold_import] WARNING: legacy chainstate had no 'B' key; "
                "pending CSR anchor not recorded\n");
    }

    r.total_secs = (double)(lci_now_ms() - t_start) / 1000.0;
    r.ok = true;
    if (out) *out = r;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] DONE in %.1fs: block_index=%" PRId64
            " utxos=%" PRId64 " blk_files=%" PRId64 "\n",
            r.total_secs, r.block_index_writes, r.utxos_imported,
            r.blk_files_linked);
    ldb_snapshot_destroy(idx_dir);
    ldb_snapshot_destroy(cs_dir);
    return true;
}
