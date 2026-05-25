/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "sim/postmortem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PM_CHECK(name, expr) do { \
    printf("postmortem: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int rm_rf_simple(const char *path)
{
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    return system(cmd);
}

static bool file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int test_postmortem(void)
{
    printf("\n=== postmortem tests ===\n");
    int failures = 0;

    char dir_template[128];
    snprintf(dir_template, sizeof(dir_template),
             "/tmp/zcl_postmortem_%d_XXXXXX", (int)getpid());
    char *dir = mkdtemp(dir_template);
    PM_CHECK("mkdtemp", dir != NULL);
    if (!dir) return failures + 1;

    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/node.log", dir);
    FILE *lf = fopen(log_path, "wb");
    PM_CHECK("create source log", lf != NULL);
    if (lf) {
        fprintf(lf, "line one\nfatal breadcrumb\n");
        fclose(lf);
    }

    seed_tape_t *tape = seed_tape_open(0x12345678ULL, 1779665000);
    PM_CHECK("seed tape open", tape != NULL);
    if (tape) {
        seed_tape_advance(tape, 5000);
        seed_tape_inject(tape, 7, "abc", 3);
    }

    char cap_path[512];
    struct postmortem_capture_opts opts = {
        .dir = dir,
        .tape = tape,
        .crash_signal = 11,
        .crash_unix = 1779665123,
        .reason = "unit-test",
        .log_path = log_path,
    };
    int rc = postmortem_capture_write(&opts, cap_path, sizeof(cap_path));
    PM_CHECK("capture write returns 0", rc == 0);
    PM_CHECK("capsule validates", postmortem_capsule_validate(cap_path));

    char manifest_path[576];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json",
             cap_path);
    PM_CHECK("manifest records signal",
             file_contains(manifest_path, "\"crash_signal\": 11"));
    PM_CHECK("manifest records reason",
             file_contains(manifest_path, "\"reason\": \"unit-test\""));

    char copied_log_path[576];
    snprintf(copied_log_path, sizeof(copied_log_path), "%s/log.txt",
             cap_path);
    PM_CHECK("log tail copied",
             file_contains(copied_log_path, "fatal breadcrumb"));

    seed_tape_t *loaded = postmortem_capsule_load_tape(cap_path);
    PM_CHECK("load tape from capsule", loaded != NULL);
    if (loaded) {
        PM_CHECK("loaded tape preserves inject count",
                 seed_tape_inject_count(loaded) == 1);
        seed_tape_close(loaded);
    }

    char cap_path_old[512];
    opts.crash_unix = 1779665001;
    opts.crash_signal = 6;
    opts.reason = "older";
    rc = postmortem_capture_write(&opts, cap_path_old, sizeof(cap_path_old));
    PM_CHECK("older capture write returns 0", rc == 0);

    char cap_path_new[512];
    opts.crash_unix = 1779665999;
    opts.crash_signal = 8;
    opts.reason = "newer";
    rc = postmortem_capture_write(&opts, cap_path_new, sizeof(cap_path_new));
    PM_CHECK("newer capture write returns 0", rc == 0);

    struct postmortem_capsule_entry entries[2];
    size_t count = 0;
    rc = postmortem_capsule_list(dir, entries, 2, &count);
    PM_CHECK("list returns 0", rc == 0);
    PM_CHECK("list sees three capsules", count == 3);
    PM_CHECK("list returns newest first within cap",
             count >= 3 &&
             entries[0].crash_unix == 1779665999 &&
             entries[1].crash_unix == 1779665123);
    PM_CHECK("list parses manifest summary",
             entries[0].crash_signal == 8 &&
             entries[0].tape_size_bytes == seed_tape_size_bytes(tape));

    char tape_path[576];
    snprintf(tape_path, sizeof(tape_path), "%s/tape.bin", cap_path);
    int fd = open(tape_path, O_RDWR);
    PM_CHECK("open tape for corruption", fd >= 0);
    if (fd >= 0) {
        unsigned char b = 0;
        ssize_t got = pread(fd, &b, 1, 40);
        b ^= 0xff;
        ssize_t wrote = pwrite(fd, &b, 1, 40);
        close(fd);
        PM_CHECK("corrupt tape byte", got == 1 && wrote == 1);
        PM_CHECK("corrupt capsule rejected",
                 !postmortem_capsule_validate(cap_path));
    }

    PM_CHECK("NULL opts rejected",
             postmortem_capture_write(NULL, NULL, 0) == -EINVAL);

    seed_tape_close(tape);
    rm_rf_simple(dir);

    if (failures == 0)
        printf("postmortem: 6 passed, 0 failed\n");
    else
        printf("postmortem: failures=%d\n", failures);
    return failures;
}
