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
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIXTURE_SRC_REL "lib/test/fixtures/raw_sqlite_step_fixture.c"
#define FIXTURE_DST_REL "app/_lint_gate_fixture_tmp.c"
#define COINS_FIXTURE_SRC_REL "lib/test/fixtures/coins_lookup_guard_fixture.c"
#define COINS_FIXTURE_DST_REL "app/controllers/src/_coins_lookup_guard_fixture_tmp.c"
#define OBS_FIXTURE_SRC_REL "lib/test/fixtures/observability_unpaired_stderr_fixture.c"
#define OBS_FIXTURE_DST_REL "app/_observability_lint_fixture_tmp.c"
#define OBS_OK_FIXTURE_SRC_REL "lib/test/fixtures/observability_paired_stderr_fixture.c"
#define OBS_OK_FIXTURE_DST_REL "app/_observability_ok_lint_fixture_tmp.c"
#define RAW_MALLOC_FIXTURE_DST_REL "app/_raw_malloc_lint_fixture_tmp.c"
#define RAW_MALLOC_OK_FIXTURE_DST_REL "app/_raw_malloc_ok_lint_fixture_tmp.c"
#define RAW_MALLOC_SCRIPT_REL "tools/scripts/check_raw_malloc.sh"

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

static bool active_chain_set_tip_file_allowed(const char *path)
{
    return strstr(path, "/app/services/src/chain_tip.c") ||
           strstr(path, "/app/services/src/chain_state_repository.c");
}

static int check_active_chain_set_tip_file(const char *path)
{
    if (active_chain_set_tip_file_allowed(path))
        return 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *hit = strstr(line, "active_chain_set_tip(");
        if (!hit)
            continue;
        char *block_comment = strstr(line, "/*");
        char *star_comment = strstr(line, "*");
        char *line_comment = strstr(line, "//");
        bool comment_only = (block_comment && block_comment < hit) ||
                            (star_comment && star_comment < hit) ||
                            (line_comment && line_comment < hit);
        if (!comment_only) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
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

static int run_check_service_tip_mutation_gate(void)
{
    char services_dir[PATH_MAX];
    if (repo_path(services_dir, sizeof(services_dir), "app/services/src") != 0)
        return -1;
    return walk_c_files(services_dir, check_active_chain_set_tip_file);
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

static int write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t n = strlen(contents);
    int ok = fwrite(contents, 1, n, fp) == n;
    fclose(fp);
    return ok ? 0 : -1;
}

/* Invokes tools/scripts/check_raw_malloc.sh and returns the script's
 * exit status (0 = clean, non-zero = violations). */
static int run_check_raw_malloc_script(void)
{
    char script[PATH_MAX];
    if (repo_path(script, sizeof(script), RAW_MALLOC_SCRIPT_REL) != 0)
        return -1;

    char out_path[PATH_MAX];
    if (repo_path(out_path, sizeof(out_path),
                  "test-tmp/zcl_raw_malloc_lint.out") != 0)
        return -1;

    struct sigaction old_chld;
    struct sigaction dfl_chld;
    int restore_chld = 0;
    memset(&old_chld, 0, sizeof(old_chld));
    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, NULL, &old_chld) == 0 &&
        sigaction(SIGCHLD, &dfl_chld, NULL) == 0) {
        restore_chld = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (pid == 0) {
        int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execl(script, script, (char *)NULL);
        _exit(127);
    }

    int rc = 0;
    while (waitpid(pid, &rc, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (restore_chld)
        (void)sigaction(SIGCHLD, &old_chld, NULL);
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static void unlink_lint_fixtures(void)
{
    const char *fixtures[] = {
        FIXTURE_DST_REL,
        COINS_FIXTURE_DST_REL,
        OBS_FIXTURE_DST_REL,
        OBS_OK_FIXTURE_DST_REL,
        RAW_MALLOC_FIXTURE_DST_REL,
        RAW_MALLOC_OK_FIXTURE_DST_REL,
    };

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        char path[PATH_MAX];
        if (repo_path(path, sizeof(path), fixtures[i]) == 0)
            (void)unlink(path);
    }
}

static int t_raw_malloc_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), RAW_MALLOC_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw_malloc fixture path\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    const char *bad = "/* fixture */\n#include <stdlib.h>\nvoid *f(void){return malloc(16);}\n";
    if (write_file(fixture_dst, bad) != 0) {
        fprintf(stderr, "[lint-gate] could not plant raw_malloc fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_malloc_script();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] raw malloc fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_raw_malloc_zcl_fixture_passes(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), RAW_MALLOC_OK_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw_malloc-ok fixture path\n");
        return 1;
    }
    unlink_lint_fixtures();
    const char *good =
        "/* fixture */\n"
        "#include \"util/safe_alloc.h\"\n"
        "void *f(void){return zcl_malloc(16, \"fixture\");}\n";
    if (write_file(fixture_dst, good) != 0) {
        fprintf(stderr, "[lint-gate] could not plant raw_malloc-ok fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_malloc_script();
    unlink_lint_fixtures();
    TEST("[lint-gate] zcl_malloc-only fixture passes the gate (exit == 0)") {
        ASSERT(rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_raw_malloc_gate_recovers(void)
{
    int failures = 0;
    char fixture_dst1[PATH_MAX];
    char fixture_dst2[PATH_MAX];
    if (repo_path(fixture_dst1, sizeof(fixture_dst1), RAW_MALLOC_FIXTURE_DST_REL) != 0 ||
        repo_path(fixture_dst2, sizeof(fixture_dst2), RAW_MALLOC_OK_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw_malloc fixture paths\n");
        return 1;
    }
    (void)fixture_dst1;
    (void)fixture_dst2;
    unlink_lint_fixtures();
    TEST("[lint-gate] raw_malloc gate passes after fixtures removed") {
        ASSERT(run_check_raw_malloc_script() == 0);
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

static int t_tools_z_mirror_fallback_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("tools/z mirror fallback preserves local authority contract") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/z") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf,
                      "\"consensus_authority\":\"local_consensus_validation\"")
               != NULL);
        ASSERT(strstr(buf, "\"candidate_source\":\"legacy_advisory\"")
               != NULL);
        ASSERT(strstr(buf, "\"candidate_trust\":\"bounded_advisory_fallback\"")
               != NULL);
        ASSERT(strstr(buf,
                      "\"legacy_advisory_gated_by_native_retries\":false")
               != NULL);
        ASSERT(strstr(buf, "mirror_authorization_enabled") == NULL);
        ASSERT(strstr(buf, "mirror_source_trust") == NULL);
        ASSERT(strstr(buf, "\"blockers_total\":0") != NULL);
        ASSERT(strstr(buf, "\"stalls_total\":0") != NULL);
        ASSERT(strstr(buf, "\"unsafe_overrides_total\":0") != NULL);
        ASSERT(strstr(buf, "\"last_override_safe\":false") != NULL);
        ASSERT(strstr(buf, "\"last_override_scope\":\"\"") != NULL);
        ASSERT(strstr(buf, "blockers=%s") != NULL);
        ASSERT(strstr(buf, "stalls=%s") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides=%s") != NULL);
        ASSERT(strstr(buf, "last_override_safe=%s") != NULL);
        ASSERT(strstr(buf, "legacy_advisory_gated=%s") != NULL);
        ASSERT(strstr(buf, "mirror_gated=%s") == NULL);
        ASSERT(strstr(buf, "\"consensus_authority\":\"zclassicd\"") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_service_tip_mutation_gate(void)
{
    int failures = 0;
    TEST("[lint-gate] services do not bypass canonical tip publication") {
        ASSERT(run_check_service_tip_mutation_gate() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_legacy_candidate_source_has_no_override_scope(void)
{
    int failures = 0;
    char *body_pull = NULL;
    char *mirror = NULL;
    TEST("legacy candidate source has no mirror override mutation path") {
        char body_path[PATH_MAX];
        char mirror_path[PATH_MAX];
        ASSERT(repo_path(body_path, sizeof(body_path),
                         "app/services/src/legacy_body_pull.c") == 0);
        ASSERT(repo_path(mirror_path, sizeof(mirror_path),
                         "app/services/src/legacy_mirror_sync_service.c") == 0);
        ASSERT(read_entire_file(body_path, &body_pull) == 0);
        ASSERT(read_entire_file(mirror_path, &mirror) == 0);
        ASSERT(strstr(body_pull, "mirror_consensus_scope_enter") == NULL);
        ASSERT(strstr(body_pull, "mirror_consensus_record_override") == NULL);
        ASSERT(strstr(body_pull, "mirror_consensus_authorize_block") == NULL);
        ASSERT(strstr(body_pull,
                      "process_block_clear_utxo_activation_pause_range") == NULL);
        ASSERT(strstr(mirror, "CSR_ROLLBACK_SOURCE_MIRROR") == NULL);
        ASSERT(strstr(mirror, "chain_set_active_tip(") == NULL);
        ASSERT(strstr(mirror, "active_chain_set_tip(") == NULL);
        PASS();
    } _test_next:;
    free(body_pull);
    free(mirror);
    return failures;
}

static int t_tools_z_operator_diagnostics_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("tools/z exposes canonical operator diagnostics") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/z") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "network|net)") != NULL);
        ASSERT(strstr(buf, "rpc getnetworkinfo") != NULL);
        ASSERT(strstr(buf, "status|health)") != NULL);
        ASSERT(strstr(buf, "rpc healthcheck") != NULL);
        ASSERT(strstr(buf, "advance|chain-advance)") != NULL);
        ASSERT(strstr(buf, "rpc dumpstate chain_advance_coordinator") != NULL);
        ASSERT(strstr(buf, "peerlife|peer-lifecycle)") != NULL);
        ASSERT(strstr(buf, "rpc dumpstate peer_lifecycle") != NULL);
        ASSERT(strstr(buf, "P2P reachability and handshake summary") != NULL);
        ASSERT(strstr(buf,
                      "Chain advance source scoring and selection blockers")
               != NULL);
        ASSERT(strstr(buf,
                      "Peer lifecycle attempts, handshakes, failures by source")
               != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "docs/RUNBOOK.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "sources[].selectable=false") != NULL);
        ASSERT(strstr(buf, "selection_blocker") != NULL);
        ASSERT(strstr(buf, "initialized=true") != NULL);
        ASSERT(strstr(buf, "has_connman=true") != NULL);
        ASSERT(strstr(buf, "has_main_state=true") != NULL);
        ASSERT(strstr(buf, "has_node_db=true") != NULL);
        ASSERT(strstr(buf, "blockers_total") != NULL);
        ASSERT(strstr(buf, "stalls_total") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides_total") != NULL);
        ASSERT(strstr(buf, "last_override_safe") != NULL);
        ASSERT(strstr(buf, "last_override_scope") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "tools/deploy_verify.sh") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "canonical diagnostics ready") != NULL);
        ASSERT(strstr(buf, "chain_advance_coordinator") != NULL);
        ASSERT(strstr(buf, "local_consensus_validation") != NULL);
        ASSERT(strstr(buf, "handshaked_connections") != NULL);
        ASSERT(strstr(buf, "peer_lifecycle") != NULL);
        ASSERT(strstr(buf, "legacy_mirror") != NULL);
        ASSERT(strstr(buf, "blockers_total") != NULL);
        ASSERT(strstr(buf, "stalls_total") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides_total") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides_total 0") != NULL);
        ASSERT(strstr(buf, "last_override_safe") != NULL);
        ASSERT(strstr(buf, "last_override_scope") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_scoreboard_operator_gate_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("tools/scoreboard.sh is the read-only operator gate") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/scoreboard.sh") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "$RPC\" healthcheck") != NULL);
        ASSERT(strstr(buf, "$RPC\" cutoverpreflight -1 -1") != NULL);
        ASSERT(strstr(buf, "rev-parse --short=9 HEAD") != NULL);
        ASSERT(strstr(buf, "VERDICT=LIVE_READY") != NULL);
        ASSERT(strstr(buf, "VERDICT=CUTOVER_READY") != NULL);
        ASSERT(strstr(buf, "VERDICT=LIVE_NOT_READY") != NULL);
        ASSERT(strstr(buf, "VERDICT=CUTOVER_NOT_READY") != NULL);
        ASSERT(strstr(buf, "source_gate") != NULL);
        ASSERT(strstr(buf, "source_dirty") != NULL);
        ASSERT(strstr(buf, "source_tree_dirty") != NULL);
        ASSERT(strstr(buf, "build_matches_source") != NULL);
        ASSERT(strstr(buf, "live_build_not_current") != NULL);
        ASSERT(strstr(buf, "canary_status") != NULL);
        ASSERT(strstr(buf, "canary_failed") != NULL);
        ASSERT(strstr(buf, "elapsed_seconds") != NULL);
        ASSERT(strstr(buf, "cutover_live_gate") != NULL);
        ASSERT(strstr(buf, "cutover_chain_advance_gate") != NULL);
        ASSERT(strstr(buf, "cutover_guard_gate") != NULL);
        ASSERT(strstr(buf, "cutover_header_admit_gate") != NULL);
        ASSERT(strstr(buf, "cutover_validate_headers_gate") != NULL);
        ASSERT(strstr(buf, "operator_needed_blocks") != NULL);
        ASSERT(strstr(buf, "source_ready") != NULL);
        ASSERT(strstr(buf, "chain_advance_not_ready_reason") != NULL);
        ASSERT(strstr(buf, "chain_advance_target_gap") != NULL);
        ASSERT(strstr(buf, "target_height_gap") != NULL);
        ASSERT(strstr(buf, "projection_lag_unknown") != NULL);
        ASSERT(strstr(buf, "not_ready_reason") != NULL);
        ASSERT(strstr(buf, "target_gap") != NULL);
        ASSERT(strstr(buf, "local_height") != NULL);
        ASSERT(strstr(buf, "target_height") != NULL);
        ASSERT(strstr(buf, "projection_lag") != NULL);
        ASSERT(strstr(buf, "projection_ready") != NULL);
        ASSERT(strstr(buf, "projection_gate") != NULL);
        ASSERT(strstr(buf, "diagnostic_only") != NULL);
        ASSERT(strstr(buf, "state_ready") != NULL);
        ASSERT(strstr(buf, "cursor_lag") != NULL);
        ASSERT(strstr(buf, "window_complete") != NULL);
        ASSERT(strstr(buf, "no_failures") != NULL);
        ASSERT(strstr(buf, "failed_total") != NULL);
        ASSERT(strstr(buf, "failure_log_count") != NULL);
        ASSERT(strstr(buf, "first_failed_height") != NULL);
        ASSERT(strstr(buf, "first_fail_reason") != NULL);
        ASSERT(strstr(buf, "last_failed_height") != NULL);
        ASSERT(strstr(buf, "last_fail_reason") != NULL);
        ASSERT(strstr(buf, "error_count") != NULL);
        ASSERT(strstr(buf, "last_blocked_age_seconds") != NULL);
        ASSERT(strstr(buf, "cutovermode") == NULL);
        ASSERT(strstr(buf, "setgenerate") == NULL);
        ASSERT(strstr(buf, "sendtoaddress") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "docs/work/cutover-safety-protocol.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "tools/scoreboard.sh --cutover") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_chain_advance_diagnostics_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot wiring initializes chain advance before diagnostics") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *init = strstr(buf, "chain_advance_coordinator_init(");
        char *diag_state = strstr(buf, "diagnostics_controller_set_state(");
        char *diag_register = strstr(buf, "register_diagnostics_rpc_commands(");
        ASSERT(init != NULL);
        ASSERT(diag_state != NULL);
        ASSERT(diag_register != NULL);
        ASSERT(init < diag_state);
        ASSERT(init < diag_register);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_addrman_persistence_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot uses one sidecar-protected addrman persistence path") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "connman_load_addrman(") != NULL);
        ASSERT(strstr(buf, "addr_db_read(") == NULL);
        ASSERT(strstr(buf, "addr_db_write(") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_shutdown_persistence_order_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("shutdown persists block index after network quiesce") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *network_stop = strstr(buf, "zcl_service_kernel_stop_all(&svc->network_kernel);");
        char *replay_join = strstr(buf, "boot_join_replay_service(svc);");
        char *coins_flushed = strstr(buf, "Coins cache flushed.");
        char *fast = strstr(buf, "shutdown_persist_fast_restart_state(svc);");
        char *connman_join = strstr(buf, "connman_join(svc->connman);");
        ASSERT(network_stop != NULL);
        ASSERT(replay_join != NULL);
        ASSERT(coins_flushed != NULL);
        ASSERT(fast != NULL);
        ASSERT(connman_join != NULL);
        ASSERT(network_stop < fast);
        ASSERT(replay_join < fast);
        ASSERT(coins_flushed < fast);
        ASSERT(fast < connman_join);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_peer_save_busy_reports_db_error(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("peer save lock exhaustion is reported as DB error") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "app/models/src/peer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "event_emitf(EV_DB_ERROR") != NULL);
        ASSERT(strstr(buf, "sqlite3_errstr(rc)") != NULL);
        ASSERT(strstr(buf, "model=peer op=%s rc=%d attempts=%d msg=%s")
               != NULL);
        ASSERT(strstr(buf, "peer %s skipped") != NULL);
        ASSERT(strstr(buf, "event_emitf(EV_MODEL_VALIDATION_FAILED, 0,\n"
                           "                    \"model=peer op=save") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_handshake_peer_save_is_async(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("handshake peer persistence is advisory async write") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msg_version.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "db_service_enqueue_write(dbsvc") != NULL);
        ASSERT(strstr(buf, "db_peer_save_advisory") != NULL);
        ASSERT(strstr(buf, "msg_version.peer_save_ctx") != NULL);
        ASSERT(strstr(buf, "enqueue_queue_full") != NULL);
        ASSERT(strstr(buf, "peer_lifecycle_note_cache_skipped") != NULL);
        ASSERT(strstr(buf, "peer_lifecycle_note_cache_skipped_addr") != NULL);
        ASSERT(strstr(buf, "handshake processing") != NULL);
        ASSERT(strstr(buf, "EV_DB_ERROR") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/peer_lifecycle.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "EV_PEER_CACHE_SKIPPED") != NULL);
        ASSERT(strstr(buf, "\"cache_skipped\"") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_repaired_index_persistence_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot persists repaired canonical block index") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *height_repair = strstr(buf, "block_index_repair_heights(&g_state)");
        char *pprev_repair = strstr(buf, "block_index_repair_pprev(&g_state");
        char *repaired_save = strstr(buf, "Block index repaired: saving canonical flat file");
        char *integrity = strstr(buf, "bii_verify(ctx->datadir");
        ASSERT(height_repair != NULL);
        ASSERT(pprev_repair != NULL);
        ASSERT(repaired_save != NULL);
        ASSERT(integrity != NULL);
        ASSERT(height_repair < repaired_save);
        ASSERT(pprev_repair < repaired_save);
        ASSERT(repaired_save < integrity);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_genesis_init_preserves_restored_authority_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot genesis init preserves restored non-genesis authority tip") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *restore = strstr(buf, "utxo_recovery_restore_chain_tip");
        char *guard = strstr(buf, "boot_restored_authority_tip = true");
        char *skip = strstr(buf, "skipped genesis_init");
        char *genesis = strstr(buf, "\"genesis_init\"");
        char *skip_activate = strstr(buf, "skip_initial_activate");
        ASSERT(restore != NULL);
        ASSERT(guard != NULL);
        ASSERT(skip != NULL);
        ASSERT(genesis != NULL);
        ASSERT(skip_activate != NULL);
        ASSERT(restore < guard);
        ASSERT(guard < genesis);
        ASSERT(skip < genesis);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/utxo_recovery_service.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "coins_best_block is genesis but UTXOs reach")
               != NULL);
        ASSERT(strstr(buf, "utxo_recovery_find_disk_backed_utxo_tip")
               != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_cold_import_fails_closed_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    char *importer_buf = NULL;
    TEST("cold-import failure aborts boot before fallback imports") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &importer_buf) == 0);
        char *cold = strstr(buf, "if (ctx->cold_import_from)");
        char *missing_prereq = strstr(buf, "FATAL: cold-import requested");
        char *bad_source = strstr(importer_buf,
            "[cold_import] source %s does not look like a zclassic");
        char *call = strstr(buf, "LEGACY_BOOTSTRAP_IMPORT_COLD");
        char *fail_closed = strstr(buf, "continue with fallback import paths");
        char *disable_auto = strstr(buf, "ctx->no_legacy_auto_import = true");
        char *ldb_import = strstr(buf, "utxo_recovery_import_ldb(&uctx)");
        ASSERT(cold != NULL);
        ASSERT(missing_prereq != NULL);
        ASSERT(bad_source != NULL);
        ASSERT(call != NULL);
        ASSERT(fail_closed != NULL);
        ASSERT(disable_auto != NULL);
        ASSERT(ldb_import != NULL);
        ASSERT(cold < call);
        ASSERT(call < fail_closed);
        ASSERT(fail_closed < disable_auto);
        ASSERT(disable_auto < ldb_import);
        PASS();
    } _test_next:;
    free(buf);
    free(importer_buf);
    return failures;
}

static int t_cold_import_spotcheck_diagnostics_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("cold-import spotcheck failure reports deterministic digest evidence") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_spotcheck.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *spotcheck = strstr(buf,
            "legacy_bootstrap_spotcheck_sha3_windows(");
        char *debug_check = strstr(buf, "if (debug_env && debug_env[0])");
        char *debug_parse = strstr(buf, "strtoull(debug_window");
        char *debug_verify = debug_check
            ? strstr(debug_check, "legacy_bootstrap_verify_window_logged(")
            : NULL;
        char *random_check = strstr(buf, "SHA3 spotcheck: K=%d");
        char *range = strstr(buf, "(h=%d..%d)");
        char *expected = strstr(buf, "expected=%s actual=%s");
        ASSERT(spotcheck != NULL);
        ASSERT(debug_check != NULL);
        ASSERT(debug_parse != NULL);
        ASSERT(debug_verify != NULL);
        ASSERT(random_check != NULL);
        ASSERT(range != NULL);
        ASSERT(expected != NULL);
        ASSERT(spotcheck < debug_check);
        ASSERT(debug_check < debug_parse);
        ASSERT(debug_parse < debug_verify);
        ASSERT(debug_verify < random_check);
        ASSERT(range < expected);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *debug_env = strstr(buf, "ZCL_COLD_IMPORT_DEBUG_WINDOW");
        char *call = strstr(buf, "legacy_bootstrap_open_block_source(");
        char *required = strstr(buf, "true,  /* require_spotcheck */");
        ASSERT(debug_env != NULL);
        ASSERT(call != NULL);
        ASSERT(required != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        call = strstr(buf, "legacy_bootstrap_spotcheck_sha3_windows(");
        char *refuse = strstr(buf, "refusing to import");
        ASSERT(call != NULL);
        ASSERT(refuse != NULL);
        ASSERT(call < refuse);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_cold_import_uses_leveldb_snapshots_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("cold-import reads legacy LevelDBs through staged snapshots") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *include = strstr(buf, "#include \"storage/ldb_snapshot.h\"");
        char *cold_mode = strstr(buf, "static bool legacy_bootstrap_import_cold");
        ASSERT(cold_mode != NULL);
        char *stage = strstr(buf, "cold_import_ldb_snapshot");
        char *snapshot = strstr(cold_mode,
            "legacy_bootstrap_snapshot_leveldbs(");
        char *cs_probe = strstr(cold_mode,
            "legacy_bootstrap_read_chainstate_best_block(");
        char *height_map = strstr(cold_mode,
            "legacy_bootstrap_load_height_map(");
        char *snapshot_import = strstr(cold_mode,
            "legacy_bootstrap_import_snapshot_state(");
        char *anchor_height = strstr(cold_mode,
            ".anchor_height = legacy_tip");
        char *has_anchor_height = strstr(cold_mode,
            ".has_anchor_height = true");
        char *destroy = strstr(cold_mode, "ldb_snapshot_destroy(idx_dir)");
        ASSERT(include != NULL);
        ASSERT(stage != NULL);
        ASSERT(snapshot != NULL);
        ASSERT(cs_probe != NULL);
        ASSERT(height_map != NULL);
        ASSERT(snapshot_import != NULL);
        ASSERT(anchor_height != NULL);
        ASSERT(has_anchor_height != NULL);
        ASSERT(destroy != NULL);
        ASSERT(snapshot < cs_probe);
        ASSERT(cs_probe < height_map);
        ASSERT(height_map < snapshot_import);
        ASSERT(height_map < anchor_height);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_legacy_chainstate_batches_own_callback_buffers(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("legacy chainstate import batches own txid and script bytes") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "uint8_t (*txids)[32]") != NULL);
        ASSERT(strstr(buf, "uint8_t **scripts") != NULL);
        ASSERT(strstr(buf, "memcpy(c->txids[slot], txid->data, 32)") != NULL);
        ASSERT(strstr(buf, "zcl_malloc(script_len ? script_len : 1") != NULL);
        ASSERT(strstr(buf, "memcpy(script_copy, lc->vouts[i].script") != NULL);
        ASSERT(strstr(buf, ".txid = c->txids[slot]") != NULL);
        ASSERT(strstr(buf, ".script = script_copy") != NULL);
        ASSERT(strstr(buf,
            "legacy_bootstrap_chainstate_clear_batch") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf,
            "legacy_bootstrap_import_snapshot_state(") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/legacy_bootstrap_importer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf,
            "legacy_bootstrap_import_snapshot_state(") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_sha3_window_tool_check_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("gen_sha3_windows supports single-window source proof checks") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/gen_sha3_windows.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *flag = strstr(buf, "--check-window=");
        char *no_write = strstr(buf, "without writing output files");
        char *compare = strstr(buf, "expected=%s actual=%s");
        char *return_mismatch = strstr(buf, "return ok ? 0 : 1");
        ASSERT(flag != NULL);
        ASSERT(no_write != NULL);
        ASSERT(compare != NULL);
        ASSERT(return_mismatch != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "Makefile") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "lib/chain/src/sha3_windows.c") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_block_index_flat_atomic_save_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("block index flat save is atomic") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/block_index_loader.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *tmp = strstr(buf, "block_index.bin\", datadir");
        char *tmp_suffix = strstr(buf, "\"%s.tmp\", path");
        char *unlink_tmp = strstr(buf, "(void)unlink(tmp_path)");
        char *open_tmp = strstr(buf, "fopen(tmp_path, \"wb\")");
        char *rename_tmp = strstr(buf, "rename(tmp_path, path)");
        char *sidecar = strstr(buf, "bii_write_sidecar(datadir)");
        ASSERT(tmp != NULL);
        ASSERT(tmp_suffix != NULL);
        ASSERT(unlink_tmp != NULL);
        ASSERT(open_tmp != NULL);
        ASSERT(rename_tmp != NULL);
        ASSERT(sidecar != NULL);
        ASSERT(tmp_suffix < unlink_tmp);
        ASSERT(unlink_tmp < open_tmp);
        ASSERT(open_tmp < rename_tmp);
        ASSERT(rename_tmp < sidecar);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_projection_deferral_is_not_block_rejected_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("projection deferral is chain advance diagnostic, not block reject") {
        /* WS-6 phase 1 moved connect_tip() out of process_block_core.c
         * into its own file. The projection-deferred contract now lives
         * in connect_tip.c — check there instead. */
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "lib/validation/src/connect_tip.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "chain_advance_coordinator_note_projection_deferred") != NULL);
        ASSERT(strstr(buf, "\"consensus_path\"") != NULL);
        ASSERT(strstr(buf, "projection-deferred-consensus-path") == NULL);
        ASSERT(strstr(buf, "EV_CHAIN_ADVANCE_DECISION") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "app/controllers/src/sync_controller_blocks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "chain_advance_coordinator_note_projection_deferred") != NULL);
        ASSERT(strstr(buf, "\"no_db_service\"") != NULL);
        ASSERT(strstr(buf, "projection-deferred-no-db-service") == NULL);
        ASSERT(strstr(buf, "EV_CHAIN_ADVANCE_DECISION") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/chain_advance_coordinator.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "op=projection_deferred reason=%s") != NULL);
        ASSERT(strstr(buf, "projection_deferred_total") != NULL);
        ASSERT(strstr(buf, "EV_CHAIN_ADVANCE_DECISION") != NULL);
        PASS();
    } _test_next:;
    free(buf);
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
    failures += t_raw_malloc_fixture_trips_gate();
    failures += t_raw_malloc_zcl_fixture_passes();
    failures += t_raw_malloc_gate_recovers();
    failures += t_service_tip_mutation_gate();
    failures += t_legacy_candidate_source_has_no_override_scope();
    failures += t_tools_z_mirror_fallback_contract();
    failures += t_tools_z_operator_diagnostics_contract();
    failures += t_scoreboard_operator_gate_contract();
    failures += t_boot_chain_advance_diagnostics_contract();
    failures += t_boot_addrman_persistence_contract();
    failures += t_boot_shutdown_persistence_order_contract();
    failures += t_peer_save_busy_reports_db_error();
    failures += t_handshake_peer_save_is_async();
    failures += t_boot_repaired_index_persistence_contract();
    failures += t_boot_genesis_init_preserves_restored_authority_contract();
    failures += t_cold_import_fails_closed_contract();
    failures += t_cold_import_spotcheck_diagnostics_contract();
    failures += t_cold_import_uses_leveldb_snapshots_contract();
    failures += t_legacy_chainstate_batches_own_callback_buffers();
    failures += t_sha3_window_tool_check_contract();
    failures += t_block_index_flat_atomic_save_contract();
    failures += t_projection_deferral_is_not_block_rejected_contract();
    return failures;
}

#else  /* !ZCL_TESTING */

int test_make_lint_gates(void)
{
    /* No-op when the lint-gate integration test is disabled. */
    return 0;
}

#endif /* ZCL_TESTING */
