/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Tests for the shadow pipeline conservation ledger.
 *
 * Closes the named cutover gap: there must be a process-global
 * assertion that every block FED into the shadow pipeline was actually
 * DIFFED against legacy. Without it a silently-dropped block would leave
 * the shadow pipeline looking healthy while diverging.
 *
 * The honest invariant is fed == diffed at quiesce, with `skipped`
 * (mutator-backpressure drops) accounted separately — a skipped block
 * is in the shadow log but was never pushed into the diff path on this
 * pass, so it is not expected on the diffed side.
 *
 * Coverage:
 *   - clean ledger after reset
 *   - feed N blocks through the REAL shadow feeder → fed advances to N,
 *     skipped == 0 (queue has room); record N diffs → fed == diffed,
 *     conservation OK
 *   - backpressure path: a tiny queue forces drops → skipped advances,
 *     fed counts only the successfully-queued blocks, and conservation
 *     still holds once exactly the fed blocks are diffed
 *   - NEGATIVE CONTROL: simulate a dropped diff (fed > diffed) → the
 *     conservation predicate returns FALSE, proving the assert has teeth
 *   - over-count guard: diffed > fed also reports not-conserved
 */

#include "test/test_helpers.h"

#include "adapters/inbound/shadow_conservation.h"
#include "adapters/inbound/shadow_feeder.h"
#include "consensus/params.h"
#include "core/uint256.h"
#include "json/json.h"
#include "mutator/mutator.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "services/cutover_modes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SC_CHECK(name, expr) do {                          \
    printf("shadow_conservation: %s... ", (name));         \
    if ((expr)) { printf("OK\n"); }                        \
    else { printf("FAIL\n"); failures++; }                 \
} while (0)

static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_scons_XXXXXX");
    if (!mkdtemp(buf)) { perror("mkdtemp"); buf[0] = '\0'; }
}

static void rm_rf(const char *dir)
{
    if (!dir || !dir[0]) return;
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)!system(cmd);
}

static void easy_params(struct consensus_params *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 32; i++) p->powLimit.data[i] = 0xff;
}

static void minimal_coinbase(struct transaction *tx)
{
    transaction_init(tx);
    transaction_alloc(tx, 1, 1);
    outpoint_set_null(&tx->vin[0].prevout);
    tx->vin[0].sequence = UINT32_MAX;
    tx->vout[0].value = 1250000000;
    tx->vout[0].script_pub_key.size = 0;
}

/* Build a distinct synthetic coinbase-only block keyed by `seed` so each
 * observe lands at a distinct shadow-log hash. The caller owns `b`. */
static void make_block(struct block *b, uint8_t seed)
{
    block_init(b);
    b->header.nBits = 0x207fffff;
    /* Vary a header field so block_get_hash differs per seed — the
     * shadow log keys on the block hash. nNonce is a uint256. */
    b->header.nNonce.data[0] = seed;
    b->vtx = calloc(1, sizeof(struct transaction));
    b->num_vtx = 1;
    minimal_coinbase(&b->vtx[0]);
}

int test_shadow_conservation(void)
{
    int failures = 0;

    /* ── 1. Clean ledger after reset. */
    {
        shadow_conservation_reset();
        unsigned long fed = 9, diffed = 9, skipped = 9;
        bool ok = shadow_conservation_ok(&fed, &diffed, &skipped);
        SC_CHECK("reset -> fed=0", fed == 0);
        SC_CHECK("reset -> diffed=0", diffed == 0);
        SC_CHECK("reset -> skipped=0", skipped == 0);
        SC_CHECK("reset -> conserved (0==0)", ok);
    }

    /* ── 2. Feed N blocks through the REAL feeder; queue has room so
     *    every observe succeeds → global fed == N, skipped == 0. Then
     *    record N diffs (as the live-shadow diff route would) → the
     *    conservation law fed == diffed holds. */
    {
        shadow_conservation_reset();

        char dir[64]; make_tmpdir(dir, sizeof dir);
        struct mutator *m = NULL;
        struct mutator_config mc = { .queue_capacity = 64,
                                     .thread_name = "scons-ok" };
        mutator_start(&mc, &m);

        struct consensus_params p; easy_params(&p);
        struct shadow_feeder_config cfg = {
            .shadow_dir = dir, .mutator = m, .params = &p,
        };
        struct shadow_feeder *f = NULL;
        shadow_feeder_create(&cfg, &f);

        const unsigned N = 8;
        for (unsigned i = 0; i < N; i++) {
            struct block b; make_block(&b, (uint8_t)(0x10 + i));
            uint8_t bytes[8];
            memset(bytes, (int)(0x10 + i), sizeof bytes);
            struct zcl_result r =
                shadow_feeder_observe_block(f, i, &b, bytes, sizeof bytes);
            (void)r;  /* queue has room; result is OK */
            block_free(&b);
        }

        unsigned long fed = 0, diffed = 0, skipped = 0;
        shadow_conservation_snapshot(&fed, &diffed, &skipped);
        SC_CHECK("fed advanced to N after feeding", fed == N);
        SC_CHECK("skipped == 0 (queue had room)", skipped == 0);
        SC_CHECK("per-handle observed == N",
                 shadow_feeder_observed_count(f) == N);

        /* Transient state: fed > diffed before any diff has run is
         * EXPECTED and is NOT conserved-OK yet. */
        SC_CHECK("transient fed>diffed -> NOT conserved (pre-diff)",
                 !shadow_conservation_ok(&fed, &diffed, &skipped));

        /* The live-shadow diff route now compares exactly the fed range
         * and records checked_count. Quiesced: fed == diffed. */
        shadow_conservation_record_diffed(N);
        bool ok = shadow_conservation_ok(&fed, &diffed, &skipped);
        SC_CHECK("after diffing N -> fed == diffed", fed == diffed);
        SC_CHECK("conserved once quiesced (fed==diffed)", ok);

        shadow_feeder_destroy(f);
        mutator_stop(m);
        rm_rf(dir);
    }

    /* ── 3. Backpressure path through the REAL feeder. A capacity-1
     *    queue against many fast observes MAY force BACKPRESSURE drops
     *    (timing-dependent — the mutator thread can drain between
     *    pushes). Whatever happens, the accounting must be EXACT: every
     *    successfully-pushed block counts as `fed`, every dropped one as
     *    `skipped`, and fed + skipped == N (no block goes unaccounted).
     *    Conservation holds once exactly the fed blocks are diffed — the
     *    skipped ones are not expected on the diffed side. */
    {
        shadow_conservation_reset();

        char dir[64]; make_tmpdir(dir, sizeof dir);
        struct mutator *m = NULL;
        struct mutator_config mc = { .queue_capacity = 1,
                                     .thread_name = "scons-bp" };
        mutator_start(&mc, &m);

        struct consensus_params p; easy_params(&p);
        struct shadow_feeder_config cfg = {
            .shadow_dir = dir, .mutator = m, .params = &p,
        };
        struct shadow_feeder *f = NULL;
        shadow_feeder_create(&cfg, &f);

        const unsigned N = 16;
        unsigned ok_pushes = 0, bp_pushes = 0;
        for (unsigned i = 0; i < N; i++) {
            struct block b; make_block(&b, (uint8_t)(0x80 + i));
            uint8_t bytes[8];
            memset(bytes, (int)(0x80 + i), sizeof bytes);
            struct zcl_result r =
                shadow_feeder_observe_block(f, i, &b, bytes, sizeof bytes);
            if (r.ok) ok_pushes++; else bp_pushes++;
            block_free(&b);
        }

        unsigned long fed = 0, diffed = 0, skipped = 0;
        shadow_conservation_snapshot(&fed, &diffed, &skipped);
        /* The exact split is timing-dependent; the accounting is not. */
        SC_CHECK("fed == successful pushes", fed == ok_pushes);
        SC_CHECK("skipped == backpressure drops", skipped == bp_pushes);
        SC_CHECK("fed + skipped == N (no block unaccounted)",
                 fed + skipped == N);

        /* Diff exactly the fed blocks (the diff route walks the shadow
         * log range; backpressure-dropped blocks ARE in the log too, but
         * the conservation law tracks the validate/diff path, where they
         * were never queued — so we record only the fed count). */
        shadow_conservation_record_diffed(fed);
        bool conserved = shadow_conservation_ok(&fed, &diffed, &skipped);
        SC_CHECK("conserved with backpressure skips accounted",
                 conserved && fed == diffed && skipped == bp_pushes);

        shadow_feeder_destroy(f);
        mutator_stop(m);
        rm_rf(dir);
    }

    /* ── 3b. Deterministic skip accounting: record skips directly (the
     *    feeder calls this on every backpressure drop). A skipped block
     *    must NOT inflate the diffed-side expectation — conservation
     *    stays fed==diffed even with skips outstanding. This pins the
     *    "skipped is separate honest accounting" semantics without
     *    relying on backpressure timing. */
    {
        shadow_conservation_reset();
        shadow_conservation_record_fed(6);
        shadow_conservation_record_skipped(2);   /* 2 dropped to log only */
        shadow_conservation_record_diffed(6);    /* the 6 fed get diffed */

        unsigned long fed = 0, diffed = 0, skipped = 0;
        bool ok = shadow_conservation_ok(&fed, &diffed, &skipped);
        SC_CHECK("skips don't inflate diffed expectation",
                 ok && fed == 6 && diffed == 6 && skipped == 2);
    }

    /* ── 4. NEGATIVE CONTROL: simulate a SILENTLY DROPPED diff. Feed 5,
     *    diff only 4. A dropped block must make the conservation
     *    predicate FALSE — proving the assert has teeth and would catch
     *    a real silent divergence. */
    {
        shadow_conservation_reset();
        shadow_conservation_record_fed(5);
        shadow_conservation_record_diffed(4);   /* one diff "dropped" */

        unsigned long fed = 0, diffed = 0, skipped = 0;
        bool ok = shadow_conservation_ok(&fed, &diffed, &skipped);
        SC_CHECK("negative control: fed=5 diffed=4", fed == 5 && diffed == 4);
        SC_CHECK("DROPPED DIFF -> conservation predicate is FALSE", !ok);

        /* And once the missing diff lands, conservation recovers. */
        shadow_conservation_record_diffed(1);
        SC_CHECK("recovered diff -> conserved again",
                 shadow_conservation_ok(&fed, &diffed, &skipped));
    }

    /* ── 5. Over-count guard: diffed > fed is also NOT conserved
     *    (a diff counting a block that was never fed is just as wrong as
     *    a missing one — the law is strict equality). */
    {
        shadow_conservation_reset();
        shadow_conservation_record_fed(3);
        shadow_conservation_record_diffed(4);
        unsigned long fed = 0, diffed = 0, skipped = 0;
        bool ok = shadow_conservation_ok(&fed, &diffed, &skipped);
        SC_CHECK("diffed>fed -> NOT conserved",
                 !ok && fed == 3 && diffed == 4);
    }

    /* ── 6. zcl_state subsystem=cutover dumper: consolidates per-stage
     *    modes + authoritative_active + canary anchor + conservation into
     *    one read-only, reentrant-safe, non-allocating snapshot. Pin the
     *    object shape and that it reflects the live cutover_modes /
     *    conservation state. */
#ifdef ZCL_TESTING
    {
        cutover_modes_test_reset();
        shadow_conservation_reset();

        /* Authoritative header_admit, shadow validate_headers; record a
         * conservation drop (fed > diffed) so conserved is observably
         * FALSE in the dump. */
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_SHADOW);
        cutover_modes_record_change(100, 100, 101, 1);
        shadow_conservation_record_fed(5);
        shadow_conservation_record_diffed(4);

        struct json_value v;
        json_init(&v);
        json_set_object(&v);
        bool ok = cutover_dump_state_json(&v, NULL);
        SC_CHECK("cutover dump returns true", ok);

        const struct json_value *modes = json_get(&v, "modes");
        SC_CHECK("cutover dump has modes object", modes != NULL);
        if (modes) {
            const char *ha = json_get_str(json_get(modes, "header_admit"));
            const char *vh =
                json_get_str(json_get(modes, "validate_headers"));
            SC_CHECK("modes.header_admit == authoritative",
                     ha && strcmp(ha, "authoritative") == 0);
            SC_CHECK("modes.validate_headers == shadow",
                     vh && strcmp(vh, "shadow") == 0);
        }
        SC_CHECK("authoritative_active == true",
                 json_get_bool(json_get(&v, "authoritative_active")));

        const struct json_value *canary = json_get(&v, "canary");
        SC_CHECK("cutover dump has canary object", canary != NULL);
        if (canary) {
            SC_CHECK("canary.has_change == true",
                     json_get_bool(json_get(canary, "has_change")));
            SC_CHECK("canary.change_height == 100",
                     json_get_int(json_get(canary, "change_height")) == 100);
        }

        const struct json_value *cons = json_get(&v, "conservation");
        SC_CHECK("cutover dump has conservation object", cons != NULL);
        if (cons) {
            SC_CHECK("conservation.fed == 5",
                     json_get_int(json_get(cons, "fed")) == 5);
            SC_CHECK("conservation.diffed == 4",
                     json_get_int(json_get(cons, "diffed")) == 4);
            SC_CHECK("conservation.skipped == 0",
                     json_get_int(json_get(cons, "skipped")) == 0);
            SC_CHECK("conservation.conserved == false (fed>diffed)",
                     !json_get_bool(json_get(cons, "conserved")));
        }
        json_free(&v);

        /* NULL out -> false, no crash. */
        SC_CHECK("cutover dump NULL out -> false",
                 !cutover_dump_state_json(NULL, NULL));

        cutover_modes_test_reset();
    }
#endif

    shadow_conservation_reset();
    return failures;
}
