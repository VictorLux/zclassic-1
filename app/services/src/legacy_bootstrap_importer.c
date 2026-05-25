/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared legacy bootstrap import primitives. */

#include "services/legacy_bootstrap_importer.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "models/database.h"
#include "storage/block_index_db.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/dbwrapper.h"
#include "storage/ldb_snapshot.h"
#include "util/long_op.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int64_t legacy_bootstrap_link_blk_files(const char *legacy_blocks_dir,
                                        const char *our_blocks_dir,
                                        const char *log_prefix)
{
    if (!legacy_blocks_dir || !our_blocks_dir || !log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] link blk files: NULL argument\n");
        return -1;  // raw-return-ok:logged-above
    }

    DIR *d = opendir(legacy_blocks_dir);
    if (!d) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] cannot opendir %s: %s\n",
                log_prefix, legacy_blocks_dir, strerror(errno));
        return -1;  // raw-return-ok:logged-above
    }

    if (mkdir(our_blocks_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] cannot mkdir %s: %s\n",
                log_prefix, our_blocks_dir, strerror(errno));
        closedir(d);
        return -1;  // raw-return-ok:logged-above
    }

    int64_t linked = 0;
    int64_t copied = 0;
    int64_t skipped = 0;
    int64_t errors = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 12 || strncmp(de->d_name, "blk", 3) != 0 ||
            strcmp(de->d_name + nlen - 4, ".dat") != 0)
            continue;

        char src[1200], dst[1200];
        int ns = snprintf(src, sizeof(src), "%s/%s",
                          legacy_blocks_dir, de->d_name);
        int nd = snprintf(dst, sizeof(dst), "%s/%s",
                          our_blocks_dir, de->d_name);
        if (ns <= 0 || (size_t)ns >= sizeof(src) ||
            nd <= 0 || (size_t)nd >= sizeof(dst)) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] blk path too long for %s\n",
                    log_prefix, de->d_name);
            errors++;
            continue;
        }

        struct stat st;
        if (stat(dst, &st) == 0) {
            skipped++;
            continue;
        }
        if (link(src, dst) == 0) {
            linked++;
            continue;
        }
        if (errno != EXDEV && errno != EPERM) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] link(%s -> %s) failed: %s\n",
                    log_prefix, src, dst, strerror(errno));
            errors++;
            continue;
        }

        FILE *fsrc = fopen(src, "rb");
        FILE *fdst = fopen(dst, "wb");
        if (!fsrc || !fdst) {
            if (fsrc) fclose(fsrc);
            if (fdst) fclose(fdst);
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] open failed for copy %s -> %s\n",
                    log_prefix, src, dst);
            errors++;
            continue;
        }

        char buf[1u << 20];
        size_t n;
        bool copy_ok = true;
        while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
            if (fwrite(buf, 1, n, fdst) != n) {
                copy_ok = false;
                break;
            }
        }
        if (ferror(fsrc))
            copy_ok = false;
        if (fclose(fsrc) != 0)
            copy_ok = false;
        if (fclose(fdst) != 0)
            copy_ok = false;
        if (!copy_ok) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] copy failed for %s -> %s\n",
                    log_prefix, src, dst);
            errors++;
            continue;
        }
        copied++;
    }
    closedir(d);

    fprintf(stderr,  // obs-ok:bootstrap-import-terminal-summary
            "[%s] blk files: linked=%" PRId64 " copied=%" PRId64
            " skipped=%" PRId64 " errors=%" PRId64 "\n",
            log_prefix, linked, copied, skipped, errors);
    return (errors > 0) ? -1 : (linked + copied);
}

bool legacy_bootstrap_make_stage_dir(const char *datadir,
                                     const char *stage_subdir,
                                     char *out_stage_dir,
                                     size_t out_cap,
                                     const char *log_prefix)
{
    if (!datadir || !stage_subdir || !out_stage_dir || !log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] make stage dir: NULL argument\n");
        return false;
    }

    int n = snprintf(out_stage_dir, out_cap, "%s/%s",
                     datadir, stage_subdir);
    if (n <= 0 || (size_t)n >= out_cap) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] stage dir path too long\n", log_prefix);
        return false;
    }

    if (mkdir(out_stage_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] cannot mkdir %s: %s\n",
                log_prefix, out_stage_dir, strerror(errno));
        return false;
    }
    return true;
}

static bool legacy_bootstrap_snapshot_one_leveldb(const char *src,
                                                  const char *dst,
                                                  const char *label,
                                                  const char *log_prefix)
{
    char err[128];
    for (int tries = 0; tries < 3; tries++) {
        err[0] = '\0';
        if (ldb_snapshot_make(src, dst, err, sizeof(err)))
            return true;
        if (strcmp(err, "manifest_changed") != 0) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] snapshot of %s failed: %s\n",
                    log_prefix, src, err);
            return false;
        }
        fprintf(stderr,  // obs-ok:retryable-leveldb-snapshot-race
                "[%s] snapshot %s manifest_changed; retry %d\n",
                log_prefix, label, tries + 1);
    }

    fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
            "[%s] snapshot of %s failed after retries: %s\n",
            log_prefix, src, err);
    return false;
}

bool legacy_bootstrap_snapshot_leveldbs(const char *legacy_datadir,
                                        const char *stage_dir,
                                        char *out_idx_path,
                                        size_t idx_cap,
                                        char *out_cs_path,
                                        size_t cs_cap,
                                        const char *log_prefix)
{
    if (!legacy_datadir || !stage_dir || !out_idx_path || !out_cs_path ||
        !log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] snapshot LevelDBs: NULL argument\n");
        return false;
    }

    char src_idx[1100], src_cs[1100];
    int si = snprintf(src_idx, sizeof(src_idx), "%s/blocks/index",
                      legacy_datadir);
    int sc = snprintf(src_cs, sizeof(src_cs), "%s/chainstate",
                      legacy_datadir);
    int ni = snprintf(out_idx_path, idx_cap, "%s/blocks-index", stage_dir);
    int nc = snprintf(out_cs_path, cs_cap, "%s/chainstate", stage_dir);
    if (si <= 0 || (size_t)si >= sizeof(src_idx) ||
        sc <= 0 || (size_t)sc >= sizeof(src_cs) ||
        ni <= 0 || (size_t)ni >= idx_cap ||
        nc <= 0 || (size_t)nc >= cs_cap) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] snapshot path too long\n", log_prefix);
        return false;
    }

    if (!legacy_bootstrap_snapshot_one_leveldb(src_idx, out_idx_path,
                                               "blocks/index", log_prefix))
        return false;
    if (!legacy_bootstrap_snapshot_one_leveldb(src_cs, out_cs_path,
                                               "chainstate", log_prefix)) {
        ldb_snapshot_destroy(out_idx_path);
        return false;
    }
    return true;
}

int64_t legacy_bootstrap_copy_block_index(const char *legacy_index_dir,
                                          struct block_tree_db *our_btdb,
                                          struct uint256 *out_tip_hash,
                                          int32_t *out_tip_height,
                                          const char *long_op_name,
                                          const char *log_prefix)
{
    if (out_tip_height)
        *out_tip_height = -1;
    if (out_tip_hash)
        memset(out_tip_hash, 0, sizeof(*out_tip_hash));
    if (!legacy_index_dir || !our_btdb || !long_op_name || !log_prefix) {
        fprintf(stderr,
                "[legacy_bootstrap] copy block_index: NULL argument\n");
        return -1;  // raw-return-ok:logged-above
    }

    struct db_wrapper src;
    if (!db_wrapper_open(&src, legacy_index_dir, 16u << 20, false, false)) {
        fprintf(stderr,
                "[%s] cannot open block index %s\n",
                log_prefix, legacy_index_dir);
        return -1;  // raw-return-ok:logged-above
    }

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
    long_op_begin(&lo_scope, long_op_name);

    while (db_iter_valid(&it)) {
        if (thread_registry_shutdown_requested())
            break;

        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 1 || k[0] != 'b')
            break;
        if (klen != 33) {
            db_iter_next(&it);
            continue;
        }

        size_t vlen = 0;
        const char *v = db_iter_value(&it, &vlen);
        if (!v || vlen == 0) {
            db_iter_next(&it);
            continue;
        }

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        struct byte_stream s;
        stream_init_from_data(&s, (unsigned char *)v, vlen);
        bool ok = disk_block_index_deserialize(&dbi, &s);
        stream_free(&s);
        if (!ok) {
            db_iter_next(&it);
            continue;
        }

        bool have_data = (dbi.nStatus & BLOCK_HAVE_DATA) != 0;
        bool failed = (dbi.nStatus & BLOCK_FAILED_MASK) != 0;
        if (!have_data || failed) {
            db_iter_next(&it);
            continue;
        }

        if (dbi.nHeight > best_h) {
            best_h = dbi.nHeight;
            if (out_tip_hash)
                memcpy(out_tip_hash->data, k + 1, 32);
        }

        db_batch_put(&batch, k, klen, v, vlen);
        batch_fill++;
        written++;

        if (batch_fill >= BATCH_LIMIT) {
            if (!db_write_batch(&our_btdb->db, &batch, false)) {
                fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                        "[%s] db_write_batch failed\n",
                        log_prefix);
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
        if (!db_write_batch(&our_btdb->db, &batch, false)) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] final db_write_batch failed\n",
                    log_prefix);
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

    if (out_tip_height)
        *out_tip_height = best_h;
    return written;
}

bool legacy_bootstrap_load_height_map(
    const char *legacy_index_dir,
    const struct uint256 *tip_filter,
    const char *log_prefix,
    struct legacy_bootstrap_height_map_result *out)
{
    if (out)
        *out = (struct legacy_bootstrap_height_map_result){
            .map = NULL,
            .map_count = 0,
            .tip_height = -1,
        };
    if (!legacy_index_dir || !log_prefix || !out) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] load height map: bad args\n");
        return false;
    }

    struct bilr *bilr = NULL;
    if (!bilr_open(legacy_index_dir, &bilr)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] cannot open block index %s\n",
                log_prefix, legacy_index_dir);
        return false;
    }

    struct legacy_block_loc *map = NULL;
    size_t map_count = 0;
    bool ok = tip_filter
        ? bilr_load_height_map_for_tip(bilr, tip_filter, &map, &map_count)
        : bilr_load_height_map(bilr, &map, &map_count);
    bilr_close(bilr);
    if (!ok) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] bilr_load_height_map%s failed\n",
                log_prefix, tip_filter ? "_for_tip" : "");
        return false;
    }

    int legacy_tip = (int)map_count - 1;
    while (legacy_tip > 0 && map[(size_t)legacy_tip].height < 0)
        legacy_tip--;

    out->map = map;
    out->map_count = map_count;
    out->tip_height = legacy_tip;
    return true;
}

struct legacy_bootstrap_chainstate_ctx {
    struct utxo_bulk_rec *batch;
    uint8_t (*txids)[32];
    uint8_t **scripts;
    size_t fill;
    size_t cap;
    struct coins_view_sqlite *cvs;
    int64_t inserted;
    int64_t records;
    int errors;
    struct long_op_scope *lo_scope;
};

static void legacy_bootstrap_chainstate_clear_batch(
    struct legacy_bootstrap_chainstate_ctx *c)
{
    if (!c || !c->scripts) return;
    for (size_t i = 0; i < c->fill; i++) {
        free(c->scripts[i]);
        c->scripts[i] = NULL;
    }
    c->fill = 0;
}

static bool legacy_bootstrap_chainstate_flush(
    struct legacy_bootstrap_chainstate_ctx *c)
{
    if (c->fill == 0) return true;
    int64_t w = coins_view_sqlite_bulk_insert(c->cvs, c->batch, c->fill);
    if (w != (int64_t)c->fill) {
        c->errors++;
        legacy_bootstrap_chainstate_clear_batch(c);
        return false;
    }
    c->inserted += w;
    legacy_bootstrap_chainstate_clear_batch(c);
    if (c->lo_scope)
        long_op_tick(c->lo_scope);
    return true;
}

static bool legacy_bootstrap_chainstate_cb(const struct uint256 *txid,
                                           const struct legacy_coins *lc,
                                           void *vctx)
{
    struct legacy_bootstrap_chainstate_ctx *c = vctx;
    if (thread_registry_shutdown_requested()) return false;
    c->records++;
    for (size_t i = 0; i < lc->num_vouts; i++) {
        if (c->fill >= c->cap) {
            if (!legacy_bootstrap_chainstate_flush(c)) return false;
        }

        size_t script_len = lc->vouts[i].script_len;
        uint8_t *script_copy = zcl_malloc(script_len ? script_len : 1,
                                          "legacy_bootstrap.chainstate.script");
        if (!script_copy) {
            c->errors++;
            return false;
        }

        size_t slot = c->fill;
        memcpy(c->txids[slot], txid->data, 32);
        if (script_len)
            memcpy(script_copy, lc->vouts[i].script, script_len);
        c->batch[c->fill++] = (struct utxo_bulk_rec){
            .txid = c->txids[slot],
            .vout = lc->vouts[i].n,
            .value = lc->vouts[i].value,
            .script = script_copy,
            .script_len = (uint32_t)script_len,
            .height = (uint32_t)lc->height,
            .is_coinbase = lc->coinbase ? 1u : 0u,
        };
        c->scripts[slot] = script_copy;
    }
    return true;
}

bool legacy_bootstrap_import_chainstate_utxos(
    const char *chainstate_dir,
    struct coins_view_sqlite *cvs,
    size_t batch_limit,
    const char *long_op_name,
    const char *log_prefix,
    struct legacy_bootstrap_chainstate_import_result *out)
{
    if (out)
        *out = (struct legacy_bootstrap_chainstate_import_result){0};
    if (!chainstate_dir || !cvs || batch_limit == 0 || !log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] import chainstate: bad args\n");
        return false;
    }

    void *cs = NULL;
    if (!chainstate_legacy_open(chainstate_dir, &cs)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] chainstate_legacy_open %s failed\n",
                log_prefix, chainstate_dir);
        return false;
    }

    struct long_op_scope lo_scope;
    struct long_op_scope *lo_ptr = NULL;
    if (long_op_name) {
        long_op_begin(&lo_scope, long_op_name);
        lo_ptr = &lo_scope;
    }

    struct utxo_bulk_rec *batch =
        zcl_malloc(sizeof(*batch) * batch_limit,
                   "legacy_bootstrap.chainstate.batch");
    uint8_t (*txids)[32] =
        zcl_malloc(sizeof(*txids) * batch_limit,
                   "legacy_bootstrap.chainstate.txids");
    uint8_t **scripts =
        zcl_malloc(sizeof(*scripts) * batch_limit,
                   "legacy_bootstrap.chainstate.scripts");
    if (!batch || !txids || !scripts) {
        free(batch);
        free(txids);
        free(scripts);
        if (lo_ptr)
            long_op_end(lo_ptr);
        chainstate_legacy_close(cs);
        return false;
    }
    memset(scripts, 0, sizeof(*scripts) * batch_limit);

    struct legacy_bootstrap_chainstate_ctx ctx = {
        .batch = batch,
        .txids = txids,
        .scripts = scripts,
        .fill = 0,
        .cap = batch_limit,
        .cvs = cvs,
        .lo_scope = lo_ptr,
    };
    int64_t n = chainstate_legacy_iter(cs, legacy_bootstrap_chainstate_cb,
                                       &ctx);
    if (n >= 0 && ctx.fill > 0)
        legacy_bootstrap_chainstate_flush(&ctx);

    struct uint256 best_block;
    bool got_best = chainstate_legacy_get_best_block(cs, &best_block);
    chainstate_legacy_close(cs);
    legacy_bootstrap_chainstate_clear_batch(&ctx);
    free(batch);
    free(txids);
    free(scripts);
    if (lo_ptr)
        long_op_end(lo_ptr);

    if (n < 0 || ctx.errors > 0) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] chainstate import failed "
                "(iter=%" PRId64 " errors=%d)\n",
                log_prefix, n, ctx.errors);
        return false;
    }

    if (out) {
        out->inserted = ctx.inserted;
        out->records = ctx.records;
        out->got_best_block = got_best;
        if (got_best)
            out->best_block = best_block;
    }
    return true;
}

bool legacy_bootstrap_record_pending_csr_anchor(
    struct node_db *ndb,
    const struct uint256 *best_block,
    int32_t best_height,
    int64_t utxo_count,
    const char *log_prefix)
{
    if (!ndb || !best_block || !log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] record CSR anchor: bad args\n");
        return false;
    }

    bool pending_ok =
        node_db_state_set(ndb, "cold_import_pending_coins_best_block",
                          best_block->data, 32) &&
        node_db_state_set(ndb, "cold_import_pending_coins_best_height",
                          &best_height, sizeof(best_height)) &&
        node_db_state_set(ndb, "cold_import_pending_utxo_count",
                          &utxo_count, sizeof(utxo_count));
    if (!pending_ok) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] failed to persist pending CSR anchor\n", log_prefix);
        return false;
    }

    char hex[65] = {0};
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", best_block->data[31 - i]);
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[%s] pending CSR anchor recorded %s h=%d\n",
            log_prefix, hex, best_height);
    return true;
}
