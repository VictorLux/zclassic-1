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
#include "core/uint256.h"
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

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
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
    struct legacy_bootstrap_height_map_result hmap;
    if (!legacy_bootstrap_load_height_map(idx_dir, &cs_best_for_map,
                                          "cold_import", &hmap)) {
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    struct legacy_block_loc *map = hmap.map;
    size_t map_count = hmap.map_count;
    int legacy_tip = hmap.tip_height;
    r.legacy_tip = legacy_tip;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] legacy tip h=%d (map size=%zu)\n",
            legacy_tip, map_count);

    /* ── SHA3 spot-check ──────────────────────────────────── */
    struct legacy_bootstrap_block_source source;
    const struct legacy_bootstrap_block_source_options source_opts = {
        .legacy_blocks_dir = blk_dir,
        .map = map,
        .map_count = map_count,
        .legacy_tip = legacy_tip,
        .spotcheck_k = LCI_SPOTCHECK_K,
        .require_spotcheck = true,
        .log_prefix = "cold_import",
        .debug_env = "ZCL_COLD_IMPORT_DEBUG_WINDOW",
        .dump_map_on_failure = true,
    };
    if (!legacy_bootstrap_open_block_source(&source_opts, &source)) {
        bilr_free_height_map(map);
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    legacy_bootstrap_close_block_source(&source);
    r.evidence_armed = true;

    /* ── Shared snapshot import: blk*.dat + block_index + UTXOs ─────── */
    struct legacy_bootstrap_snapshot_import_result imported;
    const struct legacy_bootstrap_snapshot_import_options import_opts = {
        .legacy_blocks_dir = blk_dir,
        .our_blocks_dir = our_blocks,
        .legacy_index_dir = idx_dir,
        .chainstate_dir = cs_dir,
        .btdb = btdb,
        .cvs = cvs,
        .ndb = ndb,
        .chainstate_batch_limit = 5000,
        .min_legacy_tip = -1,
        .require_best_block = false,
        .block_index_long_op_name = "legacy_cold_import.bulk_copy",
        .chainstate_long_op_name = NULL,
        .log_prefix = "cold_import",
    };
    int64_t t_import = lci_now_ms();
    bool import_ok = legacy_bootstrap_import_snapshot_state(
        &import_opts, &imported);
    bilr_free_height_map(map);
    if (!import_ok) {
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    r.blk_files_linked = imported.blk_files_linked;
    r.block_index_writes = imported.block_index_writes;
    r.utxos_imported = imported.utxos_imported;
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[cold_import] snapshot state import took %" PRId64 " ms "
            "(best h=%d records=%" PRId64 ")\n",
            lci_now_ms() - t_import, imported.legacy_tip_height,
            imported.chainstate_records);

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
