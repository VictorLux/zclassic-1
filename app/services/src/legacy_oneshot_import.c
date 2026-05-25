/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * legacy_oneshot_import.c — see header.
 *
 * Pipeline:
 *   0. Pre-flight: validate args, refuse if our active tip > threshold,
 *      confirm legacy dir has blocks/index + chainstate.
 *   1. Idempotency check via progress_meta (read sentinel + last
 *      attached tip metadata).
 *   2. Snapshot legacy LevelDBs into <our_datadir>/legacy-attach-stage/.
 *      Retry once on `manifest_changed`.
 *   3. Set the `import_in_progress` sentinel.
 *   4. Hardlink blk*.dat from legacy/blocks/ into our blocks/ (EXDEV
 *      → copy fallback).
 *   5. Bulk-copy block_index entries from legacy snapshot into our
 *      LevelDB. Capture the tip hash + height.
 *   6. Bulk-import chainstate UTXOs from legacy snapshot into coins.db.
 *      Capture the legacy 'B' key as the canonical best block.
 *   7. Persist `cold_import_pending_*` keys in node.db so the existing
 *      `boot_resolve_cold_import_pending_anchor` can publish the tip
 *      via CSR later in boot.
 *   8. Atomic finalization (single BEGIN IMMEDIATE on progress.kv):
 *        - UPSERT stage_cursor for "header_admit" = legacy_tip+1
 *        - UPSERT stage_cursor for "validate_headers" = legacy_tip+1
 *        - UPSERT stage_cursor for "body_fetch" = legacy_tip+1
 *        - SET progress_meta legacy_attach_tip_{hash,height,done_at}
 *        - DELETE progress_meta import_in_progress
 *      COMMIT — the saga atomicity contract: cursors and the
 *      completion record commit or roll back together.
 *   9. Tear down the snapshot directory. */

#include "platform/time_compat.h"
#include "services/legacy_oneshot_import.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "models/database.h"
#include "services/legacy_bootstrap_importer.h"
#include "storage/block_index_db.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/dbwrapper.h"
#include "storage/ldb_snapshot.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Refuse to one-shot import when our active tip is at or above this.
 * Same threshold as legacy_cold_import — keeps the bootstrap mode
 * separate from steady-state operation. */
#define LOI_REFUSE_ABOVE_TIP 1000

/* Refuse to stamp cursors if the discovered legacy tip is below this.
 * A genuine legacy datadir has hundreds of thousands of blocks; below
 * this threshold something is wrong (corrupt LevelDB, wrong directory,
 * fresh zclassicd that hasn't synced). Stamping cursors at tiny values
 * is worse than stamping nothing — it silently advertises "we've
 * processed the early chain" when we haven't. */
#define LOI_MIN_LEGACY_TIP 100

/* progress_meta keys (kept here, not in the header — internal protocol). */
#define LOI_META_SENTINEL      "import_in_progress"
#define LOI_META_TIP_HASH      "legacy_attach_tip_hash"
#define LOI_META_TIP_HEIGHT    "legacy_attach_tip_height"
#define LOI_META_DONE_AT       "legacy_attach_done_at"

/* Subdir under our datadir for the legacy LevelDB snapshots. */
#define LOI_STAGE_SUBDIR       "legacy-attach-stage"

const char *loi_outcome_name(enum loi_outcome o)
{
    switch (o) {
        case LOI_OUTCOME_DID_IMPORT:           return "did_import";
        case LOI_OUTCOME_NOOP_SAME_TIP:        return "noop_same_tip";
        case LOI_OUTCOME_RECOVERED_FROM_CRASH: return "recovered_from_crash";
        case LOI_OUTCOME_REFUSED_HAS_STATE:    return "refused_has_state";
        case LOI_OUTCOME_LEGACY_NOT_FOUND:     return "legacy_not_found";
        case LOI_OUTCOME_FAILED:               return "failed";
    }
    return "?";
}

static int64_t loi_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool path_isfile(const char *p)
{
    struct stat st;
    if (!p) return false;
    if (stat(p, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

/* Conservative legacy-datadir detection: must have blocks/index/CURRENT
 * and chainstate/CURRENT and blocks/blk00000.dat. */
static bool loi_detect_legacy_datadir(const char *legacy_datadir)
{
    if (!legacy_datadir || !legacy_datadir[0]) return false;
    char p[1100];
    snprintf(p, sizeof(p), "%s/blocks/index/CURRENT", legacy_datadir);
    if (!path_isfile(p)) return false;
    snprintf(p, sizeof(p), "%s/chainstate/CURRENT", legacy_datadir);
    if (!path_isfile(p)) return false;
    snprintf(p, sizeof(p), "%s/blocks/blk00000.dat", legacy_datadir);
    if (!path_isfile(p)) return false;
    return true;
}

/* ── progress_meta convenience wrappers (typed) ──────────────────────── */

static bool loi_meta_has_sentinel(sqlite3 *db)
{
    if (!db) return false;
    uint8_t buf[1];
    size_t got = 0;
    bool found = false;
    if (!progress_meta_get(db, LOI_META_SENTINEL,
                           buf, sizeof(buf), &got, &found))
        return false;
    return found;
}

static bool loi_meta_get_tip(sqlite3 *db,
                             struct uint256 *out_hash,
                             int32_t *out_height,
                             bool *out_found)
{
    if (out_hash) memset(out_hash, 0, sizeof(*out_hash));
    if (out_height) *out_height = -1;
    if (out_found) *out_found = false;

    if (!db) return false;
    bool fh = false, fH = false;
    size_t nh = 0, nH = 0;
    if (!progress_meta_get(db, LOI_META_TIP_HASH,
                           out_hash ? out_hash->data : NULL,
                           out_hash ? sizeof(out_hash->data) : 0,
                           &nh, &fh)) return false;
    if (!progress_meta_get(db, LOI_META_TIP_HEIGHT,
                           out_height, sizeof(*out_height), &nH, &fH))
        return false;
    if (!fh || !fH) return true;  /* not found is not an error */
    if (nh != 32 || nH != sizeof(*out_height)) return true;
    if (out_found) *out_found = true;
    return true;
}

/* ── atomic finalization on progress.kv ──────────────────────────────── */

/* ── Wave S stage stamp list ──────────────────────────────────────────
 *
 * **Read the long comment in legacy_oneshot_import.h before editing.**
 * Every addition or removal here MUST be paired with the EXPECTED list
 * in lib/test/src/test_legacy_oneshot_import.c::EXPECTED_LOI_STAGES.
 * The drift-gate test asserts both lists match exactly. */
static const char *const LOI_STAGES_TO_STAMP[] = {
    "header_admit",
    "validate_headers",
    "body_fetch",
    NULL,
};

/* Computed once at init: how many real (non-NULL) entries the list has. */
static size_t loi_stages_count_cached(void)
{
    size_t n = 0;
    while (LOI_STAGES_TO_STAMP[n]) n++;
    return n;
}

size_t loi_stages_to_stamp_count(void)
{
    return loi_stages_count_cached();
}

const char *loi_stages_to_stamp_at(size_t i)
{
    if (i >= loi_stages_count_cached()) return NULL;
    return LOI_STAGES_TO_STAMP[i];
}

/* Stamp a stage cursor with anti-rewind semantics: if the existing
 * persisted cursor is already >= new_cursor, leave it untouched. This
 * prevents -legacy-attach from rewinding a stage that has already
 * advanced past legacy_tip+1 via the live shadow pipeline (or via a
 * prior -legacy-attach against a more recent legacy tip).
 *
 * Returns true on success (whether or not a write occurred). Sets
 * *out_was_write to true iff an UPSERT actually happened. */
static bool loi_stamp_stage_cursor_in_tx(sqlite3 *db,
                                         const char *name,
                                         uint64_t new_cursor,
                                         bool *out_was_write)
{
    if (out_was_write) *out_was_write = false;

    /* Read existing cursor (defaults to 0 on miss). */
    uint64_t existing = 0;
    bool have_row = false;
    {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT cursor FROM stage_cursor WHERE name = ?",
                -1, &q, NULL) != SQLITE_OK) return false;
        sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(q);  // raw-sql-ok:kernel-primitive
        if (rc == SQLITE_ROW) {
            existing = (uint64_t)sqlite3_column_int64(q, 0);
            have_row = true;
        } else if (rc != SQLITE_DONE) {
            sqlite3_finalize(q);
            return false;
        }
        sqlite3_finalize(q);
    }

    /* Anti-rewind: never move cursor backward or stay-in-place. */
    if (have_row && existing >= new_cursor) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-no-rewind
                "[legacy_attach] stage '%s': cursor already at %" PRIu64
                " (>= proposed %" PRIu64 "); leaving as-is\n",
                name, existing, new_cursor);
        return true;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO stage_cursor(name, cursor, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  cursor = excluded.cursor, updated_at = excluded.updated_at",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)new_cursor);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)platform_time_wall_time_t());
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && out_was_write) *out_was_write = true;
    return rc == SQLITE_DONE;
}

/* Test seam: exercise the anti-rewind logic from a unit test without
 * spinning up the full pipeline. Wraps the static helper above in a
 * BEGIN IMMEDIATE / COMMIT for own-txn semantics. */
bool loi_stamp_one_for_test(sqlite3 *db, const char *name,
                            uint64_t cursor, bool *out_was_write)
{
    if (!db) return false;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = loi_stamp_stage_cursor_in_tx(db, name, cursor, out_was_write);
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

static bool loi_finalize_atomic(sqlite3 *db,
                                int32_t legacy_tip,
                                const struct uint256 *legacy_tip_hash,
                                int64_t *out_stages_stamped)
{
    if (out_stages_stamped) *out_stages_stamped = 0;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-finalize-failure
                "[legacy_attach] finalize BEGIN failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    bool ok = true;
    int64_t stamped = 0;
    uint64_t cursor_value = (uint64_t)(legacy_tip + 1);
    for (size_t i = 0; ok && LOI_STAGES_TO_STAMP[i]; i++) {
        bool was_write = false;
        if (!loi_stamp_stage_cursor_in_tx(db, LOI_STAGES_TO_STAMP[i],
                                          cursor_value, &was_write)) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-finalize-failure
                    "[legacy_attach] stamp stage '%s' failed\n",
                    LOI_STAGES_TO_STAMP[i]);
            ok = false;
        } else if (was_write) {
            stamped++;
        }
    }

    if (ok) {
        ok = progress_meta_set_in_tx(db, LOI_META_TIP_HASH,
                                     legacy_tip_hash->data, 32);
    }
    if (ok) {
        ok = progress_meta_set_in_tx(db, LOI_META_TIP_HEIGHT,
                                     &legacy_tip, sizeof(legacy_tip));
    }
    if (ok) {
        int64_t now = (int64_t)platform_time_wall_time_t();
        ok = progress_meta_set_in_tx(db, LOI_META_DONE_AT,
                                     &now, sizeof(now));
    }
    if (ok) {
        ok = progress_meta_delete_in_tx(db, LOI_META_SENTINEL);
    }

    const char *fini = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, fini, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-finalize-failure
                "[legacy_attach] finalize %s failed: %s\n",
                fini, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    if (out_stages_stamped) *out_stages_stamped = stamped;
    return ok;
}

/* Wipe block_index entries written by a prior aborted import. We
 * delete every 'b'-prefixed key from our LevelDB. After a crash mid-
 * import the caller can repopulate them cleanly. blk*.dat hardlinks
 * are idempotent (skipped if present) so we leave them. coins.db rows
 * are also idempotent (INSERT OR REPLACE) so we leave them; the next
 * full pass overwrites every row. */
static bool loi_wipe_block_index(struct block_tree_db *our_btdb)
{
    struct db_wrapper *db = &our_btdb->db;
    struct db_iterator it;
    db_iter_init(&it, db);
    const char seek_key = 'b';
    db_iter_seek(&it, &seek_key, 1);
    struct db_batch batch;
    db_batch_init(&batch);
    int64_t deleted = 0;
    while (db_iter_valid(&it)) {
        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 1 || k[0] != 'b') break;
        db_batch_delete(&batch, k, klen);
        deleted++;
        if (deleted % 5000 == 0) {
            if (!db_write_batch(db, &batch, false)) {
                db_batch_free(&batch);
                db_iter_free(&it);
                return false;
            }
            db_batch_clear(&batch);
        }
        db_iter_next(&it);
    }
    bool ok = db_write_batch(db, &batch, false);
    db_batch_free(&batch);
    db_iter_free(&it);
    fprintf(stderr,  // obs-ok:legacy-oneshot-wipe
            "[legacy_attach] wiped %" PRId64 " block_index entries "
            "from prior aborted import\n", deleted);
    return ok;
}

/* ── main entry ──────────────────────────────────────────────────────── */

bool legacy_oneshot_import_run(
    const char *our_datadir,
    const char *legacy_datadir,
    struct main_state *ms,
    struct coins_view_sqlite *cvs,
    struct node_db *ndb,
    struct block_tree_db *btdb,
    struct loi_result *out)
{
    struct loi_result r = {0};
    r.legacy_tip_height = -1;
    r.outcome = LOI_OUTCOME_FAILED;
    if (out) *out = r;

    if (!our_datadir || !legacy_datadir || !ms || !cvs ||
        !ndb || !ndb->open || !btdb) {
        LOG_FAIL("legacy_oneshot_import", "bad args");
    }

    sqlite3 *pdb = progress_store_db();
    if (!pdb) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-preflight
            "[legacy_attach] progress.kv not open — boot order regression?\n");
        return false;
    }

    /* ── Pre-flight ──────────────────────────────────────────────────── */
    if (!loi_detect_legacy_datadir(legacy_datadir)) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-soft-skip
            "[legacy_attach] %s does not look like a zclassic datadir; "
            "skipping.\n", legacy_datadir);
        r.outcome = LOI_OUTCOME_LEGACY_NOT_FOUND;
        if (out) *out = r;
        return true;  /* soft skip — not a failure */
    }

    int our_tip = active_chain_height(&ms->chain_active);
    bool sentinel_present = loi_meta_has_sentinel(pdb);
    if (our_tip > LOI_REFUSE_ABOVE_TIP && !sentinel_present) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-refused
            "[legacy_attach] REFUSING: our active_tip=%d > %d. "
            "Legacy-attach is for empty datadirs; use other modes for "
            "warm catch-up.\n", our_tip, LOI_REFUSE_ABOVE_TIP);
        r.outcome = LOI_OUTCOME_REFUSED_HAS_STATE;
        if (out) *out = r;
        return true;
    }

    /* ── Idempotency: skip if we already attached to this legacy tip. ── */
    /* Cheap probe: open chainstate snapshot for 'B' read only.
     * Avoid the full snapshot/import work if last_attach_tip matches. */
    if (!sentinel_present) {
        struct uint256 last_hash;
        int32_t last_h = -1;
        bool last_found = false;
        if (loi_meta_get_tip(pdb, &last_hash, &last_h, &last_found) &&
            last_found) {
            /* Build a tiny snapshot of just chainstate to read 'B'. */
            char stage_dir[1100], cs_path[1200];
            if (legacy_bootstrap_make_stage_dir(our_datadir,
                                                LOI_STAGE_SUBDIR,
                                                stage_dir,
                                                sizeof(stage_dir),
                                                "legacy_attach")) {
                snprintf(cs_path, sizeof(cs_path),
                         "%s/probe-chainstate", stage_dir);
                char err[128] = {0};
                char src_cs[1100];
                snprintf(src_cs, sizeof(src_cs), "%s/chainstate",
                         legacy_datadir);
                if (ldb_snapshot_make(src_cs, cs_path, err, sizeof(err))) {
                    void *cs = NULL;
                    if (chainstate_legacy_open(cs_path, &cs)) {
                        struct uint256 cur_best;
                        if (chainstate_legacy_get_best_block(cs, &cur_best) &&
                            memcmp(cur_best.data, last_hash.data, 32) == 0) {
                            chainstate_legacy_close(cs);
                            ldb_snapshot_destroy(cs_path);
                            r.outcome = LOI_OUTCOME_NOOP_SAME_TIP;
                            r.legacy_tip_height = last_h;
                            r.ok = true;
                            if (out) *out = r;
                            fprintf(stderr,  // obs-ok:legacy-oneshot-noop
                                "[legacy_attach] NOOP: already attached "
                                "to legacy tip h=%d\n", last_h);
                            return true;
                        }
                        chainstate_legacy_close(cs);
                    }
                    ldb_snapshot_destroy(cs_path);
                }
            }
        }
    } else {
        /* Wipe guard — never wipe block_index if our active_chain has
         * substantial state. A sentinel + live state is anomalous (the
         * sentinel is supposed to clear atomically at import-end; a live
         * chain means we got past S-4b cleanly at some point). Refuse
         * and leave the operator to diagnose. */
        if (our_tip > 100) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-wipe-refused
                "[legacy_attach] REFUSING wipe: sentinel found AND "
                "active_chain_height=%d > 100. This combination is "
                "anomalous (sentinel should clear atomically with import "
                "completion). Manual intervention required: inspect "
                "progress.kv (sqlite3 -- DELETE FROM progress_meta WHERE "
                "key='import_in_progress') if the sentinel is truly "
                "stale.\n", our_tip);
            return false;
        }
        fprintf(stderr,  // obs-ok:legacy-oneshot-recovery
                "[legacy_attach] sentinel found from a prior aborted "
                "import — recovering: wipe + re-import (active_tip=%d)\n",
                our_tip);
        if (!loi_wipe_block_index(btdb)) {
            fprintf(stderr,
                "[legacy_attach] wipe of stale block_index failed\n");
            return false;
        }
        r.outcome = LOI_OUTCOME_RECOVERED_FROM_CRASH;
    }

    int64_t t_start = loi_now_ms();

    /* ── Set sentinel before doing any work. ─────────────────────────── */
    uint8_t one = 1;
    if (!progress_meta_set(pdb, LOI_META_SENTINEL, &one, 1)) {
        fprintf(stderr,
            "[legacy_attach] failed to set in-progress sentinel\n");
        return false;
    }

    /* ── Snapshot legacy LevelDBs. ───────────────────────────────────── */
    char stage_dir[1100], idx_snap[1200], cs_snap[1200];
    if (!legacy_bootstrap_make_stage_dir(our_datadir, LOI_STAGE_SUBDIR,
                                         stage_dir, sizeof(stage_dir),
                                         "legacy_attach")) {
        fprintf(stderr,
            "[legacy_attach] cannot create stage dir under %s\n",
            our_datadir);
        return false;
    }
    if (!legacy_bootstrap_snapshot_leveldbs(legacy_datadir, stage_dir,
                                            idx_snap, sizeof(idx_snap),
                                            cs_snap, sizeof(cs_snap),
                                            "legacy_attach")) {
        return false;
    }

    /* ── Shared snapshot import: blk*.dat + block_index + UTXOs. ────── */
    char legacy_blocks[1100], our_blocks[1100];
    snprintf(legacy_blocks, sizeof(legacy_blocks), "%s/blocks", legacy_datadir);
    snprintf(our_blocks, sizeof(our_blocks), "%s/blocks", our_datadir);
    struct legacy_bootstrap_snapshot_import_result imported;
    const struct legacy_bootstrap_snapshot_import_options import_opts = {
        .legacy_blocks_dir = legacy_blocks,
        .our_blocks_dir = our_blocks,
        .legacy_index_dir = idx_snap,
        .chainstate_dir = cs_snap,
        .btdb = btdb,
        .cvs = cvs,
        .ndb = ndb,
        .chainstate_batch_limit = 50000,
        .min_legacy_tip = LOI_MIN_LEGACY_TIP,
        .require_best_block = true,
        .block_index_long_op_name = "legacy_attach.bi_copy",
        .chainstate_long_op_name = "legacy_attach.cs_import",
        .log_prefix = "legacy_attach",
    };
    int64_t t_import = loi_now_ms();
    if (!legacy_bootstrap_import_snapshot_state(&import_opts, &imported)) {
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    r.blk_files_linked = imported.blk_files_linked;
    r.block_index_writes = imported.block_index_writes;
    r.utxos_imported = imported.utxos_imported;
    r.legacy_tip_height = imported.legacy_tip_height;
    fprintf(stderr,  // obs-ok:legacy-oneshot-progress
            "[legacy_attach] snapshot state import took %" PRId64 " ms "
            "(best h=%d records=%" PRId64 ")\n",
            loi_now_ms() - t_import, imported.legacy_tip_height,
            imported.chainstate_records);

    /* Use the chainstate 'B' hash as the canonical legacy tip hash for
     * the cursor-stamp record. */
    const struct uint256 *cursor_hash = &imported.best_block;

    /* ── Atomic finalization: cursors + completion record + sentinel ── */
    int64_t stages_stamped = 0;
    if (!loi_finalize_atomic(pdb, imported.legacy_tip_height, cursor_hash,
                             &stages_stamped)) {
        /* Sentinel left set so the next boot retries. */
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    r.stages_stamped = stages_stamped;
    r.evidence_armed = false;  /* SHA3 spotcheck deferred to follow-up */

    /* ── Tear down snapshots. ────────────────────────────────────────── */
    ldb_snapshot_destroy(idx_snap);
    ldb_snapshot_destroy(cs_snap);
    /* Best-effort remove of stage_dir if empty. */
    rmdir(stage_dir);

    r.total_secs = (double)(loi_now_ms() - t_start) / 1000.0;
    if (r.outcome == LOI_OUTCOME_FAILED)
        r.outcome = LOI_OUTCOME_DID_IMPORT;
    r.ok = true;
    if (out) *out = r;

    fprintf(stderr,  // obs-ok:legacy-oneshot-done
        "[legacy_attach] DONE outcome=%s in %.1fs: legacy_tip=%d "
        "block_index=%" PRId64 " utxos=%" PRId64 " blk_files=%" PRId64
        " stages_stamped=%" PRId64 "\n",
        loi_outcome_name(r.outcome), r.total_secs, r.legacy_tip_height,
        r.block_index_writes, r.utxos_imported, r.blk_files_linked,
        r.stages_stamped);

    return true;
}
