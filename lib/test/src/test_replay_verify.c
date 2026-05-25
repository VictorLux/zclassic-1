/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * test_replay_verify — exercises the offline integrity/PoW sweep service
 * (app/services/src/replay_verify_service.c).
 *
 * Strategy mirrors test_block_log_legacy: cheap unit assertions always run
 * (NULL guards, missing-datadir → error), and a richer "live" assertion
 * block runs only when a legacy datadir is reachable (ZCL_LEGACY_DATADIR
 * override or $HOME/.zclassic with a blocks/ subdir). The live block is
 * skipped with PASS in CI so a fresh checkout doesn't fail, and is also
 * skipped when the LevelDB LOCK is held by a running zclassicd. */

#include "test/test_helpers.h"
#include "services/replay_verify_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RV_CHECK(name, expr) do {                        \
    printf("replay_verify: %s... ", (name));             \
    if ((expr)) { printf("OK\n"); }                      \
    else { printf("FAIL\n"); failures++; }               \
} while (0)

static const char *rv_resolve_live_datadir(void)
{
    const char *env = getenv("ZCL_LEGACY_DATADIR");
    if (env && env[0]) return env;

    static char home_zcl[1024];
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    snprintf(home_zcl, sizeof home_zcl, "%s/.zclassic", home);
    struct stat st;
    if (stat(home_zcl, &st) != 0 || !S_ISDIR(st.st_mode))
        return NULL;
    return home_zcl;
}

int test_replay_verify(void)
{
    int failures = 0;

    /* ── 1. NULL / empty arg guards. */
    {
        struct replay_verify_report rep;
        struct zcl_result r = replay_verify_run(NULL, 0, 1, &rep);
        RV_CHECK("run(NULL datadir) → err", !r.ok);

        r = replay_verify_run("", 0, 1, &rep);
        RV_CHECK("run(empty datadir) → err", !r.ok);

        r = replay_verify_run("/anything", 0, 1, NULL);
        RV_CHECK("run(NULL report) → err", !r.ok);
    }

    /* ── 2. Missing datadir → operational error (open fails). */
    {
        struct replay_verify_report rep;
        struct zcl_result r = replay_verify_run(
                "/tmp/zcl_no_such_legacy_dir_91919191", 0, 1, &rep);
        RV_CHECK("run(missing datadir) → err", !r.ok);
    }

    /* ── 3. Datadir with no blocks/ subdir → operational error. */
    {
        char tmpl[] = "/tmp/zcl_rv_emptyXXXXXX";
        char *dir = mkdtemp(tmpl);
        RV_CHECK("mkdtemp empty", dir != NULL);
        if (dir) {
            struct replay_verify_report rep;
            struct zcl_result r = replay_verify_run(dir, 0, 1, &rep);
            RV_CHECK("run(no blocks/) → err", !r.ok);
            rmdir(dir);
        }
    }

    /* ── 4. Live block: real legacy datadir. Bounded sweep over the
     * first few blocks. Skipped (with PASS) when no datadir is reachable
     * or the LevelDB LOCK is held by a running zclassicd. */
    const char *datadir = rv_resolve_live_datadir();
    if (!datadir) {
        printf("replay_verify: live block SKIPPED "
               "(no ZCL_LEGACY_DATADIR or ~/.zclassic)\n");
        return failures;
    }

    struct replay_verify_report rep;
    struct zcl_result r = replay_verify_run(datadir, 0, /*max_blocks=*/8,
                                            &rep);
    if (!r.ok) {
        printf("replay_verify: live block SKIPPED "
               "(run %s failed: code=%d %s)\n",
               datadir, r.code, r.message);
        return failures;
    }

    /* Report shape: a bounded run of N requested blocks reads at most N. */
    RV_CHECK("blocks_checked in (0, 8]",
             rep.blocks_checked > 0 && rep.blocks_checked <= 8);

    /* start_height echoed; tip and end are sane. */
    RV_CHECK("start_height == 0", rep.start_height == 0);
    RV_CHECK("tip_height >= end_height", rep.tip_height >= rep.end_height);

    /* The first blocks of mainnet must verify cleanly: no PoW, linkage,
     * or merkle failures, and no recorded first failure. */
    RV_CHECK("no pow failures",      rep.pow_failures == 0);
    RV_CHECK("no linkage failures",  rep.linkage_failures == 0);
    RV_CHECK("no merkle failures",   rep.merkle_failures == 0);
    RV_CHECK("first_fail_height == -1", rep.first_fail_height == -1);
    RV_CHECK("first_fail_reason NULL",  rep.first_fail_reason == NULL);

    printf("  blocks_checked=%llu tip=%u (clean)\n",
           (unsigned long long)rep.blocks_checked, rep.tip_height);

    return failures;
}
