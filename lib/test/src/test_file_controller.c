/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * File controller tests — manifest coverage and cache invalidation. */

#include "test/test_helpers.h"
#include "controllers/file_controller.h"
#include "net/file_service.h"
#include <utime.h>
#include <unistd.h>

static void test_file_write_bytes(const char *path, size_t size, uint8_t seed)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;

    uint8_t buf[4096];
    memset(buf, seed, sizeof(buf));
    while (size > 0) {
        size_t n = size > sizeof(buf) ? sizeof(buf) : size;
        fwrite(buf, 1, n, f);
        size -= n;
    }
    fclose(f);
}

static void test_file_touch_age(const char *path, int seconds_ago)
{
    struct utimbuf tb;
    time_t now = time(NULL);
    tb.actime = now - seconds_ago;
    tb.modtime = now - seconds_ago;
    utime(path, &tb);
}

static bool manifest_has_file_index(const struct file_manifest *fm,
                                    uint8_t file_index)
{
    for (uint32_t i = 0; i < fm->num_chunks; i++) {
        if (fm->chunks[i].file_index == file_index)
            return true;
    }
    return false;
}

static void setup_manifest_test_dir(char *dir, size_t dir_sz)
{
    snprintf(dir, dir_sz, ".zcl_test_file_manifest_%d", (int)getpid());
    mkdir(dir, 0755);

    char blocks[320];
    snprintf(blocks, sizeof(blocks), "%s/blocks", dir);
    mkdir(blocks, 0755);
}

static void cleanup_manifest_test_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

static int test_manifest_build_includes_snapshot(void)
{
    int failures = 0;

    printf("file_controller: manifest includes consensus snapshot when present... ");
    {
        char dir[256];
        char blk[320];
        char snap[320];
        struct file_manifest fm;

        setup_manifest_test_dir(dir, sizeof(dir));
        snprintf(blk, sizeof(blk), "%s/blocks/blk00000.dat", dir);
        snprintf(snap, sizeof(snap), "%s/consensus_snapshot.db", dir);

        test_file_write_bytes(blk, 4096, 0x11);
        test_file_touch_age(blk, 7200);
        test_file_write_bytes(snap, 1100000, 0x22);

        memset(&fm, 0, sizeof(fm));
        bool ok = file_manifest_build(&fm, dir);
        ok = ok && manifest_has_file_index(&fm, 254);

        cleanup_manifest_test_dir(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

static int test_manifest_cache_rebuilds_when_snapshot_appears(void)
{
    int failures = 0;

    printf("file_controller: cached manifest rebuilds when snapshot appears... ");
    {
        char dir[256];
        char blk[320];
        char snap[320];
        struct file_manifest fm;

        setup_manifest_test_dir(dir, sizeof(dir));
        snprintf(blk, sizeof(blk), "%s/blocks/blk00000.dat", dir);
        snprintf(snap, sizeof(snap), "%s/consensus_snapshot.db", dir);

        test_file_write_bytes(blk, 4096, 0x33);
        test_file_touch_age(blk, 7200);

        memset(&fm, 0, sizeof(fm));
        bool ok = file_manifest_build(&fm, dir);
        ok = ok && !manifest_has_file_index(&fm, 254);

        test_file_write_bytes(snap, 1100000, 0x44);
        memset(&fm, 0, sizeof(fm));
        ok = ok && file_manifest_build(&fm, dir);
        ok = ok && manifest_has_file_index(&fm, 254);

        cleanup_manifest_test_dir(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

static int test_manifest_skips_hot_block_index(void)
{
    int failures = 0;

    printf("file_controller: manifest skips recently modified block_index.bin... ");
    {
        char dir[256];
        char blk[320];
        char flat[320];
        struct file_manifest fm;

        setup_manifest_test_dir(dir, sizeof(dir));
        snprintf(blk, sizeof(blk), "%s/blocks/blk00000.dat", dir);
        snprintf(flat, sizeof(flat), "%s/block_index.bin", dir);

        test_file_write_bytes(blk, 4096, 0x55);
        test_file_touch_age(blk, 7200);
        test_file_write_bytes(flat, 1100000, 0x66);

        memset(&fm, 0, sizeof(fm));
        bool ok = file_manifest_build(&fm, dir);
        ok = ok && !manifest_has_file_index(&fm, 253);

        cleanup_manifest_test_dir(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

static int test_controller_refresh_manifest_api(void)
{
    int failures = 0;

    printf("file_controller: explicit refresh builds cached manifest... ");
    {
        char dir[256];
        char blk[320];
        char snap[320];
        const struct file_manifest *fm = NULL;

        setup_manifest_test_dir(dir, sizeof(dir));
        snprintf(blk, sizeof(blk), "%s/blocks/blk00000.dat", dir);
        snprintf(snap, sizeof(snap), "%s/consensus_snapshot.db", dir);

        test_file_write_bytes(blk, 4096, 0x77);
        test_file_touch_age(blk, 7200);
        test_file_write_bytes(snap, 1100000, 0x88);

        file_controller_init(dir);
        bool ok = file_controller_refresh_manifest();
        fm = file_controller_get_manifest();
        ok = ok && fm != NULL && manifest_has_file_index(fm, 254);

        cleanup_manifest_test_dir(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

static int test_manifest_status_reports_export_readiness(void)
{
    int failures = 0;

    printf("file_controller: manifest status reports export readiness... ");
    {
        char dir[256];
        char blk[320];
        char snap[320];
        char flat[320];
        struct file_manifest_status status;

        setup_manifest_test_dir(dir, sizeof(dir));
        snprintf(blk, sizeof(blk), "%s/blocks/blk00000.dat", dir);
        snprintf(snap, sizeof(snap), "%s/consensus_snapshot.db", dir);
        snprintf(flat, sizeof(flat), "%s/block_index.bin", dir);

        test_file_write_bytes(blk, 4096, 0x99);
        test_file_touch_age(blk, 7200);
        test_file_write_bytes(snap, 1100000, 0xAA);
        test_file_write_bytes(flat, 1100000, 0xBB);

        file_controller_init(dir);
        memset(&status, 0, sizeof(status));
        file_controller_get_manifest_status(&status);
        bool ok = status.datadir_configured;
        ok = ok && !status.manifest_valid;
        ok = ok && status.snapshot_present;
        ok = ok && !status.snapshot_served;
        ok = ok && status.block_index_present;
        ok = ok && !status.block_index_served;

        ok = ok && file_controller_refresh_manifest();
        memset(&status, 0, sizeof(status));
        file_controller_get_manifest_status(&status);
        ok = ok && status.manifest_valid;
        ok = ok && status.snapshot_present;
        ok = ok && status.snapshot_served;
        ok = ok && status.block_index_present;
        ok = ok && !status.block_index_served;
        ok = ok && status.num_chunks > 0;
        ok = ok && status.total_bytes > 0;

        cleanup_manifest_test_dir(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

static int test_file_service_start_stop_idempotent(void)
{
    int failures = 0;

    printf("file_controller: file service start/stop is idempotent... ");
    {
        char dir[256];
        char blk[320];
        bool ok = true;

        setup_manifest_test_dir(dir, sizeof(dir));
        snprintf(blk, sizeof(blk), "%s/blocks/blk00000.dat", dir);
        test_file_write_bytes(blk, 4096, 0xCC);
        test_file_touch_age(blk, 7200);

        fs_server_start(dir, 0);
        fs_server_start(dir, 0);
        sleep(1);
        fs_server_stop();
        fs_server_stop();

        cleanup_manifest_test_dir(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

int test_file_controller(void)
{
    int failures = 0;

    failures += test_manifest_build_includes_snapshot();
    failures += test_manifest_cache_rebuilds_when_snapshot_appears();
    failures += test_manifest_skips_hot_block_index();
    failures += test_controller_refresh_manifest_api();
    failures += test_manifest_status_reports_export_readiness();
    failures += test_file_service_start_stop_idempotent();

    return failures;
}
