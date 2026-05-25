/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared legacy bootstrap import primitives. */

#include "services/legacy_bootstrap_importer.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "storage/block_index_db.h"
#include "storage/dbwrapper.h"
#include "storage/ldb_snapshot.h"
#include "util/long_op.h"
#include "util/thread_registry.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
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
