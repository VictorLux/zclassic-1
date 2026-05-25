/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the Phase 6c chaos scenario parser skeleton.
 */

#include "test/test_helpers.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHAOS_NO_MAIN
#include "../../../tools/sim/chaos.c"

#define CHAOS_CHECK(name, expr) do { \
    printf("chaos_harness: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int write_temp_scenario(const char *body, char *path, size_t path_cap)
{
    int n = snprintf(path, path_cap, "/tmp/zcl_chaos_harness_%d_XXXXXX",
                     (int)getpid());
    if (n < 0 || (size_t)n >= path_cap) return -1;
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        unlink(path);
        return -1;
    }
    size_t len = strlen(body);
    size_t wrote = fwrite(body, 1, len, fp);
    int close_rc = fclose(fp);
    if (wrote != len || close_rc != 0) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int run_temp_scenario(const char *body, struct chaos_ctx *ctx_out)
{
    char path[128];
    if (write_temp_scenario(body, path, sizeof(path)) != 0)
        return 99;

    struct chaos_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scenario_path = path;
    snprintf(ctx.boot_phase, sizeof(ctx.boot_phase), "idb_complete");

    int rc = run_scenario(&ctx);
    if (ctx_out) *ctx_out = ctx;
    unlink(path);
    return rc;
}

int test_chaos_harness(void)
{
    printf("\n=== chaos_harness tests ===\n");
    int failures = 0;

    struct chaos_ctx ctx;
    int rc = run_temp_scenario(
        "# valid smoke\n"
        "seed 0x2a\n"
        "boot_phase listening\n"
        "peer_count 3\n"
        "expect no_crash\n"
        "expect tip_height >= 0\n"
        "expect reorg_count == 0\n",
        &ctx);
    CHAOS_CHECK("valid scenario passes", rc == 0);
    CHAOS_CHECK("valid scenario records seed",
                ctx.seed_set && ctx.seed == 0x2a);
    CHAOS_CHECK("valid scenario records boot phase",
                strcmp(ctx.boot_phase, "listening") == 0);
    CHAOS_CHECK("valid scenario records peers", ctx.peer_count == 3);
    CHAOS_CHECK("valid scenario counts expects", ctx.expect_count == 3);

    rc = run_temp_scenario(
        "# comments only\n"
        "\n"
        "   # also ignored\n",
        NULL);
    CHAOS_CHECK("comment-only scenario fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "unknown_command yes\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("unknown command fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "kill_peer 0\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("recognized stub command fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "expect tip_height > 0\n",
        NULL);
    CHAOS_CHECK("failing expect fails scenario", rc != 0);

    rc = run_temp_scenario(
        "seed not-a-number\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("bad seed fails", rc != 0);

    if (failures == 0)
        printf("=== chaos_harness tests: ALL PASS ===\n\n");
    else
        printf("chaos_harness: failures=%d\n", failures);
    return failures;
}
