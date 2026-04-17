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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIXTURE_SRC "lib/test/fixtures/raw_sqlite_step_fixture.c"
#define FIXTURE_DST "app/_lint_gate_fixture_tmp.c"

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

/* Returns the exit status of `make check-raw-sqlite 2>/dev/null >/dev/null`.
 * -1 on spawn failure. */
static int run_check_raw_sqlite(void)
{
    int rc = system("make -s check-raw-sqlite >/dev/null 2>&1");
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -2;
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
    (void)unlink(FIXTURE_DST);
    if (copy_file(FIXTURE_SRC, FIXTURE_DST) != 0) {
        fprintf(stderr, "[lint-gate] could not plant fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_sqlite();
    (void)unlink(FIXTURE_DST);
    TEST("[lint-gate] planted fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_gate_recovers_after_removal(void)
{
    int failures = 0;
    (void)unlink(FIXTURE_DST);
    TEST("[lint-gate] gate passes again after fixture removed") {
        ASSERT(run_check_raw_sqlite() == 0);
        PASS();
    } _test_next:;
    return failures;
}

int test_make_lint_gates(void)
{
    printf("\n=== make_lint_gates tests ===\n");

    /* Skip silently when we're not running from the repo root — the
     * test is meaningless there (make + fixture paths are relative). */
    struct stat st;
    if (stat(FIXTURE_SRC, &st) != 0 || stat("Makefile", &st) != 0) {
        printf("[lint-gate] SKIP: not running from repo root\n");
        return 0;
    }

    int failures = 0;
    failures += t_baseline_passes();
    failures += t_fixture_trips_gate();
    failures += t_gate_recovers_after_removal();
    return failures;
}

#else  /* !ZCL_TESTING */

int test_make_lint_gates(void)
{
    /* No-op when the lint-gate integration test is disabled. */
    return 0;
}

#endif /* ZCL_TESTING */
