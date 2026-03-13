/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "models/block_data.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void count_dat_files(const char *dir, const char *prefix,
                            int *count, int64_t *bytes)
{
    *count = 0;
    *bytes = 0;
    DIR *d = opendir(dir);
    if (!d) return;

    size_t pfx_len = strlen(prefix);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, pfx_len) == 0 &&
            strstr(ent->d_name, ".dat")) {
            (*count)++;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0)
                *bytes += st.st_size;
        }
    }
    closedir(d);
}

bool block_data_validate(struct block_data *bd, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    bd->num_blk_files = 0;
    bd->num_rev_files = 0;
    bd->blk_bytes = 0;
    bd->rev_bytes = 0;
    bd->copy_blk_ok = false;
    bd->copy_rev_ok = false;

    if (!bd->src_dir)
        ar_errors_add(errors, "src_dir", "can't be blank");
    if (!bd->dst_dir)
        ar_errors_add(errors, "dst_dir", "can't be blank");
    if (ar_errors_any(errors))
        return false;

    struct stat st;
    if (stat(bd->src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ar_errors_add(errors, "src_dir", "is not a directory");
        return false;
    }

    count_dat_files(bd->src_dir, "blk", &bd->num_blk_files, &bd->blk_bytes);
    count_dat_files(bd->src_dir, "rev", &bd->num_rev_files, &bd->rev_bytes);

    if (bd->num_blk_files == 0)
        ar_errors_add(errors, "num_blk_files", "must be positive");

    return !ar_errors_any(errors);
}

bool block_data_save(struct block_data *bd)
{
    bd->copy_blk_ok = false;
    bd->copy_rev_ok = false;

    struct stat st;
    if (stat(bd->dst_dir, &st) != 0)
        mkdir(bd->dst_dir, 0700);

    char cmd[4096];

    printf("block_data: copying %d blk files (%.1f GB)...\n",
           bd->num_blk_files,
           (double)bd->blk_bytes / (1024.0 * 1024.0 * 1024.0));
    fflush(stdout);
    snprintf(cmd, sizeof(cmd),
             "cp -au '%s'/blk*.dat '%s'/ 2>/dev/null", bd->src_dir, bd->dst_dir);
    bd->copy_blk_ok = (system(cmd) == 0);

    if (bd->num_rev_files > 0) {
        printf("block_data: copying %d rev files...\n", bd->num_rev_files);
        fflush(stdout);
        snprintf(cmd, sizeof(cmd),
                 "cp -au '%s'/rev*.dat '%s'/ 2>/dev/null",
                 bd->src_dir, bd->dst_dir);
        bd->copy_rev_ok = (system(cmd) == 0);
    } else {
        bd->copy_rev_ok = true;
    }

    return bd->copy_blk_ok;
}

void block_data_summary(const struct block_data *bd, char *out, size_t len)
{
    snprintf(out, len, "blk=%d(%.1fGB) rev=%d copy=%s",
             bd->num_blk_files,
             (double)bd->blk_bytes / (1024.0 * 1024.0 * 1024.0),
             bd->num_rev_files,
             bd->copy_blk_ok ? "ok" : "fail");
}
