/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "models/chainstate_store.h"
#include "models/leveldb_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool chainstate_store_validate(struct chainstate_store *cs,
                                struct ar_errors *errors)
{
    ar_errors_clear(errors);
    cs->num_sst_files = 0;
    cs->total_bytes = 0;
    cs->has_manifest = false;
    cs->has_current = false;
    cs->copy_ok = false;

    if (!cs->src_dir)
        ar_errors_add(errors, "src_dir", "can't be blank");
    if (!cs->dst_dir)
        ar_errors_add(errors, "dst_dir", "can't be blank");
    if (ar_errors_any(errors))
        return false;

    struct stat st;
    if (stat(cs->src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ar_errors_add(errors, "src_dir", "is not a directory");
        return false;
    }

    scan_leveldb_dir(cs->src_dir, &cs->num_sst_files, &cs->total_bytes,
                     &cs->has_manifest, &cs->has_current);

    if (!cs->has_manifest)
        ar_errors_add(errors, "has_manifest", "MANIFEST file required");
    if (!cs->has_current)
        ar_errors_add(errors, "has_current", "CURRENT file required");
    if (cs->total_bytes == 0)
        ar_errors_add(errors, "total_bytes", "must be positive");

    return !ar_errors_any(errors);
}

bool chainstate_store_save(struct chainstate_store *cs)
{
    cs->copy_ok = false;

    printf("chainstate_store: clean copy (%d SST files, %.1f MB)...\n",
           cs->num_sst_files,
           (double)cs->total_bytes / (1024.0 * 1024.0));
    fflush(stdout);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s' && mkdir -p '%s' && cp -a '%s'/. '%s'/ 2>/dev/null",
             cs->dst_dir, cs->dst_dir, cs->src_dir, cs->dst_dir);
    cs->copy_ok = (system(cmd) == 0);
    return cs->copy_ok;
}

void chainstate_store_summary(const struct chainstate_store *cs,
                               char *out, size_t len)
{
    snprintf(out, len, "sst=%d(%.1fMB) manifest=%s current=%s copy=%s",
             cs->num_sst_files,
             (double)cs->total_bytes / (1024.0 * 1024.0),
             cs->has_manifest ? "yes" : "no",
             cs->has_current ? "yes" : "no",
             cs->copy_ok ? "ok" : "fail");
}
