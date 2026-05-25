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
#include "storage/block_index_db.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
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
#include "validation/process_block.h"
#include "wallet/wallet.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEGACY_BOOTSTRAP_COLD_REFUSE_ABOVE_TIP 1000
#define LEGACY_BOOTSTRAP_COLD_STAGE_SUBDIR "cold_import_ldb_snapshot"
#define LEGACY_BOOTSTRAP_ATTACH_REFUSE_ABOVE_TIP 1000
#define LEGACY_BOOTSTRAP_ATTACH_MIN_TIP 100
#define LEGACY_BOOTSTRAP_ATTACH_STAGE_SUBDIR "legacy-attach-stage"
#define LEGACY_BOOTSTRAP_ATTACH_META_SENTINEL "import_in_progress"
#define LEGACY_BOOTSTRAP_ATTACH_META_TIP_HASH "legacy_attach_tip_hash"
#define LEGACY_BOOTSTRAP_ATTACH_META_TIP_HEIGHT "legacy_attach_tip_height"

enum legacy_bootstrap_snapshot_mode {
    LEGACY_BOOTSTRAP_SNAPSHOT_COLD = 0,
    LEGACY_BOOTSTRAP_SNAPSHOT_ATTACH = 1,
};

enum legacy_bootstrap_block_source_mode {
    LEGACY_BOOTSTRAP_BLOCK_SOURCE_COLD = 0,
    LEGACY_BOOTSTRAP_BLOCK_SOURCE_DIRECT = 1,
};

struct legacy_bootstrap_block_source_config {
    int spotcheck_k;
    bool require_spotcheck;
    const char *log_prefix;
    const char *debug_env;
    bool dump_map_on_failure;
};

static const struct legacy_bootstrap_block_source_config
    g_block_source_cfg[] = {
        [LEGACY_BOOTSTRAP_BLOCK_SOURCE_COLD] = {
            .spotcheck_k = 5,
            .require_spotcheck = true,
            .log_prefix = "cold_import",
            .debug_env = "ZCL_COLD_IMPORT_DEBUG_WINDOW",
            .dump_map_on_failure = true,
        },
        [LEGACY_BOOTSTRAP_BLOCK_SOURCE_DIRECT] = {
            .spotcheck_k = 3,
            .require_spotcheck = false,
            .log_prefix = "legacy_direct_import",
            .debug_env = NULL,
            .dump_map_on_failure = false,
        },
    };

struct legacy_bootstrap_snapshot_mode_config {
    size_t chainstate_batch_limit;
    int32_t min_legacy_tip;
    bool require_best_block;
    const char *block_index_long_op_name;
    const char *chainstate_long_op_name;
    const char *log_prefix;
};

static const struct legacy_bootstrap_snapshot_mode_config
    g_snapshot_mode_cfg[] = {
        [LEGACY_BOOTSTRAP_SNAPSHOT_COLD] = {
            .chainstate_batch_limit = 5000,
            .min_legacy_tip = -1,
            .require_best_block = false,
            .block_index_long_op_name = "legacy_cold_import.bulk_copy",
            .chainstate_long_op_name = NULL,
            .log_prefix = "cold_import",
        },
        [LEGACY_BOOTSTRAP_SNAPSHOT_ATTACH] = {
            .chainstate_batch_limit = 50000,
            .min_legacy_tip = LEGACY_BOOTSTRAP_ATTACH_MIN_TIP,
            .require_best_block = true,
            .block_index_long_op_name = "legacy_attach.bi_copy",
            .chainstate_long_op_name = "legacy_attach.cs_import",
            .log_prefix = "legacy_attach",
        },
    };

static const struct legacy_bootstrap_snapshot_mode_config *
legacy_bootstrap_snapshot_mode_cfg(enum legacy_bootstrap_snapshot_mode mode)
{
    return ((size_t)mode <
            sizeof(g_snapshot_mode_cfg) / sizeof(g_snapshot_mode_cfg[0]))
        ? &g_snapshot_mode_cfg[mode]
        : NULL;
}

struct legacy_bootstrap_snapshot_import_options {
    enum legacy_bootstrap_snapshot_mode mode;
    const char *legacy_blocks_dir;
    const char *our_blocks_dir;
    const char *legacy_index_dir;
    const char *chainstate_dir;
    struct block_tree_db *btdb;
    struct coins_view_sqlite *cvs;
    struct node_db *ndb;
    int32_t anchor_height;
};

struct legacy_bootstrap_snapshot_import_result {
    int64_t blk_files_linked;
    int64_t block_index_writes;
    int64_t utxos_imported;
    struct uint256 best_block;
    int32_t legacy_tip_height;
};

struct legacy_bootstrap_chainstate_import_result {
    int64_t inserted;
    bool got_best_block;
    struct uint256 best_block;
};

struct legacy_bootstrap_staged_snapshot_paths {
    char stage_dir[1100];
    char idx_dir[1200];
    char cs_dir[1200];
    char legacy_blocks_dir[1100];
    char our_blocks_dir[1100];
};

bool legacy_bootstrap_spotcheck_sha3_windows(
    struct blocks_mmap *bmr,
    const struct legacy_block_loc *map,
    size_t map_count,
    int legacy_tip,
    int k,
    const char *log_prefix,
    const char *debug_env,
    bool dump_map_on_failure);

static bool legacy_bootstrap_read_chainstate_best_block(
    const char *chainstate_dir,
    const char *log_prefix,
    struct uint256 *out_hash);

static int64_t legacy_bootstrap_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool legacy_bootstrap_format_child_path(const char *base,
                                               const char *child,
                                               char *out,
                                               size_t out_sz,
                                               const char *log_prefix,
                                               const char *what)
{
    if (!base || !child || !out || out_sz == 0 || !log_prefix || !what) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] format path: bad args\n");
        return false;
    }

    int n = snprintf(out, out_sz, "%s/%s", base, child);
    if (n <= 0 || (size_t)n >= out_sz) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] %s path too long\n", log_prefix, what);
        return false;
    }
    return true;
}

static int64_t legacy_bootstrap_link_blk_files(const char *legacy_blocks_dir,
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

static bool legacy_bootstrap_detect_datadir(const char *legacy_datadir,
                                            bool require_leveldbs)
{
    if (!legacy_datadir || !legacy_datadir[0]) return false;

    static const char *const suffixes[] = {
        "blocks/blk00000.dat",
        "blocks/index/CURRENT",
        "chainstate/CURRENT",
    };
    char p[1100];
    size_t n = require_leveldbs ? 3u : 1u;
    for (size_t i = 0; i < n; i++) {
        int written = snprintf(p, sizeof(p), "%s/%s",
                               legacy_datadir, suffixes[i]);
        if (written <= 0 || (size_t)written >= sizeof(p))
            return false;
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
            return false;
    }
    return true;
}

static bool legacy_bootstrap_make_stage_dir(const char *datadir,
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

    if (!legacy_bootstrap_format_child_path(datadir, stage_subdir,
                                            out_stage_dir, out_cap,
                                            log_prefix, "stage directory"))
        return false;

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

static bool legacy_bootstrap_snapshot_leveldbs(const char *legacy_datadir,
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
    if (!legacy_bootstrap_format_child_path(
            legacy_datadir, "blocks/index", src_idx, sizeof(src_idx),
            log_prefix, "legacy block-index directory") ||
        !legacy_bootstrap_format_child_path(
            legacy_datadir, "chainstate", src_cs, sizeof(src_cs),
            log_prefix, "legacy chainstate directory") ||
        !legacy_bootstrap_format_child_path(
            stage_dir, "blocks-index", out_idx_path, idx_cap,
            log_prefix, "staged block-index directory") ||
        !legacy_bootstrap_format_child_path(
            stage_dir, "chainstate", out_cs_path, cs_cap,
            log_prefix, "staged chainstate directory"))
        return false;

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

static void legacy_bootstrap_cleanup_staged_snapshot(
    const struct legacy_bootstrap_staged_snapshot_paths *paths,
    bool remove_stage_dir)
{
    if (!paths) return;
    if (paths->idx_dir[0])
        ldb_snapshot_destroy(paths->idx_dir);
    if (paths->cs_dir[0])
        ldb_snapshot_destroy(paths->cs_dir);
    if (remove_stage_dir && paths->stage_dir[0])
        rmdir(paths->stage_dir);
}

static bool legacy_bootstrap_prepare_staged_snapshot(
    const struct legacy_bootstrap_import_options *opts,
    const char *stage_subdir,
    const char *log_prefix,
    struct legacy_bootstrap_staged_snapshot_paths *paths)
{
    if (paths)
        memset(paths, 0, sizeof(*paths));
    if (!opts || !opts->our_datadir || !opts->legacy_datadir ||
        !stage_subdir || !log_prefix || !paths) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] prepare staged snapshot: bad args\n");
        return false;
    }

    if (!legacy_bootstrap_make_stage_dir(
            opts->our_datadir, stage_subdir, paths->stage_dir,
            sizeof(paths->stage_dir), log_prefix))
        return false;

    if (!legacy_bootstrap_snapshot_leveldbs(
            opts->legacy_datadir, paths->stage_dir, paths->idx_dir,
            sizeof(paths->idx_dir), paths->cs_dir, sizeof(paths->cs_dir),
            log_prefix))
        return false;

    if (!legacy_bootstrap_format_child_path(
            opts->legacy_datadir, "blocks", paths->legacy_blocks_dir,
            sizeof(paths->legacy_blocks_dir), log_prefix,
            "legacy blocks directory") ||
        !legacy_bootstrap_format_child_path(
            opts->our_datadir, "blocks", paths->our_blocks_dir,
            sizeof(paths->our_blocks_dir), log_prefix,
            "local blocks directory")) {
        legacy_bootstrap_cleanup_staged_snapshot(paths, false);
        return false;
    }
    return true;
}

static bool legacy_bootstrap_probe_chainstate_best(
    const struct legacy_bootstrap_import_options *opts,
    const char *stage_subdir,
    const char *snapshot_name,
    const char *log_prefix,
    struct uint256 *out_best)
{
    if (out_best)
        memset(out_best, 0, sizeof(*out_best));
    if (!opts || !opts->our_datadir || !opts->legacy_datadir ||
        !stage_subdir || !snapshot_name || !log_prefix || !out_best) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] probe chainstate best: bad args\n");
        return false;
    }

    char stage_dir[1100];
    if (!legacy_bootstrap_make_stage_dir(
            opts->our_datadir, stage_subdir, stage_dir, sizeof(stage_dir),
            log_prefix))
        return false;

    char src_cs[1100], cs_path[1200];
    if (!legacy_bootstrap_format_child_path(
            opts->legacy_datadir, "chainstate", src_cs, sizeof(src_cs),
            log_prefix, "legacy chainstate directory") ||
        !legacy_bootstrap_format_child_path(
            stage_dir, snapshot_name, cs_path, sizeof(cs_path),
            log_prefix, "probe chainstate directory"))
        return false;

    if (!legacy_bootstrap_snapshot_one_leveldb(
            src_cs, cs_path, "chainstate", log_prefix))
        return false;

    bool ok = legacy_bootstrap_read_chainstate_best_block(
        cs_path, log_prefix, out_best);
    ldb_snapshot_destroy(cs_path);
    return ok;
}

static int64_t legacy_bootstrap_copy_block_index(
    const char *legacy_index_dir,
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

static bool legacy_bootstrap_resolve_tip_height(
    const char *legacy_index_dir,
    const struct uint256 *tip_hash,
    const char *log_prefix,
    int32_t *out_height)
{
    if (out_height)
        *out_height = -1;
    if (!legacy_index_dir || !tip_hash || !log_prefix || !out_height) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] resolve tip height: bad args\n");
        return false;
    }

    struct legacy_bootstrap_height_map_result hmap;
    if (!legacy_bootstrap_load_height_map(legacy_index_dir, tip_hash,
                                          log_prefix, &hmap))
        return false;
    *out_height = hmap.tip_height;
    bilr_free_height_map(hmap.map);
    return *out_height >= 0;
}

struct legacy_bootstrap_chainstate_ctx {
    struct utxo_bulk_rec *batch;
    uint8_t (*txids)[32];
    uint8_t **scripts;
    size_t fill;
    size_t cap;
    struct coins_view_sqlite *cvs;
    int64_t inserted;
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

static bool legacy_bootstrap_import_chainstate_utxos(
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
        out->got_best_block = got_best;
        if (got_best)
            out->best_block = best_block;
    }
    return true;
}

static bool legacy_bootstrap_read_chainstate_best_block(
    const char *chainstate_dir,
    const char *log_prefix,
    struct uint256 *out_best)
{
    if (!chainstate_dir || !log_prefix || !out_best) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] read chainstate best: bad args\n");
        return false;
    }

    void *cs = NULL;
    if (!chainstate_legacy_open(chainstate_dir, &cs)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] chainstate_legacy_open %s failed\n",
                log_prefix, chainstate_dir);
        return false;
    }

    bool ok = chainstate_legacy_get_best_block(cs, out_best);
    chainstate_legacy_close(cs);
    if (!ok)
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] chainstate best block unavailable\n", log_prefix);
    return ok;
}

static bool legacy_bootstrap_record_pending_csr_anchor(
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

static bool legacy_bootstrap_import_snapshot_state(
    const struct legacy_bootstrap_snapshot_import_options *opts,
    struct legacy_bootstrap_snapshot_import_result *out)
{
    if (out)
        *out = (struct legacy_bootstrap_snapshot_import_result){
            .legacy_tip_height = -1,
        };
    const struct legacy_bootstrap_snapshot_mode_config *cfg =
        opts ? legacy_bootstrap_snapshot_mode_cfg(opts->mode) : NULL;
    if (!opts || !cfg || !opts->legacy_blocks_dir || !opts->our_blocks_dir ||
        !opts->legacy_index_dir || !opts->chainstate_dir || !opts->btdb ||
        !opts->cvs || cfg->chainstate_batch_limit == 0 ||
        !cfg->block_index_long_op_name || !cfg->log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] import snapshot state: bad args\n");
        return false;
    }

    struct legacy_bootstrap_snapshot_import_result r = {
        .legacy_tip_height = -1,
    };

    int64_t linked = legacy_bootstrap_link_blk_files(opts->legacy_blocks_dir,
                                                     opts->our_blocks_dir,
                                                     cfg->log_prefix);
    if (linked < 0)
        return false;
    r.blk_files_linked = linked;

    int32_t block_index_tip_height = -1;
    int64_t bi_written = legacy_bootstrap_copy_block_index(
        opts->legacy_index_dir, opts->btdb, NULL,
        &block_index_tip_height, cfg->block_index_long_op_name,
        cfg->log_prefix);
    if (bi_written < 0)
        return false;
    r.block_index_writes = bi_written;

    if (cfg->min_legacy_tip >= 0 &&
        block_index_tip_height < cfg->min_legacy_tip) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] REFUSING: discovered legacy tip h=%d is below "
                "minimum %d; no chainstate import or cursor publication\n",
                cfg->log_prefix, block_index_tip_height,
                cfg->min_legacy_tip);
        return false;
    }

    struct legacy_bootstrap_chainstate_import_result cs_import;
    if (!legacy_bootstrap_import_chainstate_utxos(
            opts->chainstate_dir, opts->cvs, cfg->chainstate_batch_limit,
            cfg->chainstate_long_op_name, cfg->log_prefix, &cs_import))
        return false;

    r.utxos_imported = cs_import.inserted;
    if (cs_import.got_best_block)
        r.best_block = cs_import.best_block;

    if (!cs_import.got_best_block) {
        if (cfg->require_best_block) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] REFUSING: legacy chainstate had no 'B' key; "
                    "cannot publish an activatable tip\n",
                    cfg->log_prefix);
            return false;
        }
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] WARNING: legacy chainstate had no 'B' key; "
                "pending CSR anchor not recorded\n",
                cfg->log_prefix);
    } else if (opts->ndb) {
        int32_t anchor_height = opts->anchor_height;
        if (anchor_height < 0 &&
            !legacy_bootstrap_resolve_tip_height(
                opts->legacy_index_dir, &cs_import.best_block,
                cfg->log_prefix, &anchor_height)) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] REFUSING: chainstate best block is not on a "
                    "usable legacy index chain; no pending CSR anchor "
                    "published\n", cfg->log_prefix);
            return false;
        }
        if (cfg->min_legacy_tip >= 0 &&
            anchor_height < cfg->min_legacy_tip) {
            fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                    "[%s] REFUSING: chainstate anchor h=%d is below "
                    "minimum %d; no cursor publication\n",
                    cfg->log_prefix, anchor_height,
                    cfg->min_legacy_tip);
            return false;
        }
        r.legacy_tip_height = anchor_height;
        if (block_index_tip_height != anchor_height) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[%s] block-index tip h=%d differs from chainstate "
                    "anchor h=%d; publishing chainstate anchor\n",
                    cfg->log_prefix, block_index_tip_height,
                    anchor_height);
        }
        if (!legacy_bootstrap_record_pending_csr_anchor(
                opts->ndb, &cs_import.best_block, anchor_height,
                cs_import.inserted, cfg->log_prefix))
            return false;
    } else {
        r.legacy_tip_height = opts->anchor_height >= 0
            ? opts->anchor_height
            : block_index_tip_height;
    }

    if (out)
        *out = r;
    return true;
}

static bool legacy_bootstrap_import_staged_snapshot(
    enum legacy_bootstrap_snapshot_mode mode,
    const struct legacy_bootstrap_import_options *opts,
    const struct legacy_bootstrap_staged_snapshot_paths *paths,
    int32_t anchor_height,
    struct legacy_bootstrap_snapshot_import_result *imported,
    struct legacy_bootstrap_import_result *result)
{
    const struct legacy_bootstrap_snapshot_mode_config *cfg =
        legacy_bootstrap_snapshot_mode_cfg(mode);
    if (imported)
        *imported = (struct legacy_bootstrap_snapshot_import_result){
            .legacy_tip_height = -1,
        };
    if (!cfg || !opts || !paths || !paths->legacy_blocks_dir[0] ||
        !paths->our_blocks_dir[0] || !paths->idx_dir[0] ||
        !paths->cs_dir[0] || !imported || !result || !cfg->log_prefix) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] import staged snapshot: bad args\n");
        return false;
    }

    const struct legacy_bootstrap_snapshot_import_options import_opts = {
        .mode = mode,
        .legacy_blocks_dir = paths->legacy_blocks_dir,
        .our_blocks_dir = paths->our_blocks_dir,
        .legacy_index_dir = paths->idx_dir,
        .chainstate_dir = paths->cs_dir,
        .btdb = opts->btdb,
        .cvs = opts->cvs,
        .ndb = opts->ndb,
        .anchor_height = anchor_height,
    };
    int64_t t_import = legacy_bootstrap_now_ms();
    if (!legacy_bootstrap_import_snapshot_state(&import_opts, imported))
        return false;

    result->blk_files_linked = imported->blk_files_linked;
    result->block_index_writes = imported->block_index_writes;
    result->utxos_imported = imported->utxos_imported;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[%s] snapshot state import took %" PRId64 " ms (best h=%d)\n",
            cfg->log_prefix, legacy_bootstrap_now_ms() - t_import,
            imported->legacy_tip_height);
    return true;
}

static bool legacy_bootstrap_open_block_source(
    enum legacy_bootstrap_block_source_mode mode,
    const char *legacy_blocks_dir,
    const struct legacy_block_loc *map,
    size_t map_count,
    int legacy_tip,
    struct blocks_mmap **out_bmr)
{
    if (out_bmr)
        *out_bmr = NULL;
    const struct legacy_bootstrap_block_source_config *cfg =
        ((size_t)mode <
             sizeof(g_block_source_cfg) / sizeof(g_block_source_cfg[0]))
            ? &g_block_source_cfg[mode]
            : NULL;
    if (!legacy_blocks_dir || !map || map_count == 0 || legacy_tip < 0 ||
        !cfg || cfg->spotcheck_k <= 0 || !cfg->log_prefix || !out_bmr) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_bootstrap] open block source: bad args\n");
        return false;
    }

    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(legacy_blocks_dir, &bmr)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[%s] cannot open legacy blocks dir %s\n",
                cfg->log_prefix, legacy_blocks_dir);
        return false;
    }

    bool checked = legacy_bootstrap_spotcheck_sha3_windows(
        bmr, map, map_count, legacy_tip, cfg->spotcheck_k, cfg->log_prefix,
        cfg->debug_env, cfg->dump_map_on_failure);
    if (!checked) {
        if (cfg->require_spotcheck) {
            bmr_close(bmr);
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[%s] refusing to import: aborting due to spotcheck "
                    "failure\n",
                    cfg->log_prefix);
            return false;
        }
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] WARNING: SHA3 spotcheck did not pass; continuing "
                "with full validation\n",
                cfg->log_prefix);
    } else if (cfg->require_spotcheck) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] SHA3 source spotcheck passed\n",
                cfg->log_prefix);
    } else {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[%s] SHA3 source spotcheck passed; proof validation "
                "remains enabled\n",
                cfg->log_prefix);
    }

    *out_bmr = bmr;
    return true;
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

    if (!legacy_bootstrap_detect_datadir(opts->legacy_datadir, false)) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[cold_import] source %s does not look like a zclassic "
                "datadir\n", opts->legacy_datadir);
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

    int64_t t_start = legacy_bootstrap_now_ms();

    struct legacy_bootstrap_staged_snapshot_paths paths;
    int64_t t_snap = legacy_bootstrap_now_ms();
    if (!legacy_bootstrap_prepare_staged_snapshot(
            opts, LEGACY_BOOTSTRAP_COLD_STAGE_SUBDIR, "cold_import", &paths))
        return false;
    fprintf(stderr,  // obs-ok:cold-import-progress
            "[cold_import] LevelDB snapshots took %" PRId64 " ms\n",
            legacy_bootstrap_now_ms() - t_snap);

    struct uint256 cs_best_for_map;
    if (!legacy_bootstrap_read_chainstate_best_block(
            paths.cs_dir, "cold_import", &cs_best_for_map)) {
        legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
        return false;
    }

    struct legacy_bootstrap_height_map_result hmap;
    if (!legacy_bootstrap_load_height_map(paths.idx_dir, &cs_best_for_map,
                                          "cold_import", &hmap)) {
        legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
        return false;
    }
    struct legacy_block_loc *map = hmap.map;
    size_t map_count = hmap.map_count;
    int legacy_tip = hmap.tip_height;
    r.legacy_tip = legacy_tip;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[cold_import] legacy tip h=%d (map size=%zu)\n",
            legacy_tip, map_count);

    struct blocks_mmap *bmr = NULL;
    if (!legacy_bootstrap_open_block_source(
            LEGACY_BOOTSTRAP_BLOCK_SOURCE_COLD,
            paths.legacy_blocks_dir, map, map_count, legacy_tip,
            &bmr)) {
        bilr_free_height_map(map);
        legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
        return false;
    }
    bmr_close(bmr);

    struct legacy_bootstrap_snapshot_import_result imported;
    bool import_ok = legacy_bootstrap_import_staged_snapshot(
        LEGACY_BOOTSTRAP_SNAPSHOT_COLD, opts, &paths, legacy_tip, &imported,
        &r);
    bilr_free_height_map(map);
    if (!import_ok) {
        legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
        return false;
    }

    double total_secs = (double)(legacy_bootstrap_now_ms() - t_start) / 1000.0;
    if (out) *out = r;
    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[cold_import] DONE in %.1fs: block_index=%" PRId64
            " utxos=%" PRId64 " blk_files=%" PRId64 "\n",
            total_secs, r.block_index_writes, r.utxos_imported,
            r.blk_files_linked);
    legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
    return true;
}

static bool legacy_bootstrap_import_direct(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out)
{
    struct legacy_bootstrap_import_result r = {
        .legacy_tip = -1,
    };
    if (out) *out = r;

    if (!opts || !opts->ms || !opts->coins_tip || !opts->params ||
        !opts->our_datadir || !opts->legacy_datadir) {
        fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
                "[legacy_direct_import] bad args\n");
        return false;
    }

    if (!legacy_bootstrap_detect_datadir(opts->legacy_datadir, false)) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] %s does not contain "
                "blocks/blk00000.dat — skipping\n",
                opts->legacy_datadir);
        if (out) *out = r;
        return true;
    }

    char idx_dir[1024];
    char blk_dir[1024];
    if (!legacy_bootstrap_format_child_path(
            opts->legacy_datadir, "blocks/index", idx_dir, sizeof(idx_dir),
            "legacy_direct_import", "legacy block-index directory") ||
        !legacy_bootstrap_format_child_path(
            opts->legacy_datadir, "blocks", blk_dir, sizeof(blk_dir),
            "legacy_direct_import", "legacy blocks directory"))
        return false;

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

    int from_height = active_chain_height(&opts->ms->chain_active);
    if (from_height < 0)
        from_height = 0;
    if (from_height >= legacy_tip) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] already at/past legacy tip "
                "(from=%d legacy=%d) — nothing to do\n",
                from_height, legacy_tip);
        bilr_free_height_map(map);
        if (out) *out = r;
        return true;
    }

    struct blocks_mmap *bmr = NULL;
    if (!legacy_bootstrap_open_block_source(
            LEGACY_BOOTSTRAP_BLOCK_SOURCE_DIRECT,
            blk_dir, map, map_count, legacy_tip,
            &bmr)) {
        bilr_free_height_map(map);
        if (out) *out = r;
        return false;
    }

    atomic_store(&g_body_pull_active, 1);

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] starting walk: [%d+1 .. %d] "
            "(%d blocks)\n",
            from_height, legacy_tip, legacy_tip - from_height);

    int64_t t_walk = legacy_bootstrap_now_ms();
    int last_log_h = from_height;
    int64_t t_last_log = t_walk;
    int skipped_have_data = 0;
    int skipped_failed = 0;
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
            skipped_have_data++;
            continue;
        }
        if (failed) {
            skipped_failed++;
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
    bmr_close(bmr);
    bilr_free_height_map(map);

    int final_tip = active_chain_height(&opts->ms->chain_active);

    fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[legacy_direct_import] walk %s: applied=%d "
            "skipped_have=%d skipped_failed=%d elapsed=%.1fs "
            "rate=%.1f bps final_tip=%d\n",
            ok ? "complete" : "ABORTED",
            r.applied, skipped_have_data, skipped_failed,
            total_secs, avg_rate, final_tip);

    if (ok && opts->wallet && r.applied > 0) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[legacy_direct_import] starting wallet rescan "
                "[%d..%d]...\n", from_height + 1, final_tip);
        int64_t t_rescan = legacy_bootstrap_now_ms();
        int hits = wallet_rescan(opts->wallet, &opts->ms->chain_active,
                                 from_height + 1, final_tip,
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

const char *legacy_attach_outcome_name(enum legacy_attach_outcome o)
{
    switch (o) {
        case LEGACY_ATTACH_OUTCOME_DID_IMPORT:           return "did_import";
        case LEGACY_ATTACH_OUTCOME_NOOP_SAME_TIP:        return "noop_same_tip";
        case LEGACY_ATTACH_OUTCOME_RECOVERED_FROM_CRASH: return "recovered_from_crash";
        case LEGACY_ATTACH_OUTCOME_REFUSED_HAS_STATE:    return "refused_has_state";
        case LEGACY_ATTACH_OUTCOME_LEGACY_NOT_FOUND:     return "legacy_not_found";
        case LEGACY_ATTACH_OUTCOME_FAILED:               return "failed";
    }
    return "?";
}

static bool legacy_bootstrap_attach_meta_has_sentinel(sqlite3 *db)
{
    if (!db) return false;
    uint8_t buf[1];
    size_t got = 0;
    bool found = false;
    if (!progress_meta_get(db, LEGACY_BOOTSTRAP_ATTACH_META_SENTINEL,
                           buf, sizeof(buf), &got, &found))
        return false;
    return found;
}

static bool legacy_bootstrap_attach_meta_get_tip(sqlite3 *db,
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
    if (!progress_meta_get(db, LEGACY_BOOTSTRAP_ATTACH_META_TIP_HASH,
                           out_hash ? out_hash->data : NULL,
                           out_hash ? sizeof(out_hash->data) : 0,
                           &nh, &fh)) return false;
    if (!progress_meta_get(db, LEGACY_BOOTSTRAP_ATTACH_META_TIP_HEIGHT,
                           out_height, sizeof(*out_height), &nH, &fH))
        return false;
    if (!fh || !fH) return true;
    if (nh != 32 || nH != sizeof(*out_height)) return true;
    if (out_found) *out_found = true;
    return true;
}

static const char *const LEGACY_ATTACH_STAGES_TO_STAMP[] = {
    "header_admit",
    "validate_headers",
    "body_fetch",
    NULL,
};

#ifdef ZCL_TESTING
static size_t legacy_attach_stages_count_cached(void)
{
    size_t n = 0;
    while (LEGACY_ATTACH_STAGES_TO_STAMP[n]) n++;
    return n;
}

size_t legacy_attach_stages_to_stamp_count(void)
{
    return legacy_attach_stages_count_cached();
}

const char *legacy_attach_stages_to_stamp_at(size_t i)
{
    if (i >= legacy_attach_stages_count_cached()) return NULL;
    return LEGACY_ATTACH_STAGES_TO_STAMP[i];
}
#endif

static bool legacy_bootstrap_attach_stamp_stage_cursor_in_tx(
    sqlite3 *db,
    const char *name,
    uint64_t new_cursor,
    bool *out_was_write)
{
    if (out_was_write) *out_was_write = false;

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

    if (have_row && existing >= new_cursor) {
        fprintf(stderr,  // obs-ok:legacy-attach-no-rewind
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

static bool legacy_bootstrap_attach_finalize_atomic(
    sqlite3 *db,
    int32_t legacy_tip,
    const struct uint256 *legacy_tip_hash,
    int64_t *out_stages_stamped)
{
    if (out_stages_stamped) *out_stages_stamped = 0;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:legacy-attach-finalize-failure
                "[legacy_attach] finalize BEGIN failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }

    bool ok = true;
    int64_t stamped = 0;
    uint64_t cursor_value = (uint64_t)(legacy_tip + 1);
    for (size_t i = 0; ok && LEGACY_ATTACH_STAGES_TO_STAMP[i]; i++) {
        bool was_write = false;
        if (!legacy_bootstrap_attach_stamp_stage_cursor_in_tx(
                db, LEGACY_ATTACH_STAGES_TO_STAMP[i], cursor_value, &was_write)) {
            fprintf(stderr,  // obs-ok:legacy-attach-finalize-failure
                    "[legacy_attach] stamp stage '%s' failed\n",
                    LEGACY_ATTACH_STAGES_TO_STAMP[i]);
            ok = false;
        } else if (was_write) {
            stamped++;
        }
    }

    if (ok) {
        ok = progress_meta_set_in_tx(db, LEGACY_BOOTSTRAP_ATTACH_META_TIP_HASH,
                                     legacy_tip_hash->data, 32);
    }
    if (ok) {
        ok = progress_meta_set_in_tx(db,
                                     LEGACY_BOOTSTRAP_ATTACH_META_TIP_HEIGHT,
                                     &legacy_tip, sizeof(legacy_tip));
    }
    if (ok) {
        ok = progress_meta_delete_in_tx(
            db, LEGACY_BOOTSTRAP_ATTACH_META_SENTINEL);
    }

    const char *fini = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, fini, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:legacy-attach-finalize-failure
                "[legacy_attach] finalize %s failed: %s\n",
                fini, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    if (out_stages_stamped) *out_stages_stamped = stamped;
    return ok;
}

#ifdef ZCL_TESTING
bool legacy_attach_stamp_one_for_test(sqlite3 *db, const char *name,
                                      uint64_t cursor, bool *out_was_write)
{
    if (!db) return false;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = legacy_bootstrap_attach_stamp_stage_cursor_in_tx(
        db, name, cursor, out_was_write);
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return ok;
}
#endif

static bool legacy_bootstrap_attach_wipe_block_index(
    struct block_tree_db *our_btdb)
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
    fprintf(stderr,  // obs-ok:legacy-attach-wipe
            "[legacy_attach] wiped %" PRId64 " block_index entries "
            "from prior aborted import\n", deleted);
    return ok;
}

static bool legacy_bootstrap_import_attach(
    const struct legacy_bootstrap_import_options *opts,
    struct legacy_bootstrap_import_result *out)
{
    struct legacy_bootstrap_import_result r = {
        .legacy_tip = -1,
        .outcome = LEGACY_ATTACH_OUTCOME_FAILED,
    };
    if (out) *out = r;

    if (!opts || !opts->our_datadir || !opts->legacy_datadir || !opts->ms ||
        !opts->cvs || !opts->ndb || !opts->ndb->open || !opts->btdb) {
        LOG_FAIL("legacy_bootstrap_attach", "bad args");
    }

    sqlite3 *pdb = progress_store_db();
    if (!pdb) {
        fprintf(stderr,  // obs-ok:legacy-attach-preflight
            "[legacy_attach] progress.kv not open — boot order regression?\n");
        return false;
    }

    if (!legacy_bootstrap_detect_datadir(opts->legacy_datadir, true)) {
        fprintf(stderr,  // obs-ok:legacy-attach-soft-skip
            "[legacy_attach] %s does not look like a zclassic datadir; "
            "skipping.\n", opts->legacy_datadir);
        r.outcome = LEGACY_ATTACH_OUTCOME_LEGACY_NOT_FOUND;
        if (out) *out = r;
        return true;
    }

    int our_tip = active_chain_height(&opts->ms->chain_active);
    bool sentinel_present = legacy_bootstrap_attach_meta_has_sentinel(pdb);
    if (our_tip > LEGACY_BOOTSTRAP_ATTACH_REFUSE_ABOVE_TIP &&
        !sentinel_present) {
        fprintf(stderr,  // obs-ok:legacy-attach-refused
            "[legacy_attach] REFUSING: our active_tip=%d > %d. "
            "Legacy-attach is for empty datadirs; use other modes for "
            "warm catch-up.\n", our_tip,
            LEGACY_BOOTSTRAP_ATTACH_REFUSE_ABOVE_TIP);
        r.outcome = LEGACY_ATTACH_OUTCOME_REFUSED_HAS_STATE;
        if (out) *out = r;
        return true;
    }

    if (!sentinel_present) {
        struct uint256 last_hash;
        int32_t last_h = -1;
        bool last_found = false;
        if (legacy_bootstrap_attach_meta_get_tip(pdb, &last_hash, &last_h,
                                                 &last_found) &&
            last_found) {
            struct uint256 cur_best;
            if (legacy_bootstrap_probe_chainstate_best(
                    opts, LEGACY_BOOTSTRAP_ATTACH_STAGE_SUBDIR,
                    "probe-chainstate", "legacy_attach", &cur_best) &&
                memcmp(cur_best.data, last_hash.data, 32) == 0) {
                r.outcome = LEGACY_ATTACH_OUTCOME_NOOP_SAME_TIP;
                r.legacy_tip = last_h;
                if (out) *out = r;
                fprintf(stderr,  // obs-ok:legacy-attach-noop
                    "[legacy_attach] NOOP: already attached "
                    "to legacy tip h=%d\n", last_h);
                return true;
            }
        }
    } else {
        if (our_tip > 100) {
            fprintf(stderr,  // obs-ok:legacy-attach-wipe-refused
                "[legacy_attach] REFUSING wipe: sentinel found AND "
                "active_chain_height=%d > 100. This combination is "
                "anomalous (sentinel should clear atomically with import "
                "completion). Manual intervention required: inspect "
                "progress.kv (sqlite3 -- DELETE FROM progress_meta WHERE "
                "key='import_in_progress') if the sentinel is truly "
                "stale.\n", our_tip);
            return false;
        }
        fprintf(stderr,  // obs-ok:legacy-attach-recovery
                "[legacy_attach] sentinel found from a prior aborted "
                "import — recovering: wipe + re-import (active_tip=%d)\n",
                our_tip);
        if (!legacy_bootstrap_attach_wipe_block_index(opts->btdb)) {
            fprintf(stderr,
                "[legacy_attach] wipe of stale block_index failed\n");
            return false;
        }
        r.outcome = LEGACY_ATTACH_OUTCOME_RECOVERED_FROM_CRASH;
    }

    int64_t t_start = legacy_bootstrap_now_ms();

    uint8_t one = 1;
    if (!progress_meta_set(pdb, LEGACY_BOOTSTRAP_ATTACH_META_SENTINEL,
                           &one, 1)) {
        fprintf(stderr,
            "[legacy_attach] failed to set in-progress sentinel\n");
        return false;
    }

    struct legacy_bootstrap_staged_snapshot_paths paths;
    if (!legacy_bootstrap_prepare_staged_snapshot(
            opts, LEGACY_BOOTSTRAP_ATTACH_STAGE_SUBDIR, "legacy_attach",
            &paths)) {
        return false;
    }
    struct legacy_bootstrap_snapshot_import_result imported;
    if (!legacy_bootstrap_import_staged_snapshot(
            LEGACY_BOOTSTRAP_SNAPSHOT_ATTACH, opts, &paths, -1, &imported,
            &r)) {
        legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
        return false;
    }
    r.legacy_tip = imported.legacy_tip_height;

    int64_t stages_stamped = 0;
    if (!legacy_bootstrap_attach_finalize_atomic(
            pdb, imported.legacy_tip_height, &imported.best_block,
            &stages_stamped)) {
        legacy_bootstrap_cleanup_staged_snapshot(&paths, false);
        return false;
    }
    r.stages_stamped = stages_stamped;

    legacy_bootstrap_cleanup_staged_snapshot(&paths, true);

    double total_secs = (double)(legacy_bootstrap_now_ms() - t_start) / 1000.0;
    if (r.outcome == LEGACY_ATTACH_OUTCOME_FAILED)
        r.outcome = LEGACY_ATTACH_OUTCOME_DID_IMPORT;
    if (out) *out = r;

    fprintf(stderr,  // obs-ok:legacy-attach-done
        "[legacy_attach] DONE outcome=%s in %.1fs: legacy_tip=%d "
        "block_index=%" PRId64 " utxos=%" PRId64 " blk_files=%" PRId64
        " stages_stamped=%" PRId64 "\n",
        legacy_attach_outcome_name((enum legacy_attach_outcome)r.outcome), total_secs,
        r.legacy_tip, r.block_index_writes, r.utxos_imported,
        r.blk_files_linked, r.stages_stamped);

    return true;
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
        case LEGACY_BOOTSTRAP_IMPORT_ATTACH:
            return legacy_bootstrap_import_attach(opts, out);
    }

    fprintf(stderr,  // obs-ok:bootstrap-import-terminal-diagnostic
            "[legacy_bootstrap] import: unknown mode %d\n", (int)opts->mode);
    return false;
}
