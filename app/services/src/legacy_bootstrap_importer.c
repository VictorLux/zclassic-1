/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared legacy bootstrap import primitives. */

#include "services/legacy_bootstrap_importer.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "consensus/validation.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "models/database.h"
#include "platform/time_compat.h"
#include "services/legacy_cold_import.h"
#include "services/legacy_direct_import.h"
#include "storage/block_index_db.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/dbwrapper.h"
#include "storage/ldb_snapshot.h"
#include "util/long_op.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "wallet/wallet.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEGACY_BOOTSTRAP_COLD_REFUSE_ABOVE_TIP 1000
#define LEGACY_BOOTSTRAP_COLD_SPOTCHECK_K 5
#define LEGACY_BOOTSTRAP_COLD_STAGE_SUBDIR "cold_import_ldb_snapshot"
#define LEGACY_BOOTSTRAP_DIRECT_SPOTCHECK_K 3

static int64_t legacy_bootstrap_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

bool legacy_bootstrap_import_snapshot_state(
    const struct legacy_bootstrap_snapshot_import_options *opts,
    struct legacy_bootstrap_snapshot_import_result *out)
{
    if (out)
        *out = (struct legacy_bootstrap_snapshot_import_result){
            .legacy_tip_height = -1,
        };
    if (!opts || !opts->legacy_blocks_dir || !opts->our_blocks_dir ||
        !opts->legacy_index_dir || !opts->chainstate_dir || !opts->btdb ||
        !opts->cvs || opts->chainstate_batch_limit == 0 ||
        !opts->block_index_long_op_name || !opts->log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] import snapshot state: bad args\n");
        return false;
    }

    struct legacy_bootstrap_snapshot_import_result r = {
        .legacy_tip_height = -1,
    };

    int64_t linked = legacy_bootstrap_link_blk_files(opts->legacy_blocks_dir,
                                                     opts->our_blocks_dir,
                                                     opts->log_prefix);
    if (linked < 0)
        return false;
    r.blk_files_linked = linked;

    int64_t bi_written = legacy_bootstrap_copy_block_index(
        opts->legacy_index_dir, opts->btdb, &r.legacy_tip_hash,
        &r.legacy_tip_height, opts->block_index_long_op_name,
        opts->log_prefix);
    if (bi_written < 0)
        return false;
    r.block_index_writes = bi_written;

    if (opts->min_legacy_tip >= 0 &&
        r.legacy_tip_height < opts->min_legacy_tip) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] REFUSING: discovered legacy tip h=%d is below "
                "minimum %d; no chainstate import or cursor publication\n",
                opts->log_prefix, r.legacy_tip_height,
                opts->min_legacy_tip);
        return false;
    }

    struct legacy_bootstrap_chainstate_import_result cs_import;
    if (!legacy_bootstrap_import_chainstate_utxos(
            opts->chainstate_dir, opts->cvs, opts->chainstate_batch_limit,
            opts->chainstate_long_op_name, opts->log_prefix, &cs_import))
        return false;

    r.utxos_imported = cs_import.inserted;
    r.chainstate_records = cs_import.records;
    r.got_best_block = cs_import.got_best_block;
    if (cs_import.got_best_block)
        r.best_block = cs_import.best_block;

    if (!cs_import.got_best_block) {
        if (opts->require_best_block) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] REFUSING: legacy chainstate had no 'B' key; "
                    "cannot publish an activatable tip\n",
                    opts->log_prefix);
            return false;
        }
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] WARNING: legacy chainstate had no 'B' key; "
                "pending CSR anchor not recorded\n",
                opts->log_prefix);
    } else if (opts->ndb) {
        if (!legacy_bootstrap_record_pending_csr_anchor(
                opts->ndb, &cs_import.best_block, r.legacy_tip_height,
                cs_import.inserted, opts->log_prefix))
            return false;
    }

    if (out)
        *out = r;
    return true;
}

bool legacy_bootstrap_open_block_source(
    const struct legacy_bootstrap_block_source_options *opts,
    struct legacy_bootstrap_block_source *out)
{
    if (out)
        *out = (struct legacy_bootstrap_block_source){0};
    if (!opts || !opts->legacy_blocks_dir || !opts->map ||
        opts->map_count == 0 || opts->legacy_tip < 0 ||
        opts->spotcheck_k <= 0 || !opts->log_prefix || !out) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] open block source: bad args\n");
        return false;
    }

    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(opts->legacy_blocks_dir, &bmr)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] cannot open legacy blocks dir %s\n",
                opts->log_prefix, opts->legacy_blocks_dir);
        return false;
    }

    bool checked = legacy_bootstrap_spotcheck_sha3_windows(
        bmr, opts->map, opts->map_count, opts->legacy_tip,
        opts->spotcheck_k, opts->log_prefix, opts->debug_env,
        opts->dump_map_on_failure);
    if (!checked) {
        if (opts->require_spotcheck) {
            bmr_close(bmr);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[%s] refusing to import: aborting due to spotcheck "
                    "failure\n",
                    opts->log_prefix);
            return false;
        }
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] WARNING: SHA3 spotcheck did not pass; continuing "
                "with full validation\n",
                opts->log_prefix);
    } else if (opts->require_spotcheck) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] SHA3 source spotcheck passed\n",
                opts->log_prefix);
    } else {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] SHA3 source spotcheck passed; proof validation "
                "remains enabled\n",
                opts->log_prefix);
    }

    out->bmr = bmr;
    out->source_checked = checked;
    return true;
}

void legacy_bootstrap_close_block_source(
    struct legacy_bootstrap_block_source *src)
{
    if (!src || !src->bmr)
        return;
    bmr_close(src->bmr);
    src->bmr = NULL;
    src->source_checked = false;
}

static bool legacy_bootstrap_import_cold(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out)
{
    struct legacy_bootstrap_import_result r = {
        .legacy_tip = -1,
    };
    if (out) *out = r;

    if (!opts || !opts->ms || !opts->cvs || !opts->ndb ||
        !opts->ndb->open || !opts->btdb || !opts->our_datadir ||
        !opts->legacy_datadir) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[cold_import] bad args\n");
        return false;
    }

    int our_tip = active_chain_height(&opts->ms->chain_active);
    if (our_tip > LEGACY_BOOTSTRAP_COLD_REFUSE_ABOVE_TIP) {
        fprintf(stderr,
                "[cold_import] REFUSING: our active_tip=%d > %d. "
                "Cold-import is for empty datadirs; use -fastimport for "
                "warm catch-up.\n",
                our_tip, LEGACY_BOOTSTRAP_COLD_REFUSE_ABOVE_TIP);
        return false;
    }

    char blk_dir[1024];
    int nb = snprintf(blk_dir, sizeof(blk_dir), "%s/blocks",
                      opts->legacy_datadir);
    char our_blocks[1024];
    int no = snprintf(our_blocks, sizeof(our_blocks), "%s/blocks",
                      opts->our_datadir);
    if (nb <= 0 || (size_t)nb >= sizeof(blk_dir) ||
        no <= 0 || (size_t)no >= sizeof(our_blocks)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[cold_import] block directory path too long\n");
        return false;
    }

    int64_t t_start = legacy_bootstrap_now_ms();

    char stage_dir[1100], idx_dir[1200], cs_dir[1200];
    if (!legacy_bootstrap_make_stage_dir(
            opts->our_datadir, LEGACY_BOOTSTRAP_COLD_STAGE_SUBDIR,
            stage_dir, sizeof(stage_dir), "cold_import")) {
        fprintf(stderr,
                "[cold_import] cannot create stage dir under %s\n",
                opts->our_datadir);
        return false;
    }
    int64_t t_snap = legacy_bootstrap_now_ms();
    if (!legacy_bootstrap_snapshot_leveldbs(opts->legacy_datadir, stage_dir,
                                            idx_dir, sizeof(idx_dir),
                                            cs_dir, sizeof(cs_dir),
                                            "cold_import"))
        return false;
    fprintf(stderr,  // obs-ok:cold-import-progress
            "[cold_import] LevelDB snapshots took %" PRId64 " ms\n",
            legacy_bootstrap_now_ms() - t_snap);

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
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[cold_import] legacy tip h=%d (map size=%zu)\n",
            legacy_tip, map_count);

    struct legacy_bootstrap_block_source source;
    const struct legacy_bootstrap_block_source_options source_opts = {
        .legacy_blocks_dir = blk_dir,
        .map = map,
        .map_count = map_count,
        .legacy_tip = legacy_tip,
        .spotcheck_k = LEGACY_BOOTSTRAP_COLD_SPOTCHECK_K,
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

    struct legacy_bootstrap_snapshot_import_result imported;
    const struct legacy_bootstrap_snapshot_import_options import_opts = {
        .legacy_blocks_dir = blk_dir,
        .our_blocks_dir = our_blocks,
        .legacy_index_dir = idx_dir,
        .chainstate_dir = cs_dir,
        .btdb = opts->btdb,
        .cvs = opts->cvs,
        .ndb = opts->ndb,
        .chainstate_batch_limit = 5000,
        .min_legacy_tip = -1,
        .require_best_block = false,
        .block_index_long_op_name = "legacy_cold_import.bulk_copy",
        .chainstate_long_op_name = NULL,
        .log_prefix = "cold_import",
    };
    int64_t t_import = legacy_bootstrap_now_ms();
    bool import_ok =
        legacy_bootstrap_import_snapshot_state(&import_opts, &imported);
    bilr_free_height_map(map);
    if (!import_ok) {
        ldb_snapshot_destroy(idx_dir);
        ldb_snapshot_destroy(cs_dir);
        return false;
    }
    r.blk_files_linked = imported.blk_files_linked;
    r.block_index_writes = imported.block_index_writes;
    r.utxos_imported = imported.utxos_imported;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[cold_import] snapshot state import took %" PRId64 " ms "
            "(best h=%d records=%" PRId64 ")\n",
            legacy_bootstrap_now_ms() - t_import,
            imported.legacy_tip_height, imported.chainstate_records);

    r.total_secs = (double)(legacy_bootstrap_now_ms() - t_start) / 1000.0;
    r.ok = true;
    if (out) *out = r;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[cold_import] DONE in %.1fs: block_index=%" PRId64
            " utxos=%" PRId64 " blk_files=%" PRId64 "\n",
            r.total_secs, r.block_index_writes, r.utxos_imported,
            r.blk_files_linked);
    ldb_snapshot_destroy(idx_dir);
    ldb_snapshot_destroy(cs_dir);
    return true;
}

static bool legacy_bootstrap_import_direct(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out)
{
    struct legacy_bootstrap_import_result r = {
        .legacy_tip = -1,
        .final_tip = -1,
    };
    if (out) *out = r;

    if (!opts || !opts->ms || !opts->coins_tip || !opts->params ||
        !opts->our_datadir || !opts->legacy_datadir) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_direct_import] bad args\n");
        return false;
    }

    char idx_dir[1024];
    int ni = snprintf(idx_dir, sizeof(idx_dir), "%s/blocks/index",
                      opts->legacy_datadir);
    char blk_dir[1024];
    int nb = snprintf(blk_dir, sizeof(blk_dir), "%s/blocks",
                      opts->legacy_datadir);
    if (ni <= 0 || (size_t)ni >= sizeof(idx_dir) ||
        nb <= 0 || (size_t)nb >= sizeof(blk_dir)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_direct_import] legacy path too long\n");
        return false;
    }

    int64_t t_open = legacy_bootstrap_now_ms();
    struct legacy_bootstrap_height_map_result hmap;
    if (!legacy_bootstrap_load_height_map(idx_dir, NULL,
                                          "legacy_direct_import", &hmap))
        return false;

    struct legacy_block_loc *map = hmap.map;
    size_t map_count = hmap.map_count;
    int legacy_tip = hmap.tip_height;
    r.legacy_tip = legacy_tip;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] legacy tip h=%d (map_count=%zu, "
            "load took %" PRId64 " ms)\n",
            legacy_tip, map_count, legacy_bootstrap_now_ms() - t_open);

    int from_height = opts->from_height;
    if (from_height < 0)
        from_height = active_chain_height(&opts->ms->chain_active);
    if (from_height < 0)
        from_height = 0;
    if (from_height >= legacy_tip) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] already at/past legacy tip "
                "(from=%d legacy=%d) — nothing to do\n",
                from_height, legacy_tip);
        bilr_free_height_map(map);
        r.ok = true;
        r.final_tip = active_chain_height(&opts->ms->chain_active);
        if (out) *out = r;
        return true;
    }

    struct legacy_bootstrap_block_source source;
    const struct legacy_bootstrap_block_source_options source_opts = {
        .legacy_blocks_dir = blk_dir,
        .map = map,
        .map_count = map_count,
        .legacy_tip = legacy_tip,
        .spotcheck_k = LEGACY_BOOTSTRAP_DIRECT_SPOTCHECK_K,
        .require_spotcheck = false,
        .log_prefix = "legacy_direct_import",
        .debug_env = NULL,
        .dump_map_on_failure = false,
    };
    if (!legacy_bootstrap_open_block_source(&source_opts, &source)) {
        bilr_free_height_map(map);
        if (out) *out = r;
        return false;
    }
    struct blocks_mmap *bmr = source.bmr;
    r.source_checked = source.source_checked;

    atomic_store(&g_body_pull_active, 1);

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] starting walk: [%d+1 .. %d] "
            "(%d blocks)\n",
            from_height, legacy_tip, legacy_tip - from_height);

    int64_t t_walk = legacy_bootstrap_now_ms();
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

        zcl_mutex_lock(&opts->ms->cs_main);
        struct block_index *bi =
            block_map_find(&opts->ms->map_block_index, &loc->hash);
        bool have_data = bi && (bi->nStatus & BLOCK_HAVE_DATA);
        bool failed = bi && (bi->nStatus & BLOCK_FAILED_MASK);
        zcl_mutex_unlock(&opts->ms->cs_main);
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
        bool pn_ok = process_new_block(&vs, opts->ms, opts->coins_tip,
                                       opts->params, &block, true,
                                       opts->our_datadir);
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

        int64_t now = legacy_bootstrap_now_ms();
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

    int64_t t_walk_end = legacy_bootstrap_now_ms();
    double total_secs = (double)(t_walk_end - t_walk) / 1000.0;
    double avg_rate = total_secs > 0.0
        ? (double)r.applied / total_secs : 0.0;

    atomic_store(&g_body_pull_active, 0);
    legacy_bootstrap_close_block_source(&source);
    bilr_free_height_map(map);

    r.final_tip = active_chain_height(&opts->ms->chain_active);
    r.ok = ok;

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] walk %s: applied=%d "
            "skipped_have=%d skipped_failed=%d elapsed=%.1fs "
            "rate=%.1f bps final_tip=%d\n",
            ok ? "complete" : "ABORTED",
            r.applied, r.skipped_have_data, r.skipped_failed,
            total_secs, avg_rate, r.final_tip);

    if (ok && opts->wallet && r.applied > 0) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] starting wallet rescan "
                "[%d..%d]...\n", from_height + 1, r.final_tip);
        int64_t t_rescan = legacy_bootstrap_now_ms();
        int hits = wallet_rescan(opts->wallet, &opts->ms->chain_active,
                                 from_height + 1, r.final_tip,
                                 opts->our_datadir);
        double secs = (double)(legacy_bootstrap_now_ms() - t_rescan) /
                      1000.0;
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] wallet rescan complete: "
                "%d hits in %.1fs\n", hits, secs);
    }

    if (out) *out = r;
    return ok;
}

bool legacy_bootstrap_import_blocking(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out)
{
    if (out)
        *out = (struct legacy_bootstrap_import_result){.legacy_tip = -1};
    if (!opts) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] import: NULL options\n");
        return false;
    }

    switch (opts->mode) {
        case LEGACY_BOOTSTRAP_IMPORT_COLD:
            return legacy_bootstrap_import_cold(opts, out);
        case LEGACY_BOOTSTRAP_IMPORT_DIRECT:
            return legacy_bootstrap_import_direct(opts, out);
    }

    fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
            "[legacy_bootstrap] import: unknown mode %d\n", (int)opts->mode);
    return false;
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
    struct legacy_bootstrap_import_result imported;
    const struct legacy_bootstrap_import_options opts = {
        .mode = LEGACY_BOOTSTRAP_IMPORT_COLD,
        .ms = ms,
        .cvs = cvs,
        .ndb = ndb,
        .btdb = btdb,
        .our_datadir = our_datadir,
        .legacy_datadir = legacy_datadir,
    };

    bool ok = legacy_bootstrap_import_blocking(&opts, &imported);
    if (out) {
        *out = (struct lci_cold_result){
            .legacy_tip = imported.legacy_tip,
            .block_index_writes = imported.block_index_writes,
            .utxos_imported = imported.utxos_imported,
            .blk_files_linked = imported.blk_files_linked,
            .total_secs = imported.total_secs,
            .evidence_armed = imported.evidence_armed,
            .ok = imported.ok,
        };
    }
    return ok;
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
    struct legacy_bootstrap_import_result imported;
    const struct legacy_bootstrap_import_options opts = {
        .mode = LEGACY_BOOTSTRAP_IMPORT_DIRECT,
        .ms = ms,
        .coins_tip = coins_tip,
        .params = params,
        .wallet = wallet,
        .our_datadir = our_datadir,
        .legacy_datadir = legacy_datadir,
        .from_height = from_height,
    };

    bool ok = legacy_bootstrap_import_blocking(&opts, &imported);
    if (out) {
        *out = (struct ldi_result){
            .applied = imported.applied,
            .skipped_have_data = imported.skipped_have_data,
            .skipped_failed = imported.skipped_failed,
            .final_tip = imported.final_tip,
            .legacy_tip = imported.legacy_tip,
            .source_checked = imported.source_checked,
            .ok = imported.ok,
        };
    }
    return ok;
}
