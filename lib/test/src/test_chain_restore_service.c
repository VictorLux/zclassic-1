/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for chain_restore_service — planning pattern tests.
 * Each test exercises the pure plan() function with struct inputs. */

#include "test/test_helpers.h"
#include "services/chain_restore_service.h"
#include "validation/main_state.h"
#include "chain/chain.h"
#include <string.h>
#include "util/safe_alloc.h"

/* ── Plan tests ────────────────────────────────────────────────── */

static int test_plan_hash_found_in_map(void) {
    int failures = 0;
    TEST("chain_restore_plan: hash found in block_map sets chain tip") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = true;
        in.found_height = 500000;
        in.source = CHAIN_RESTORE_SRC_LDB_IMPORT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_FOUND_IN_INDEX);
        ASSERT(plan.should_create_anchor == false);
        ASSERT(plan.should_set_chain_tip == true);
        ASSERT(plan.should_set_best_header == true);
        ASSERT(plan.should_skip_activate == true);
        ASSERT(plan.anchor_height == 500000);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_hash_not_found_with_height(void) {
    int failures = 0;
    TEST("chain_restore_plan: hash not found, utxo height → create anchor") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 3072280;
        in.source = CHAIN_RESTORE_SRC_LDB_IMPORT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_ANCHOR_CREATED);
        ASSERT(plan.should_create_anchor == true);
        ASSERT(plan.should_set_chain_tip == true);
        ASSERT(plan.should_set_best_header == true);
        ASSERT(plan.should_set_snapshot_anchor == true);
        ASSERT(plan.should_skip_activate == true);
        ASSERT(plan.anchor_height == 3072280);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_null_hash(void) {
    int failures = 0;
    TEST("chain_restore_plan: null hash → FAILED") {
        struct chain_restore_input in = {0};
        /* coins_best_hash is all zeros (null) */

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_FAILED);
        ASSERT(plan.should_create_anchor == false);
        ASSERT(plan.should_skip_activate == true);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_no_utxos(void) {
    int failures = 0;
    TEST("chain_restore_plan: hash not found, no UTXOs → FAILED") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 0;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_FAILED);
        ASSERT(plan.should_skip_activate == true);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_snapshot_source(void) {
    int failures = 0;
    TEST("chain_restore_plan: snapshot source creates anchor with reason") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 100000;
        in.source = CHAIN_RESTORE_SRC_SNAPSHOT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_ANCHOR_CREATED);
        ASSERT(plan.should_create_anchor == true);
        /* reason should mention "snapshot" */
        ASSERT(strstr(plan.reason, "snapshot") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Execution tests ───────────────────────────────────────────── */

static int test_execute_anchor_creation(void) {
    int failures = 0;
    TEST("chain_restore_execute: creates anchor in block_map") {
        struct main_state ms;
        main_state_init(&ms);

        struct uint256 hash;
        uint256_set_hex(&hash, "0000abcdef1234567890");

        struct block_index *anchor = chain_restore_create_anchor(
            &ms, &hash, 500000);

        ASSERT(anchor != NULL);
        ASSERT(anchor->nHeight == 500000);
        ASSERT(anchor->nStatus & BLOCK_VALID_TREE);
        ASSERT(anchor->nStatus & BLOCK_HAVE_DATA);
        ASSERT(anchor->nChainTx == 1);
        ASSERT(anchor->phashBlock != NULL);

        /* Verify findable in block_map */
        struct block_index *found = block_map_find(&ms.map_block_index, &hash);
        ASSERT(found == anchor);

        /* Verify chain work > 0 */
        struct arith_uint256 zero;
        arith_uint256_set_u64(&zero, 0);
        ASSERT(arith_uint256_compare(&anchor->nChainWork, &zero) > 0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

static int test_execute_sets_chain_tip(void) {
    int failures = 0;
    TEST("chain_restore_execute: sets chain tip from plan") {
        struct main_state ms;
        main_state_init(&ms);

        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcdef1234567890");
        in.hash_found_in_map = false;
        in.utxo_max_height = 500000;
        in.source = CHAIN_RESTORE_SRC_NORMAL_BOOT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        struct block_index *result = chain_restore_execute(&plan, &ms);
        ASSERT(result != NULL);

        struct block_index *tip = active_chain_tip(&ms.chain_active);
        ASSERT(tip == result);
        ASSERT(tip->nHeight == 500000);
        ASSERT(ms.pindex_best_header == result);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

static int test_execute_found_in_index(void) {
    int failures = 0;
    TEST("chain_restore_execute: found in index sets tip without creating anchor") {
        struct main_state ms;
        main_state_init(&ms);

        /* Pre-insert a block_index */
        struct uint256 hash;
        uint256_set_hex(&hash, "0000abcdef1234567890");
        struct block_index *existing = zcl_calloc(1, sizeof(struct block_index), "test_block_index");
        block_index_init(existing);
        existing->nHeight = 300000;
        existing->nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA;
        block_map_insert(&ms.map_block_index, &hash, existing);
        existing->phashBlock = block_map_find_hash(&ms.map_block_index, &hash);

        struct chain_restore_input in = {0};
        in.coins_best_hash = hash;
        in.hash_found_in_map = true;
        in.found_height = 300000;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        struct block_index *result = chain_restore_execute(&plan, &ms);
        ASSERT(result == existing);

        struct block_index *tip = active_chain_tip(&ms.chain_active);
        ASSERT(tip == existing);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Validation tests ──────────────────────────────────────────── */

static int test_validate_after_anchor(void) {
    int failures = 0;
    TEST("chain_restore_validate: all checks pass after anchor creation") {
        struct main_state ms;
        main_state_init(&ms);

        struct uint256 hash;
        uint256_set_hex(&hash, "0000abcdef1234567890");

        struct chain_restore_input in = {0};
        in.coins_best_hash = hash;
        in.hash_found_in_map = false;
        in.utxo_max_height = 500000;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);
        chain_restore_execute(&plan, &ms);

        struct chain_restore_validation val;
        chain_restore_validate(&val, &ms, &hash, 500000);

        ASSERT(val.coins_hash_valid == true);
        ASSERT(val.anchor_in_map == true);
        ASSERT(val.chain_tip_set == true);
        ASSERT(val.tip_matches_expected == true);
        ASSERT(val.all_ok == true);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Activation decision tests ─────────────────────────────────── */

static int test_activation_normal_boot(void) {
    int failures = 0;
    TEST("boot_should_activate: normal boot with UTXOs → activate") {
        struct activation_decision dec;
        boot_should_activate_chain(&dec, 500000, 1000000, 600000,
                                   false, false);
        ASSERT(dec.should_activate == true);
        ASSERT(dec.reason == ACTIVATE_OK);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_legacy_import(void) {
    int failures = 0;
    TEST("boot_should_activate: legacy import → skip") {
        struct activation_decision dec;
        boot_should_activate_chain(&dec, 0, 0, 0, true, false);
        ASSERT(dec.should_activate == false);
        ASSERT(dec.reason == ACTIVATE_SKIP_LEGACY_IMPORT);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_anchor_created(void) {
    int failures = 0;
    TEST("boot_should_activate: anchor created → skip") {
        struct activation_decision dec;
        boot_should_activate_chain(&dec, 3072280, 1000000, 600000,
                                   false, true);
        ASSERT(dec.should_activate == false);
        ASSERT(dec.reason == ACTIVATE_SKIP_ANCHOR_CREATED);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_no_utxos_many_headers(void) {
    int failures = 0;
    TEST("boot_should_activate: no UTXOs, many headers → skip (awaiting snapshot)") {
        struct activation_decision dec;
        boot_should_activate_chain(&dec, 0, 50, 100000,
                                   false, false);
        ASSERT(dec.should_activate == false);
        ASSERT(dec.reason == ACTIVATE_SKIP_NO_UTXOS_AWAITING);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_few_utxos_few_headers(void) {
    int failures = 0;
    TEST("boot_should_activate: few UTXOs, few headers → activate (not awaiting)") {
        struct activation_decision dec;
        boot_should_activate_chain(&dec, 0, 50, 10,
                                   false, false);
        ASSERT(dec.should_activate == true);
        ASSERT(dec.reason == ACTIVATE_OK);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Integrity (P14.11 + P14.12) ───────────────────────────────── */

/* Build a clean chain of N linked pindex entries (h=0..N-1) with real
 * nBits and pprev, then set the tip. Integrity must hold. */
static int test_integrity_passes_on_clean_chain(void) {
    int failures = 0;
    TEST("chain_integrity: clean pprev-linked chain passes") {
        struct main_state ms;
        main_state_init(&ms);

        const int N = 20;
        struct uint256 hashes[20];
        for (int h = 0; h < N; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[1] = (uint8_t)((h >> 8) & 0xFF);
            hashes[h].data[3] = 0xAB;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(pi != NULL);
            pi->nHeight  = h;
            pi->nBits    = 0x1f07ffff;
            pi->nTime    = 1000000 + (uint32_t)h * 150;
            pi->nVersion = 4;
            pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            pi->nTx      = 1;
            pi->nChainTx = (uint32_t)(h + 1);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
            if (h > 0) {
                struct block_index *prev = block_map_find(
                    &ms.map_block_index, &hashes[h - 1]);
                if (prev) pi->pprev = prev;
            }
        }
        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[N - 1]);
        ASSERT(active_chain_set_tip(&ms.chain_active, tip));

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.zero_nbits_count == 0);
        ASSERT(r.active_chain_holes == 0);
        ASSERT(r.tip_height == N - 1);
        ASSERT(r.first_nbits_zero_height == -1);
        ASSERT(r.first_hole_height == -1);
        ASSERT(r.ok == true);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* RED: simulate the P14.11 + P14.12 live-node state. A synthetic
 * anchor at h=N with nBits==0 and pprev==NULL, set as tip. The
 * integrity check must detect BOTH:
 *   - anchor pindex with nBits=0 (P14.11 signature),
 *   - active_chain holes from 0..N-1 because anchor->pprev is NULL
 *     so active_chain_set_tip walks into NULL (P14.12 signature). */
static int test_integrity_fails_on_anchor_restore(void) {
    int failures = 0;
    TEST("chain_integrity: synthetic anchor (pprev=NULL, nBits=0) FAILS") {
        struct main_state ms;
        main_state_init(&ms);

        const int H = 1000;
        struct uint256 anchor_hash;
        uint256_set_hex(&anchor_hash, "00000000feedface");
        struct block_index *anchor =
            chain_restore_create_anchor(&ms, &anchor_hash, H);
        ASSERT(anchor != NULL);
        ASSERT(anchor->nHeight == H);
        ASSERT(anchor->nBits == 0);      /* P14.11 shape: nBits unset */
        ASSERT(anchor->pprev == NULL);   /* P14.12 shape: no ancestry */

        ASSERT(active_chain_set_tip(&ms.chain_active, anchor));

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.ok == false);
        ASSERT(r.zero_nbits_count >= 1);
        ASSERT(r.first_nbits_zero_height == H);
        ASSERT(r.active_chain_holes == H);        /* slots 0..H-1 are NULL */
        ASSERT(r.first_hole_height == 0);
        ASSERT(r.tip_height == H);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* Isolate P14.12: a clean pprev-linked chain with ONE nBits=0 entry
 * above genesis must trip the P14.11 limb independently. */
static int test_integrity_detects_isolated_nbits_zero(void) {
    int failures = 0;
    TEST("chain_integrity: single nBits=0 above genesis is detected") {
        struct main_state ms;
        main_state_init(&ms);

        const int N = 5;
        struct uint256 hashes[5];
        for (int h = 0; h < N; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[3] = 0xCD;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            pi->nHeight  = h;
            pi->nBits    = (h == 3) ? 0u : 0x1f07ffffu;   /* poison h=3 */
            pi->nTime    = 1000 + (uint32_t)h * 150;
            pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            if (h > 0) pi->pprev = block_map_find(
                &ms.map_block_index, &hashes[h - 1]);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        }
        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[N - 1]);
        ASSERT(active_chain_set_tip(&ms.chain_active, tip));

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.zero_nbits_count == 1);
        ASSERT(r.first_nbits_zero_height == 3);
        ASSERT(r.active_chain_holes == 0);
        ASSERT(r.ok == false);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Registration ──────────────────────────────────────────────── */

int test_chain_restore_service(void) {
    int failures = 0;
    /* Plan tests */
    failures += test_plan_hash_found_in_map();
    failures += test_plan_hash_not_found_with_height();
    failures += test_plan_null_hash();
    failures += test_plan_no_utxos();
    failures += test_plan_snapshot_source();
    /* Execution tests */
    failures += test_execute_anchor_creation();
    failures += test_execute_sets_chain_tip();
    failures += test_execute_found_in_index();
    /* Validation tests */
    failures += test_validate_after_anchor();
    /* Activation decision tests */
    failures += test_activation_normal_boot();
    failures += test_activation_legacy_import();
    failures += test_activation_anchor_created();
    failures += test_activation_no_utxos_many_headers();
    failures += test_activation_few_utxos_few_headers();
    /* P14.11 + P14.12 integrity-check tests */
    failures += test_integrity_passes_on_clean_chain();
    failures += test_integrity_fails_on_anchor_restore();
    failures += test_integrity_detects_isolated_nbits_zero();
    return failures;
}
