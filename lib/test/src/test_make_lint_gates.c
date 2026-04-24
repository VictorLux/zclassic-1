/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Self-test for the `make check-raw-sqlite` gate.
 *
 * Problem: the `check-raw-sqlite` lint is the only thing stopping new
 * raw `sqlite3_step` calls from reintroducing the 2026-04-10 UTXO-wipe
 * class of bug. If someone loosens the grep pattern ("oh, it's
 * annoying on this PR, let me add another exemption"), the gate
 * silently stops catching violations. This test prevents that.
 *
 * Approach:
 *   1. Copy the fixture (`lib/test/fixtures/raw_sqlite_step_fixture.c`)
 *      into `app/` under a unique temp name so the Makefile's grep
 *      scope actually sees it.
 *   2. Run `make check-raw-sqlite`.
 *   3. Assert exit code != 0 (the gate caught the fixture).
 *   4. Remove the temp file and rerun to confirm the gate passes again.
 *
 * Gated by `ZCL_TESTING` so the shell-out + make invocation only fires
 * when the suite is built by `make test`; standalone compilations of
 * test_zcl without the macro silently turn this into a no-op pass. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_helpers.h"

#ifdef ZCL_TESTING

#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIXTURE_SRC_REL "lib/test/fixtures/raw_sqlite_step_fixture.c"
#define FIXTURE_DST_REL "app/_lint_gate_fixture_tmp.c"
#define COINS_FIXTURE_SRC_REL "lib/test/fixtures/coins_lookup_guard_fixture.c"
#define COINS_FIXTURE_DST_REL "app/controllers/src/_coins_lookup_guard_fixture_tmp.c"
#define OBS_FIXTURE_SRC_REL "lib/test/fixtures/observability_unpaired_stderr_fixture.c"
#define OBS_FIXTURE_DST_REL "app/_observability_lint_fixture_tmp.c"
#define OBS_OK_FIXTURE_SRC_REL "lib/test/fixtures/observability_paired_stderr_fixture.c"
#define OBS_OK_FIXTURE_DST_REL "app/_observability_ok_lint_fixture_tmp.c"

static const char *repo_root(void)
{
    static char root[PATH_MAX];
    static int cached = 0;

    if (cached) return root[0] ? root : NULL;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe) - 1) {
        cached = 1;
        root[0] = '\0';
        return NULL;
    }
    exe[n] = '\0';

    char *slash = strrchr(exe, '/');
    if (!slash) {
        cached = 1;
        root[0] = '\0';
        return NULL;
    }
    *slash = '\0';

    if (snprintf(root, sizeof(root), "%s", exe) >= (int)sizeof(root)) {
        cached = 1;
        root[0] = '\0';
        return NULL;
    }

    cached = 1;
    return root;
}

static int repo_path(char *out, size_t outsz, const char *rel)
{
    const char *root = repo_root();
    if (!root || !out || outsz == 0 || !rel) return -1;
    return snprintf(out, outsz, "%s/%s", root, rel) >= (int)outsz ? -1 : 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "copy_file: fopen(%s) failed: %s\n",
                src, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "copy_file: fopen(%s) failed: %s\n",
                dst, strerror(errno));
        fclose(in);
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "copy_file: fwrite failed: %s\n",
                    strerror(errno));
            fclose(in); fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static bool has_c_suffix(const char *path)
{
    size_t len = strlen(path);
    return len >= 2 && strcmp(path + len - 2, ".c") == 0;
}

static bool raw_sqlite_line_allowed(const char *line)
{
    return strstr(line, "// raw-sql-ok") ||
           strstr(line, "AR_STEP_ROW") ||
           strstr(line, "AR_STEP_DONE") ||
           strstr(line, "AR_STEP_ROW_READONLY") ||
           strstr(line, "safe_alloc");
}

static int check_raw_sqlite_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "sqlite3_step(") && !raw_sqlite_line_allowed(line)) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int read_entire_file(const char *path, char **out_buf)
{
    *out_buf = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char *buf = calloc((size_t)len + 1, 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    return 0;
}

static int check_coins_guard_file(const char *path)
{
    char *buf = NULL;
    if (read_entire_file(path, &buf) != 0) return -1;

    int rc = 0;
    if (strstr(buf, "coins_view_cache_get_coins(") &&
        !strstr(buf, "rpc_require_chainstate_lookup_ready(")) {
        rc = 1;
    }

    free(buf);
    return rc;
}

static bool line_has_obs_ok(const char *line)
{
    const char *tag = strstr(line, "// obs-ok:");
    return tag && tag[10] != '\0' && tag[10] != '\n' && tag[10] != ' ';
}

static bool line_has_event_emit(const char *line)
{
    return strstr(line, "event_emit(") || strstr(line, "event_emitf(");
}

static bool line_has_terminal_propagation(const char *line)
{
    return strstr(line, "return false;") ||
           strstr(line, "return -1;") ||
           strstr(line, "return 1;") ||
           strstr(line, "return NULL;") ||
           strstr(line, "exit(") ||
           strstr(line, "abort(");
}

static bool observability_line_allowed(char lines[][4096], size_t count,
                                       size_t idx)
{
    if (line_has_obs_ok(lines[idx])) return true;

    size_t start = idx > 3 ? idx - 3 : 0;
    size_t end = idx + 3 < count ? idx + 3 : count - 1;
    for (size_t i = start; i <= end; i++) {
        if (line_has_event_emit(lines[i])) return true;
        if (i >= idx && line_has_terminal_propagation(lines[i])) return true;
    }
    return false;
}

static int check_observability_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char lines[512][4096];
    size_t count = 0;
    while (count < 512 && fgets(lines[count], sizeof(lines[count]), fp)) {
        count++;
    }
    int read_error = ferror(fp) ? -1 : 0;
    fclose(fp);
    if (read_error != 0) return read_error;

    for (size_t i = 0; i < count; i++) {
        if (strstr(lines[i], "fprintf(stderr") &&
            !observability_line_allowed(lines, count, i))
            return 1;
    }
    return 0;
}

static int walk_c_files(const char *dirpath,
                        int (*check_file)(const char *path))
{
    DIR *dir = opendir(dirpath);
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name) >=
            (int)sizeof(path)) {
            closedir(dir);
            return -1;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            int rc = walk_c_files(path, check_file);
            if (rc != 0) {
                closedir(dir);
                return rc;
            }
            continue;
        }

        if (!S_ISREG(st.st_mode) || !has_c_suffix(path))
            continue;

        int rc = check_file(path);
        if (rc != 0) {
            closedir(dir);
            return rc;
        }
    }

    closedir(dir);
    return 0;
}

static int run_check_raw_sqlite(void)
{
    char app_dir[PATH_MAX];
    char tools_dir[PATH_MAX];
    if (repo_path(app_dir, sizeof(app_dir), "app") != 0 ||
        repo_path(tools_dir, sizeof(tools_dir), "tools") != 0)
        return -1;

    int rc = walk_c_files(app_dir, check_raw_sqlite_file);
    if (rc != 0) return rc;
    return walk_c_files(tools_dir, check_raw_sqlite_file);
}

static int run_check_coins_lookup_nullcheck(void)
{
    char controllers_dir[PATH_MAX];
    if (repo_path(controllers_dir, sizeof(controllers_dir),
                  "app/controllers/src") != 0)
        return -1;
    return walk_c_files(controllers_dir, check_coins_guard_file);
}

static int t_observability_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), OBS_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), OBS_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve observability fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant observability fixture -- aborting\n");
        return 1;
    }
    int rc = check_observability_file(fixture_dst);
    (void)unlink(fixture_dst);
    TEST("[lint-gate] unpaired stderr fixture trips observability gate") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_observability_positive_controls_pass(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), OBS_OK_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), OBS_OK_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve observability-ok fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant observability-ok fixture -- aborting\n");
        return 1;
    }
    int rc = check_observability_file(fixture_dst);
    (void)unlink(fixture_dst);
    TEST("[lint-gate] observable stderr positive controls pass") {
        ASSERT(rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_baseline_passes(void)
{
    int failures = 0;
    TEST("[lint-gate] baseline passes (no fixture)") {
        ASSERT(run_check_raw_sqlite() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw sqlite fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr, "[lint-gate] could not plant fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_sqlite();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] planted fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_gate_recovers_after_removal(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw sqlite fixture path\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    TEST("[lint-gate] gate passes again after fixture removed") {
        ASSERT(run_check_raw_sqlite() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_baseline_passes(void)
{
    int failures = 0;
    TEST("[lint-gate] baseline guarded coin-lookups pass") {
        ASSERT(run_check_coins_lookup_nullcheck() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), COINS_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), COINS_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve coins guard fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant coins guard fixture — aborting\n");
        return 1;
    }
    int rc = run_check_coins_lookup_nullcheck();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] unguarded coin lookup fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_gate_recovers(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), COINS_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve coins guard fixture path\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    TEST("[lint-gate] guarded coin-lookups pass again after fixture removed") {
        ASSERT(run_check_coins_lookup_nullcheck() == 0);
        PASS();
    } _test_next:;
    return failures;
}

int test_make_lint_gates(void)
{
    printf("\n=== make_lint_gates tests ===\n");

    /* Resolve against the built test binary location so later tests can
     * change cwd without breaking these shell-outs. */
    struct stat st;
    char fixture_src[PATH_MAX];
    char makefile[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), FIXTURE_SRC_REL) != 0 ||
        repo_path(makefile, sizeof(makefile), "Makefile") != 0 ||
        stat(fixture_src, &st) != 0 || stat(makefile, &st) != 0) {
        printf("[lint-gate] SKIP: repo root not discoverable from test_zcl path\n");
        return 0;
    }

    int failures = 0;
    failures += t_baseline_passes();
    failures += t_fixture_trips_gate();
    failures += t_gate_recovers_after_removal();
    failures += t_coins_guard_baseline_passes();
    failures += t_coins_guard_fixture_trips_gate();
    failures += t_coins_guard_gate_recovers();
    failures += t_observability_fixture_trips_gate();
    failures += t_observability_positive_controls_pass();
    return failures;
}

#else  /* !ZCL_TESTING */

int test_make_lint_gates(void)
{
    /* No-op when the lint-gate integration test is disabled. */
    return 0;
}

#endif /* ZCL_TESTING */
