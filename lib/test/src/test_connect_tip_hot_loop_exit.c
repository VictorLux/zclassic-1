/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "validation/process_block.h"
#include "event/event.h"
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

extern volatile sig_atomic_t g_shutdown_requested;
extern void process_block_test_set_utxo_fail_state(int height, int count);
extern int process_block_test_get_utxo_fail_count(void);
extern void process_block_test_trigger_hot_loop_check(int height,
                                                      const char *datadir);

static int hot_loop_event_count(const char *needle)
{
    char buf[4096];
    size_t len = event_dump_json(buf, sizeof(buf) - 1, 32);
    buf[len] = '\0';

    int count = 0;
    const char *p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

static void remove_if_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        unlink(path);
}

int test_connect_tip_hot_loop_exit(void)
{
    int failures = 0;
    char dir_template[] = "./test-tmp/hot_loop_exit_XXXXXX";
    char *datadir;
    char flag_path[512];
    char marker_path[512];

    mkdir("./test-tmp", 0755);
    datadir = mkdtemp(dir_template);
    if (!datadir) {
        printf("connect_tip hot-loop exit: FAIL (mkdtemp)\n");
        return 1;
    }

    snprintf(flag_path, sizeof(flag_path), "%s/needs_reimport", datadir);
    snprintf(marker_path, sizeof(marker_path), "%s/last_reimport_attempted",
             datadir);

    printf("hot_loop_exit: 10 failures request shutdown + write flag... ");
    {
        remove_if_exists(flag_path);
        remove_if_exists(marker_path);
        event_log_init();
        g_shutdown_requested = 0;
        process_block_test_set_utxo_fail_state(3078015, 10);
        process_block_test_trigger_hot_loop_check(3078015, datadir);

        struct stat st;
        bool ok = stat(flag_path, &st) == 0 &&
                  g_shutdown_requested == 1 &&
                  process_block_test_get_utxo_fail_count() == 10 &&
                  hot_loop_event_count("FATAL_HOT_LOOP h=3078015") == 1 &&
                  hot_loop_event_count("FATAL_HOT_LOOP_STUCK h=3078015") == 0;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("hot_loop_exit: recent marker blocks shutdown request... ");
    {
        remove_if_exists(flag_path);
        FILE *marker = fopen(marker_path, "w");
        bool ok = marker != NULL;
        if (marker) {
            fputs("1\n", marker);
            fclose(marker);
        }

        if (ok) {
            event_log_init();
            g_shutdown_requested = 0;
            process_block_test_set_utxo_fail_state(3078015, 10);
            process_block_test_trigger_hot_loop_check(3078015, datadir);

            struct stat st;
            ok = stat(flag_path, &st) == 0 &&
                 g_shutdown_requested == 0 &&
                 hot_loop_event_count("FATAL_HOT_LOOP h=3078015") == 0 &&
                 hot_loop_event_count("FATAL_HOT_LOOP_STUCK h=3078015") == 1;
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("hot_loop_exit: stale marker allows shutdown request... ");
    {
        struct utimbuf old_times;
        time_t now_s = time(NULL);
        old_times.actime = now_s - 700;
        old_times.modtime = now_s - 700;
        bool ok = utime(marker_path, &old_times) == 0;
        if (ok) {
            event_log_init();
            g_shutdown_requested = 0;
            process_block_test_set_utxo_fail_state(3078015, 10);
            process_block_test_trigger_hot_loop_check(3078015, datadir);

            ok = g_shutdown_requested == 1 &&
                 hot_loop_event_count("FATAL_HOT_LOOP h=3078015") == 1 &&
                 hot_loop_event_count("FATAL_HOT_LOOP_STUCK h=3078015") == 0;
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("hot_loop_exit: 11th same-height failure is idempotent... ");
    {
        remove_if_exists(marker_path);
        event_log_init();
        g_shutdown_requested = 0;
        process_block_test_set_utxo_fail_state(3078015, 10);
        process_block_test_trigger_hot_loop_check(3078015, datadir);
        bool ok = g_shutdown_requested == 1 &&
                  hot_loop_event_count("FATAL_HOT_LOOP h=3078015") == 1;

        if (ok) {
            process_block_test_set_utxo_fail_state(3078015, 11);
            process_block_test_trigger_hot_loop_check(3078015, datadir);
            ok = g_shutdown_requested == 1 &&
                 hot_loop_event_count("FATAL_HOT_LOOP h=3078015") == 1;
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    remove_if_exists(flag_path);
    remove_if_exists(marker_path);
    rmdir(datadir);

    return failures;
}
