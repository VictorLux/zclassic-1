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

#include "services/legacy_oneshot_import.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "models/database.h"
#include "storage/block_index_db.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/dbwrapper.h"
#include "storage/ldb_snapshot.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/long_op.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Refuse to one-shot import when our active tip is at or above this.
 * Same threshold as legacy_cold_import — keeps the bootstrap mode
 * separate from steady-state operation. */
#define LOI_REFUSE_ABOVE_TIP 1000

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
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

/* ── snapshot stage dir ──────────────────────────────────────────────── */

static bool loi_make_stage_dir(const char *our_datadir, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", our_datadir, LOI_STAGE_SUBDIR);
    if (n <= 0 || (size_t)n >= cap) return false;
    if (mkdir(out, 0700) != 0 && errno != EEXIST) return false;
    return true;
}

/* Snapshot legacy/blocks/index AND legacy/chainstate. Try once; if the
 * blocks/index snapshot reports manifest_changed (live writer rolled
 * the manifest while we copied), retry once. */
static bool loi_snapshot_legacy(const char *legacy_datadir,
                                const char *stage_dir,
                                char *out_idx_path, size_t idx_cap,
                                char *out_cs_path, size_t cs_cap)
{
    char src_idx[1100], src_cs[1100];
    snprintf(src_idx, sizeof(src_idx), "%s/blocks/index", legacy_datadir);
    snprintf(src_cs,  sizeof(src_cs),  "%s/chainstate",   legacy_datadir);

    int ni = snprintf(out_idx_path, idx_cap, "%s/blocks-index", stage_dir);
    int nc = snprintf(out_cs_path,  cs_cap,  "%s/chainstate",   stage_dir);
    if (ni <= 0 || (size_t)ni >= idx_cap) return false;
    if (nc <= 0 || (size_t)nc >= cs_cap) return false;

    char err[128];
    for (int tries = 0; tries < 3; tries++) {
        err[0] = '\0';
        if (ldb_snapshot_make(src_idx, out_idx_path, err, sizeof(err))) break;
        if (strcmp(err, "manifest_changed") != 0) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-snapshot-failure
                    "[legacy_attach] snapshot of %s failed: %s\n",
                    src_idx, err);
            return false;
        }
        fprintf(stderr,  // obs-ok:legacy-oneshot-snapshot-retry
                "[legacy_attach] snapshot %s manifest_changed; retry %d\n",
                src_idx, tries + 1);
    }
    err[0] = '\0';
    for (int tries = 0; tries < 3; tries++) {
        if (ldb_snapshot_make(src_cs, out_cs_path, err, sizeof(err))) break;
        if (strcmp(err, "manifest_changed") != 0) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-snapshot-failure
                    "[legacy_attach] snapshot of %s failed: %s\n",
                    src_cs, err);
            ldb_snapshot_destroy(out_idx_path);
            return false;
        }
        fprintf(stderr,  // obs-ok:legacy-oneshot-snapshot-retry
                "[legacy_attach] snapshot %s manifest_changed; retry %d\n",
                src_cs, tries + 1);
    }
    return true;
}

/* ── blk*.dat hardlink (or copy on EXDEV) ────────────────────────────── */

static int64_t loi_link_blk_files(const char *legacy_blocks_dir,
                                  const char *our_blocks_dir)
{
    DIR *d = opendir(legacy_blocks_dir);
    if (!d) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-blk-failure
                "[legacy_attach] cannot opendir %s: %s\n",
                legacy_blocks_dir, strerror(errno));
        return -1;  // raw-return-ok:logged-above
    }
    mkdir(our_blocks_dir, 0755);

    int64_t linked = 0, copied = 0, skipped = 0, errors = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 12 || strncmp(de->d_name, "blk", 3) != 0 ||
            strcmp(de->d_name + nlen - 4, ".dat") != 0)
            continue;
        char src[1200], dst[1200];
        snprintf(src, sizeof(src), "%s/%s", legacy_blocks_dir, de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", our_blocks_dir, de->d_name);
        struct stat st;
        if (stat(dst, &st) == 0) { skipped++; continue; }
        if (link(src, dst) == 0) { linked++; continue; }
        if (errno != EXDEV && errno != EPERM) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-blk-failure
                    "[legacy_attach] link(%s -> %s) failed: %s\n",
                    src, dst, strerror(errno));
            errors++;
            continue;
        }
        FILE *fsrc = fopen(src, "rb");
        FILE *fdst = fopen(dst, "wb");
        if (!fsrc || !fdst) {
            if (fsrc) fclose(fsrc);
            if (fdst) fclose(fdst);
            errors++;
            continue;
        }
        char buf[1u << 20];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
            if (fwrite(buf, 1, n, fdst) != n) { errors++; break; }
        }
        fclose(fsrc);
        fclose(fdst);
        copied++;
    }
    closedir(d);
    fprintf(stderr,  // obs-ok:legacy-oneshot-blk-progress
            "[legacy_attach] blk files: linked=%" PRId64 " copied=%" PRId64
            " skipped=%" PRId64 " errors=%" PRId64 "\n",
            linked, copied, skipped, errors);
    return (errors > 0) ? -1 : (linked + copied);
}

/* ── block_index bulk copy ───────────────────────────────────────────── */

static int64_t loi_copy_block_index(const char *legacy_idx_snapshot,
                                    struct block_tree_db *our_btdb,
                                    struct uint256 *out_tip_hash,
                                    int32_t *out_tip_height)
{
    if (out_tip_height) *out_tip_height = -1;
    if (out_tip_hash) memset(out_tip_hash, 0, sizeof(*out_tip_hash));

    struct db_wrapper src;
    if (!db_wrapper_open(&src, legacy_idx_snapshot,
                         16u << 20, false, false)) {
        fprintf(stderr,  // obs-ok:legacy-oneshot-bi-failure
                "[legacy_attach] cannot open snapshot %s\n",
                legacy_idx_snapshot);
        return -1;  // raw-return-ok:logged-above
    }

    struct db_wrapper *dst = &our_btdb->db;

    struct db_iterator it;
    db_iter_init(&it, &src);
    const char seek_key = 'b';
    db_iter_seek(&it, &seek_key, 1);

    struct db_batch batch;
    db_batch_init(&batch);
    int64_t written = 0;
    int64_t batch_fill = 0;
    enum { BATCH_LIMIT = 5000 };
    int32_t best_h = -1;

    struct long_op_scope lo_scope;
    long_op_begin(&lo_scope, "legacy_attach.bi_copy");

    while (db_iter_valid(&it)) {
        if (thread_registry_shutdown_requested()) break;

        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 1 || k[0] != 'b') break;
        if (klen != 33) { db_iter_next(&it); continue; }

        size_t vlen = 0;
        const char *v = db_iter_value(&it, &vlen);
        if (!v || vlen == 0) { db_iter_next(&it); continue; }

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        struct byte_stream s;
        stream_init_from_data(&s, (unsigned char *)v, vlen);
        bool ok = disk_block_index_deserialize(&dbi, &s);
        stream_free(&s);
        if (!ok) { db_iter_next(&it); continue; }

        bool have_data = (dbi.nStatus & BLOCK_HAVE_DATA) != 0;
        bool failed = (dbi.nStatus & BLOCK_FAILED_MASK) != 0;
        if (!have_data || failed) { db_iter_next(&it); continue; }

        if (dbi.nHeight > best_h) {
            best_h = dbi.nHeight;
            if (out_tip_hash)
                memcpy(out_tip_hash->data, k + 1, 32);
        }

        db_batch_put(&batch, k, klen, v, vlen);
        batch_fill++;
        written++;

        if (batch_fill >= BATCH_LIMIT) {
            if (!db_write_batch(dst, &batch, false)) {
                fprintf(stderr,  // obs-ok:legacy-oneshot-bi-failure
                        "[legacy_attach] db_write_batch failed\n");
                db_batch_free(&batch);
                db_iter_free(&it);
                db_wrapper_close(&src);
                long_op_end(&lo_scope);
                return -1;  // raw-return-ok:logged-above
            }
            db_batch_clear(&batch);
            batch_fill = 0;
            long_op_tick(&lo_scope);
        }

        db_iter_next(&it);
    }

    if (batch_fill > 0) {
        if (!db_write_batch(dst, &batch, false)) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-bi-failure
                    "[legacy_attach] final db_write_batch failed\n");
            db_batch_free(&batch);
            db_iter_free(&it);
            db_wrapper_close(&src);
            long_op_end(&lo_scope);
            return -1;  // raw-return-ok:logged-above
        }
    }
    db_batch_free(&batch);
    db_iter_free(&it);
    db_wrapper_close(&src);
    long_op_end(&lo_scope);

    if (out_tip_height) *out_tip_height = best_h;
    return written;
}

/* ── chainstate UTXO bulk import ─────────────────────────────────────── */

struct loi_cs_ctx {
    struct utxo_bulk_rec *batch;
    size_t fill, cap;
    struct coins_view_sqlite *cvs;
    int64_t inserted, records;
    int errors;
    struct long_op_scope *lo_scope;
};

static bool loi_cs_flush(struct loi_cs_ctx *c)
{
    if (c->fill == 0) return true;
    int64_t w = coins_view_sqlite_bulk_insert(c->cvs, c->batch, c->fill);
    if (w != (int64_t)c->fill) {
        c->errors++;
        return false;
    }
    c->inserted += w;
    c->fill = 0;
    long_op_tick(c->lo_scope);
    return true;
}

static bool loi_cs_cb(const struct uint256 *txid,
                      const struct legacy_coins *lc,
                      void *vctx)
{
    struct loi_cs_ctx *c = vctx;
    if (thread_registry_shutdown_requested()) return false;
    c->records++;
    for (size_t i = 0; i < lc->num_vouts; i++) {
        if (c->fill >= c->cap) {
            if (!loi_cs_flush(c)) return false;
        }
        c->batch[c->fill++] = (struct utxo_bulk_rec){
            .txid = txid->data,
            .vout = lc->vouts[i].n,
            .value = lc->vouts[i].value,
            .script = lc->vouts[i].script,
            .script_len = (uint32_t)lc->vouts[i].script_len,
            .height = (uint32_t)lc->height,
            .is_coinbase = lc->coinbase ? 1u : 0u,
        };
    }
    return true;
}

/* ── atomic finalization on progress.kv ──────────────────────────────── */

static const char *const LOI_STAGES_TO_STAMP[] = {
    "header_admit",
    "validate_headers",
    "body_fetch",
    /* NB: extend when S-5..S-9 land — see header comment. */
    NULL,
};

static bool loi_stamp_stage_cursor_in_tx(sqlite3 *db,
                                         const char *name,
                                         uint64_t cursor)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO stage_cursor(name, cursor, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  cursor = excluded.cursor, updated_at = excluded.updated_at",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cursor);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
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
        if (!loi_stamp_stage_cursor_in_tx(db, LOI_STAGES_TO_STAMP[i],
                                          cursor_value)) {
            fprintf(stderr,  // obs-ok:legacy-oneshot-finalize-failure
                    "[legacy_attach] stamp stage '%s' failed\n",
                    LOI_STAGES_TO_STAMP[i]);
            ok = false;
        } else {
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
        int64_t now = (int64_t)time(NULL);
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
        fprintf(stderr,
            "[legacy_attach] progress.kv not open — boot order regression?\n");
        return false;
    }

    /* ── Pre-flight ──────────────────────────────────────────────────── */
    if (!loi_detect_legacy_datadir(legacy_datadir)) {
        fprintf(stderr,
            "[legacy_attach] %s does not look like a zclassic datadir; "
            "skipping.\n", legacy_datadir);
        r.outcome = LOI_OUTCOME_LEGACY_NOT_FOUND;
        if (out) *out = r;
        return true;  /* soft skip — not a failure */
    }

    int our_tip = active_chain_height(&ms->chain_active);
    bool sentinel_present = loi_meta_has_sentinel(pdb);
    if (our_tip > LOI_REFUSE_ABOVE_TIP && !sentinel_present) {
        fprintf(stderr,
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
            if (loi_make_stage_dir(our_datadir, stage_dir,
                                   sizeof(stage_dir))) {
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
        fprintf(stderr,  // obs-ok:legacy-oneshot-recovery
                "[legacy_attach] sentinel found from a prior aborted "
                "import — recovering: wipe + re-import\n");
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
    if (!loi_make_stage_dir(our_datadir, stage_dir, sizeof(stage_dir))) {
        fprintf(stderr,
            "[legacy_attach] cannot create stage dir under %s\n",
            our_datadir);
        return false;
    }
    if (!loi_snapshot_legacy(legacy_datadir, stage_dir,
                             idx_snap, sizeof(idx_snap),
                             cs_snap, sizeof(cs_snap))) {
        return false;
    }

    /* ── Hardlink blk*.dat. ──────────────────────────────────────────── */
    char legacy_blocks[1100], our_blocks[1100];
    snprintf(legacy_blocks, sizeof(legacy_blocks), "%s/blocks", legacy_datadir);
    snprintf(our_blocks, sizeof(our_blocks), "%s/blocks", our_datadir);
    int64_t t_link = loi_now_ms();
    int64_t linked = loi_link_blk_files(legacy_blocks, our_blocks);
    if (linked < 0) {
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    r.blk_files_linked = linked;
    fprintf(stderr,  // obs-ok:legacy-oneshot-progress
            "[legacy_attach] blk link/copy took %" PRId64 " ms\n",
            loi_now_ms() - t_link);

    /* ── Copy block_index. ───────────────────────────────────────────── */
    int64_t t_bi = loi_now_ms();
    struct uint256 legacy_tip_hash;
    int32_t legacy_tip_h = -1;
    int64_t bi_written = loi_copy_block_index(idx_snap, btdb,
                                              &legacy_tip_hash, &legacy_tip_h);
    if (bi_written < 0) {
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    r.block_index_writes = bi_written;
    r.legacy_tip_height = legacy_tip_h;
    fprintf(stderr,  // obs-ok:legacy-oneshot-progress
            "[legacy_attach] block_index copy: %" PRId64 " entries in "
            "%" PRId64 " ms (best h=%d)\n",
            bi_written, loi_now_ms() - t_bi, legacy_tip_h);

    /* ── Import chainstate UTXOs. ────────────────────────────────────── */
    void *cs = NULL;
    if (!chainstate_legacy_open(cs_snap, &cs)) {
        fprintf(stderr,
            "[legacy_attach] chainstate_legacy_open %s failed\n", cs_snap);
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    int64_t t_cs = loi_now_ms();
    struct long_op_scope cs_scope;
    long_op_begin(&cs_scope, "legacy_attach.cs_import");

    enum { BATCH = 50000 };  /* per plan: 50k rows/txn */
    struct utxo_bulk_rec *batch =
        zcl_malloc(sizeof(*batch) * BATCH, "loi.batch");
    if (!batch) {
        chainstate_legacy_close(cs);
        long_op_end(&cs_scope);
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    struct loi_cs_ctx ctx = {
        .batch = batch, .fill = 0, .cap = BATCH,
        .cvs = cvs, .lo_scope = &cs_scope,
    };
    int64_t n = chainstate_legacy_iter(cs, loi_cs_cb, &ctx);
    if (n >= 0 && ctx.fill > 0) loi_cs_flush(&ctx);

    struct uint256 cs_best;
    bool got_best = chainstate_legacy_get_best_block(cs, &cs_best);
    chainstate_legacy_close(cs);
    free(batch);
    long_op_end(&cs_scope);

    if (n < 0 || ctx.errors > 0) {
        fprintf(stderr,
            "[legacy_attach] chainstate import failed "
            "(iter=%" PRId64 " errors=%d)\n", n, ctx.errors);
        ldb_snapshot_destroy(idx_snap);
        ldb_snapshot_destroy(cs_snap);
        return false;
    }
    r.utxos_imported = ctx.inserted;
    fprintf(stderr,  // obs-ok:legacy-oneshot-progress
            "[legacy_attach] chainstate: %" PRId64 " UTXOs from "
            "%" PRId64 " records in %" PRId64 " ms\n",
            ctx.inserted, ctx.records, loi_now_ms() - t_cs);

    /* ── Persist pending CSR anchor so the existing boot resolver lifts
     *    the tip into active_chain later in boot. ────────────────────── */
    if (got_best) {
        bool pending_ok =
            node_db_state_set(ndb, "cold_import_pending_coins_best_block",
                              cs_best.data, 32) &&
            node_db_state_set(ndb, "cold_import_pending_coins_best_height",
                              &legacy_tip_h, sizeof(legacy_tip_h)) &&
            node_db_state_set(ndb, "cold_import_pending_utxo_count",
                              &ctx.inserted, sizeof(ctx.inserted));
        if (!pending_ok) {
            fprintf(stderr,
                "[legacy_attach] failed to persist pending CSR anchor\n");
            ldb_snapshot_destroy(idx_snap);
            ldb_snapshot_destroy(cs_snap);
            return false;
        }
    } else {
        fprintf(stderr,  // obs-ok:legacy-oneshot-warning
            "[legacy_attach] WARNING: legacy chainstate had no 'B' key; "
            "tip will not be activated this boot\n");
    }

    /* Use the chainstate 'B' hash as the canonical legacy tip hash for
     * the cursor-stamp record. Falls back to the block_index best hash
     * if 'B' wasn't present. */
    const struct uint256 *cursor_hash = got_best ? &cs_best : &legacy_tip_hash;

    /* ── Atomic finalization: cursors + completion record + sentinel ── */
    int64_t stages_stamped = 0;
    if (!loi_finalize_atomic(pdb, legacy_tip_h, cursor_hash, &stages_stamped)) {
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
