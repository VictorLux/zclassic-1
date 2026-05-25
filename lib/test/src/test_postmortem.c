/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "config/boot.h"
#include "sim/postmortem.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PM_CHECK(name, expr) do { \
    printf("postmortem: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int rm_rf_simple(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char child[768];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        rm_rf_simple(child);
    }
    closedir(d);
    return rmdir(path);
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

static int test_signal_handler_capsule(void)
{
    int failures = 0;
    char dir_template[128];
    snprintf(dir_template, sizeof(dir_template),
             "/tmp/zcl_postmortem_signal_%d_XXXXXX", (int)getpid());
    char *dir = mkdtemp(dir_template);
    PM_CHECK("signal mkdtemp", dir != NULL);
    if (!dir) return failures + 1;

    seed_tape_t *tape = seed_tape_open(0x51616b65ULL, 1779669000);
    PM_CHECK("signal seed tape open", tape != NULL);
    if (tape) {
        seed_tape_advance(tape, 1234);
        seed_tape_inject(tape, 13, "sig", 3);
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (!tape || postmortem_install(tape, dir) != 0)
            _exit(121);
        raise(SIGABRT);
        _exit(122);
    }

    if (pid < 0) {
        PM_CHECK("fork signal child", false);
    } else {
        PM_CHECK("fork signal child", true);
        int status = 0;
        pid_t got = waitpid(pid, &status, 0);
        PM_CHECK("wait signal child", got == pid);
        PM_CHECK("child terminated by SIGABRT",
                 got == pid && WIFSIGNALED(status) &&
                 WTERMSIG(status) == SIGABRT);

        struct postmortem_capsule_entry entries[1];
        size_t count = 0;
        int rc = postmortem_capsule_list(dir, entries, 1, &count);
        PM_CHECK("signal capsule listed", rc == 0 && count == 1);
        PM_CHECK("signal capsule records signal",
                 rc == 0 && count == 1 && entries[0].crash_signal == SIGABRT);
        if (rc == 0 && count == 1) {
            seed_tape_t *loaded = postmortem_capsule_load_tape(entries[0].path);
            PM_CHECK("signal capsule tape loads", loaded != NULL);
            if (loaded) {
                PM_CHECK("signal capsule preserves event",
                         seed_tape_inject_count(loaded) == 1);
                seed_tape_close(loaded);
            }
        }
    }

    seed_tape_close(tape);
    rm_rf_simple(dir);
    return failures;
}

static int test_boot_postmortem_install(void)
{
    int failures = 0;
    char dir_template[128];
    snprintf(dir_template, sizeof(dir_template),
             "/tmp/zcl_boot_postmortem_%d_XXXXXX", (int)getpid());
    char *dir = mkdtemp(dir_template);
    PM_CHECK("boot postmortem mkdtemp", dir != NULL);
    if (!dir) return failures + 1;

    bool ok = boot_postmortem_init_for_testing(dir);
    const char *pm_dir = boot_postmortem_dir_for_testing();
    PM_CHECK("boot postmortem init", ok && pm_dir != NULL);

    pid_t pid = fork();
    if (pid == 0) {
        if (!ok || !pm_dir)
            _exit(121);
        raise(SIGABRT);
        _exit(122);
    }

    if (pid < 0) {
        PM_CHECK("boot postmortem fork", false);
    } else {
        PM_CHECK("boot postmortem fork", true);
        int status = 0;
        pid_t got = waitpid(pid, &status, 0);
        PM_CHECK("boot postmortem wait child", got == pid);
        PM_CHECK("boot postmortem child SIGABRT",
                 got == pid && WIFSIGNALED(status) &&
                 WTERMSIG(status) == SIGABRT);

        struct postmortem_capsule_entry entries[1];
        size_t count = 0;
        int rc = postmortem_capsule_list(pm_dir, entries, 1, &count);
        PM_CHECK("boot postmortem capsule listed", rc == 0 && count == 1);
        PM_CHECK("boot postmortem signal recorded",
                 rc == 0 && count == 1 && entries[0].crash_signal == SIGABRT);
        if (rc == 0 && count == 1) {
            seed_tape_t *loaded = postmortem_capsule_load_tape(entries[0].path);
            PM_CHECK("boot postmortem tape loads", loaded != NULL);
            if (loaded)
                seed_tape_close(loaded);
        }
    }

    boot_postmortem_shutdown_for_testing();
    rm_rf_simple(dir);
    return failures;
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

    struct postmortem_summary summaries[2];
    size_t summary_count = 0;
    rc = postmortem_list(dir, summaries, 2, &summary_count);
    PM_CHECK("summary list returns 0", rc == 0);
    PM_CHECK("summary list mirrors ordering",
             summary_count == 3 &&
             summaries[0].crash_unix == 1779665999 &&
             summaries[1].crash_unix == 1779665123);
    PM_CHECK("summary includes capsule bytes",
             summaries[0].capsule_bytes > summaries[0].tape_size_bytes);

    seed_tape_t *loaded_alias = postmortem_load(cap_path_new);
    PM_CHECK("postmortem_load alias decodes tape", loaded_alias != NULL);
    if (loaded_alias) seed_tape_close(loaded_alias);

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
    failures += test_signal_handler_capsule();
    failures += test_boot_postmortem_install();

    if (failures == 0)
        printf("=== postmortem tests: ALL PASS ===\n\n");
    else
        printf("postmortem: failures=%d\n", failures);
    return failures;
}
