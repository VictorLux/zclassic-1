/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Tests for the shadow_feeder_global shim — the indirection that lets
 * lib/net call shadow_feeder_observe_block via a forward extern without
 * including an adapters/ header. If the shim ever NPEs on the unset
 * path the live ingest path crashes, so this test is small but
 * load-bearing. */

#include "test/test_helpers.h"

#include "adapters/inbound/shadow_feeder.h"
#include "adapters/inbound/shadow_feeder_global.h"
#include "consensus/params.h"
#include "mutator/mutator.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SFG_CHECK(name, expr) do {                      \
    printf("shadow_feeder_global: %s... ", (name));     \
    if ((expr)) { printf("OK\n"); }                     \
    else { printf("FAIL\n"); failures++; }              \
} while (0)

static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_sfgbl_XXXXXX");
    if (!mkdtemp(buf)) { perror("mkdtemp"); buf[0] = '\0'; }
}

int test_shadow_feeder_global(void)
{
    int failures = 0;

    /* ── 1. Unset state: every observe call is a safe no-op. */
    SFG_CHECK("unset: is_active==false",
              shadow_feeder_global_is_active() == false);

    /* No-op with NULL block. Must not crash. */
    shadow_feeder_global_observe(0, NULL, NULL, 0);

    /* No-op even with a real block + bytes — there is simply nowhere
     * to deliver. Mimics msg_blocks.c's call shape exactly. */
    {
        struct block b;
        block_init(&b);
        uint8_t fake[8] = { 0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3 };
        shadow_feeder_global_observe(123, &b, fake, sizeof fake);
        block_free(&b);
        SFG_CHECK("unset: live-shape observe is no-op",
                  shadow_feeder_global_is_active() == false);
    }

    /* ── 2. Install a feeder, verify routing flips on. */
    char dir[64];
    make_tmpdir(dir, sizeof dir);

    struct mutator *m = NULL;
    struct mutator_config mc = {
        .queue_capacity = 16,
        .thread_name = "sfgbl-test",
    };
    struct zcl_result r = mutator_start(&mc, &m);
    SFG_CHECK("mutator_start", r.ok && m != NULL);

    struct consensus_params p;
    test_make_easy_consensus_params(&p);

    struct shadow_feeder_config cfg = {
        .shadow_dir = dir,
        .mutator = m,
        .params = &p,
    };
    struct shadow_feeder *f = NULL;
    r = shadow_feeder_create(&cfg, &f);
    SFG_CHECK("shadow_feeder_create", r.ok && f != NULL);

    shadow_feeder_global_set(f);
    SFG_CHECK("after set: is_active==true",
              shadow_feeder_global_is_active() == true);

    /* observe() with NULL block must remain a no-op even with feeder
     * installed (defensive: msg_blocks should never pass NULL, but the
     * contract must hold). */
    unsigned long n0 = shadow_feeder_observed_count(f);
    shadow_feeder_global_observe(0, NULL, NULL, 0);
    SFG_CHECK("NULL block: no observation counted",
              shadow_feeder_observed_count(f) == n0);

    /* ── 3. Set/clear/set cycle preserves identity. */
    shadow_feeder_global_set(NULL);
    SFG_CHECK("after clear: is_active==false",
              shadow_feeder_global_is_active() == false);

    shadow_feeder_global_set(f);
    SFG_CHECK("after re-set: is_active==true",
              shadow_feeder_global_is_active() == true);

    /* ── 4. Phase-2 teardown order: clear first, then destroy. */
    shadow_feeder_global_set(NULL);
    shadow_feeder_destroy(f);
    mutator_stop(m);

    test_rm_rf(dir);

    /* And after disposal the observe path stays a no-op. */
    shadow_feeder_global_observe(0, NULL, NULL, 0);
    SFG_CHECK("post-disposal: is_active==false",
              shadow_feeder_global_is_active() == false);

    return failures;
}
