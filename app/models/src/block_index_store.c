/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "models/block_index_store.h"
#include "models/leveldb_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool block_index_store_validate(struct block_index_store *idx,
                                 struct ar_errors *errors)
{
    ar_errors_clear(errors);
    idx->num_sst_files = 0;
    idx->total_bytes = 0;
    idx->has_manifest = false;
    idx->has_current = false;
    idx->copy_ok = false;

    if (!idx->src_dir)
        ar_errors_add(errors, "src_dir", "can't be blank");
    if (!idx->dst_dir)
        ar_errors_add(errors, "dst_dir", "can't be blank");
    if (ar_errors_any(errors))
        return false;

    struct stat st;
    if (stat(idx->src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ar_errors_add(errors, "src_dir", "is not a directory");
        return false;
    }

    scan_leveldb_dir(idx->src_dir, &idx->num_sst_files, &idx->total_bytes,
                     &idx->has_manifest, &idx->has_current);

    if (!idx->has_manifest)
        ar_errors_add(errors, "has_manifest", "MANIFEST file required");
    if (!idx->has_current)
        ar_errors_add(errors, "has_current", "CURRENT file required");
    if (idx->num_sst_files == 0)
        ar_errors_add(errors, "num_sst_files", "must be positive");

    return !ar_errors_any(errors);
}

bool block_index_store_save(struct block_index_store *idx)
{
    idx->copy_ok = false;

    printf("block_index_store: clean copy (%d SST files, %.1f MB)...\n",
           idx->num_sst_files,
           (double)idx->total_bytes / (1024.0 * 1024.0));
    fflush(stdout);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s' && mkdir -p '%s' && cp -a '%s'/. '%s'/ 2>/dev/null",
             idx->dst_dir, idx->dst_dir, idx->src_dir, idx->dst_dir);
    idx->copy_ok = (system(cmd) == 0);
    return idx->copy_ok;
}

void block_index_store_summary(const struct block_index_store *idx,
                                char *out, size_t len)
{
    snprintf(out, len, "sst=%d(%.1fMB) manifest=%s current=%s copy=%s",
             idx->num_sst_files,
             (double)idx->total_bytes / (1024.0 * 1024.0),
             idx->has_manifest ? "yes" : "no",
             idx->has_current ? "yes" : "no",
             idx->copy_ok ? "ok" : "fail");
}
