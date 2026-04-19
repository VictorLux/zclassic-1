/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * P10.1.1 — Deterministic reproduction of the 2026-04-19 chain stall.
 *
 * Context
 * -------
 * On 2026-04-19 the live node stalled at h=3,081,407 with every
 * connect_tip(3,081,408) returning `bad-txns-BIP30`.  P8.9 shipped a
 * boot-time sweep that ran once and cleaned the orphan coinbase row;
 * the chain advanced to 3,081,408.  Three hours later the tip had
 * regressed to 3,081,407 on its own (no operator restart, no reorg
 * log line) and the BIP30 loop resumed.
 *
 * The failing shape
 * -----------------
 * The coins view holds an unspent coinbase entry for txid X belonging
 * to block N, but the chain tip has dropped back to N-1 (either from
 * a partial-application rollback or from a disconnect→reconnect path
 * that bypasses the P8.9 sweep).  connect_block(block_N) then reads
 * the stale coinbase, finds it still unspent, and trips BIP30 at
 * lib/validation/src/connect_block.c:219-233:
 *
 *     for (size_t i = 0; !skip_bip30 && i < block->num_vtx; i++) {
 *         if (coins_view_cache_have_coins(view, &block->vtx[i].hash)) {
 *             struct coins existing;
 *             coins_init(&existing);
 *             if (coins_view_cache_get_coins(view, &block->vtx[i].hash,
 *                                             &existing)) {
 *                 if (!coins_is_pruned(&existing)) {
 *                     coins_free(&existing);
 *                     return validation_state_dos(state, 100, false,
 *                         REJECT_INVALID, "bad-txns-BIP30", false, NULL);
 *                 }
 *             }
 *             coins_free(&existing);
 *         }
 *     }
 *
 * Scope — what this test is AND is NOT
 * ------------------------------------
 * This row (P10.1.1) ships the REPRODUCTION, not the fix.  The test
 * asserts positively that connect_block trips `bad-txns-BIP30` when
 * the coins view carries a stale unspent coinbase entry for the block
 * being reconnected.  The assertion PASSES today because the bug
 * reproduces deterministically — that passing assertion is the win.
 *
 * The matching negative assertion (invariant: "reconnect after rewind
 * succeeds") lives in P10.1.3 as the RED regression test.  P10.1.2
 * produces the root-cause writeup that names the exact code path and
 * invariant.  P10.1.4 is the minimal fix; P10.1.5 is the live-node
 * canary.
 *
 * Environment
 * -----------
 * All state is in-process and in-memory — no SQLite file, no node
 * boot, no threads.  We build a chain_params copy with a checkpoint
 * covering the test height so check_block's POW + size checks are
 * skipped; g_assume_valid_height is left at -1 so the BIP30 skip flag
 * stays false.  Runtime: <100ms.
 */

#include "test/test_helpers.h"
#include "validation/connect_block.h"
#include "validation/update_coins.h"
#include "validation/contextual_check_tx.h"  /* g_assume_valid_height */
#include "coins/coins_view.h"
#include "coins/coins.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "script/script.h"
#include "bloom/merkle.h"
#include "chain/chainparams.h"
#include "chain/checkpoints.h"
#include "consensus/validation.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers (mirrors test_reorg_safety.c so reviewers can compare) ── */

static struct transaction make_coinbase_seeded(int height, uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "p10_cb_vin");

    uint8_t sig[6];
    sig[0] = 4;
    sig[1] = (uint8_t)(height & 0xFF);
    sig[2] = (uint8_t)((height >> 8) & 0xFF);
    sig[3] = (uint8_t)((height >> 16) & 0xFF);
    sig[4] = (uint8_t)((height >> 24) & 0xFF);
    sig[5] = seed;
    script_set(&tx.vin[0].script_sig, sig, 6);

    uint256_set_null(&tx.vin[0].prevout.hash);
    tx.vin[0].prevout.n = 0xFFFFFFFF;
    tx.vin[0].sequence = 0xFFFFFFFF;

    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "p10_cb_vout");
    tx.vout[0].value = 1000000000LL;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, 3);

    transaction_compute_hash(&tx);
    return tx;
}

static void make_block_seeded(struct block *blk, int height,
                               const struct uint256 *prev_hash,
                               uint8_t seed)
{
    memset(blk, 0, sizeof(*blk));
    blk->num_vtx = 1;
    blk->vtx = zcl_calloc(1, sizeof(struct transaction), "p10_block_vtx");
    blk->vtx[0] = make_coinbase_seeded(height, seed);
    blk->header.nVersion = 4;
    if (prev_hash)
        blk->header.hashPrevBlock = *prev_hash;
    blk->header.nTime = 1000000 + (uint32_t)height * 150 + seed;
    blk->header.hashMerkleRoot =
        compute_merkle_root(&blk->vtx[0].hash, 1);
}

static void free_block(struct block *blk)
{
    for (size_t i = 0; i < blk->num_vtx; i++) {
        free(blk->vtx[i].vin);
        free(blk->vtx[i].vout);
    }
    free(blk->vtx);
}

/* Build a chain_params copy with a single checkpoint at `height`
 * so connect_block's checkpoint_covers() returns true and
 * check_block runs with expensive_checks=false (skips Equihash POW
 * + size limit bounds we don't need for this fixture).  Heap-owned
 * so the caller frees when done. */
struct chain_params_fixture {
    struct chain_params params;
    struct checkpoint_entry entry;
};

static void build_checkpoint_params(struct chain_params_fixture *f,
                                     int checkpoint_height,
                                     const struct uint256 *hash)
{
    f->params = *chain_params_get();
    f->entry.height = checkpoint_height;
    f->entry.hash = *hash;
    f->params.checkpointData.entries = &f->entry;
    f->params.checkpointData.nEntries = 1;
}

/* ── Test 1 — live-node stall shape reproduces ──────────────────── */

static int t_stale_coinbase_trips_bip30(void)
{
    int failures = 0;

    TEST("chain_stall_repro: stale unspent coinbase at tip+1 → bad-txns-BIP30") {
        /* g_assume_valid_height guards the BIP30 skip flag — confirm
         * it is NOT set so the check runs. */
        atomic_store(&g_assume_valid_height, -1);

        /* Heights chosen to match the live-node shape (tip+1 after
         * a partial-application rollback).  Any pair works; using
         * small values keeps the test fast. */
        const int parent_height = 199;
        const int stall_height = parent_height + 1;  /* = 200 */

        /* ── Parent block_index (height=199, the "tip" after rollback) ── */
        struct uint256 parent_hash;
        memset(parent_hash.data, 0xA0, sizeof(parent_hash.data));
        struct block_index parent_idx;
        block_index_init(&parent_idx);
        parent_idx.nHeight = parent_height;
        parent_idx.phashBlock = &parent_hash;
        parent_idx.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        parent_idx.nTx = 1;
        parent_idx.nChainTx = 1;
        arith_uint256_set_u64(&parent_idx.nChainWork, (uint64_t)parent_height + 1);

        /* ── The block that is about to be reconnected at height=200 ── */
        struct block stall_blk;
        make_block_seeded(&stall_blk, stall_height, &parent_hash, 0x00);

        struct uint256 stall_hash;
        block_header_get_hash(&stall_blk.header, &stall_hash);

        struct block_index stall_idx;
        block_index_init(&stall_idx);
        stall_idx.nHeight = stall_height;
        stall_idx.phashBlock = &stall_hash;
        stall_idx.pprev = &parent_idx;
        stall_idx.nStatus = BLOCK_HAVE_DATA;
        stall_idx.nTx = 1;

        /* ── Chain params: add a checkpoint at stall_height so
         *    check_block treats the header as trusted (skip POW/size
         *    limits — we don't mine Equihash for the fixture). ── */
        struct chain_params_fixture fx;
        build_checkpoint_params(&fx, stall_height, &stall_hash);

        /* ── Coins view: seed the stall block's coinbase as UNSPENT,
         *    then set best_block to the PARENT hash — the exact
         *    post-rewind shape the live node was trapped in. ── */
        struct coins_view_cache cache;
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        coins_view_cache_init(&cache, &null_view);

        /* Apply the block's coinbase to the cache — this simulates the
         * partial application of the original block N: the coinbase
         * output landed in the coins view but the tip-update never
         * committed.  After the mystery rollback, the cache still
         * holds this unspent coinbase. */
        update_coins(&stall_blk.vtx[0], &cache, stall_height);

        /* Pin the cache's best_block to the parent — simulating the
         * "tip regressed to N-1" state the live node entered. */
        coins_view_cache_set_best_block(&cache, &parent_hash);

        /* Sanity: the stale coinbase really is present and unspent. */
        ASSERT(coins_view_cache_have_coins(&cache, &stall_blk.vtx[0].hash));
        {
            struct coins existing;
            coins_init(&existing);
            ASSERT(coins_view_cache_get_coins(&cache,
                                               &stall_blk.vtx[0].hash,
                                               &existing));
            ASSERT(!coins_is_pruned(&existing));
            coins_free(&existing);
        }

        /* ── Attempt to reconnect block N — the actual repro call. ── */
        struct validation_state vs;
        validation_state_init(&vs);
        connect_block_set_sapling_tree(NULL);  /* just_check path */

        bool ok = connect_block(&stall_blk, &vs, &stall_idx, &cache,
                                 &fx.params, /*just_check=*/true);

        /* The assertion is the REPRODUCTION: bug is live iff
         * connect_block returns false with reject_reason == "bad-txns-BIP30".
         *
         * When P10.1.4 lands the fix (or the invariant from P10.1.2 is
         * enforced at a higher layer), connect_block should NOT see the
         * stale entry and this assertion will need to flip — at that
         * point the row's repro has served its purpose and the P10.1.3
         * regression test becomes the forward-looking gate. */
        printf("connect_block ok=%d reject=\"%s\" dos=%d at h=%d... ",
               (int)ok, vs.reject_reason, vs.dos, stall_height);

        ASSERT(!ok);
        ASSERT_STR_EQ(vs.reject_reason, "bad-txns-BIP30");
        ASSERT_EQ(vs.reject_code, REJECT_INVALID);
        ASSERT_EQ(vs.dos, 100);

        /* Cleanup */
        free_block(&stall_blk);
        coins_view_cache_free(&cache);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test 2 — clean view does NOT trip BIP30 (control) ──────────── */

static int t_clean_view_advances(void)
{
    int failures = 0;

    TEST("chain_stall_repro: clean view (no stale coinbase) does NOT trip BIP30") {
        atomic_store(&g_assume_valid_height, -1);

        const int parent_height = 199;
        const int stall_height = parent_height + 1;

        struct uint256 parent_hash;
        memset(parent_hash.data, 0xB0, sizeof(parent_hash.data));
        struct block_index parent_idx;
        block_index_init(&parent_idx);
        parent_idx.nHeight = parent_height;
        parent_idx.phashBlock = &parent_hash;
        parent_idx.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        parent_idx.nTx = 1;
        parent_idx.nChainTx = 1;
        arith_uint256_set_u64(&parent_idx.nChainWork, (uint64_t)parent_height + 1);

        struct block blk;
        make_block_seeded(&blk, stall_height, &parent_hash, 0x11);

        struct uint256 blk_hash;
        block_header_get_hash(&blk.header, &blk_hash);

        struct block_index blk_idx;
        block_index_init(&blk_idx);
        blk_idx.nHeight = stall_height;
        blk_idx.phashBlock = &blk_hash;
        blk_idx.pprev = &parent_idx;
        blk_idx.nStatus = BLOCK_HAVE_DATA;
        blk_idx.nTx = 1;

        struct chain_params_fixture fx;
        build_checkpoint_params(&fx, stall_height, &blk_hash);

        struct coins_view_cache cache;
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        coins_view_cache_init(&cache, &null_view);

        /* NO pre-seed of the coinbase — this is the "clean" state
         * that P10.1.4's fix must restore. */
        coins_view_cache_set_best_block(&cache, &parent_hash);

        ASSERT(!coins_view_cache_have_coins(&cache, &blk.vtx[0].hash));

        struct validation_state vs;
        validation_state_init(&vs);
        connect_block_set_sapling_tree(NULL);

        bool ok = connect_block(&blk, &vs, &blk_idx, &cache,
                                 &fx.params, /*just_check=*/true);

        /* With a clean view BIP30 MUST NOT trip.  connect_block may
         * still fail further down (no sapling tree, etc.) but the
         * failure reason MUST NOT be "bad-txns-BIP30". */
        printf("clean: ok=%d reject=\"%s\"... ",
               (int)ok, vs.reject_reason);
        ASSERT(strcmp(vs.reject_reason, "bad-txns-BIP30") != 0);

        free_block(&blk);
        coins_view_cache_free(&cache);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test 3 — P10.1.3 RED regression: disconnect_block purges coinbase ──
 *
 * The invariant (from `docs/postmortems/2026-04-19-bip30-stall.md`,
 * Q3): for every txid T in the coins view, the block that created
 * T's outputs must be on the active chain. Concretely: after
 * `disconnect_block(B)` runs on a scratch view wrapping a parent
 * cache, AND `coins_view_cache_flush(scratch)` propagates the
 * disconnect to the parent, the parent MUST NO LONGER report
 * `coins_view_cache_have_coins` for any tx in B.
 *
 * This test constructs the three-layer shape that production uses
 * inside `disconnect_tip` (`process_block.c:1669-1693`):
 *
 *     null_view  ←  parent   ←  scratch
 *      (stub)    (coins_tip)  (disconnect_tip's scratchpad)
 *
 * `update_coins(blk.vtx[0], parent, h)` seeds the parent with the
 * coinbase (simulating a prior connect_block). Then the scratch is
 * layered on top, `disconnect_block` is called on the scratch, and
 * the scratch is flushed into the parent — exactly the production
 * sequence. The assertion is that the parent no longer has the
 * coinbase.
 *
 * Today this test FAILS. `disconnect_block` at
 * `lib/validation/src/connect_block.c:639` calls
 * `coins_map_erase(&scratch.cache_coins, &tx->hash)` on an empty
 * scratch map — a no-op — so nothing propagates to the parent, and
 * the parent retains the coinbase as an unspent entry. The
 * assertion failure names the bug: the coinbase that belongs to a
 * disconnected block is still reachable via `coins_view_cache_have_coins`
 * on the parent.
 *
 * This is the P10.1.3 RED regression row. P10.1.4's minimal fix
 * (emit a DIRTY+pruned tombstone from disconnect_block instead of
 * a bare erase) flips this assertion from RED to GREEN. After the
 * fix lands, the test stands as the permanent gate against
 * regression. */

static int t_disconnect_block_purges_coinbase_from_backing(void)
{
    int failures = 0;

    TEST("chain_stall_repro P10.1.3 RED: disconnect_block purges coinbase from the backing parent cache") {
        atomic_store(&g_assume_valid_height, -1);

        const int coinbase_height = 200;

        /* Build a stand-in for `coins_tip` — the parent cache.  Its
         * backing is a null view (no SQLite for this test; the
         * scratch→parent layer is enough to surface the bug). */
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        struct coins_view_cache parent;
        coins_view_cache_init(&parent, &null_view);

        /* Block whose coinbase lands in the parent, then gets
         * disconnected via the scratch. */
        struct uint256 parent_prev_hash;
        memset(parent_prev_hash.data, 0xC0, sizeof(parent_prev_hash.data));
        struct block blk;
        make_block_seeded(&blk, coinbase_height, &parent_prev_hash, 0x33);

        struct uint256 blk_hash;
        block_header_get_hash(&blk.header, &blk_hash);

        /* block_index for disconnect_block: nHeight + pprev + phashBlock. */
        struct block_index parent_idx;
        block_index_init(&parent_idx);
        parent_idx.nHeight = coinbase_height - 1;
        parent_idx.phashBlock = &parent_prev_hash;
        parent_idx.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        parent_idx.nTx = 1;
        parent_idx.nChainTx = 1;
        arith_uint256_set_u64(&parent_idx.nChainWork,
                              (uint64_t)coinbase_height);

        struct block_index blk_idx;
        block_index_init(&blk_idx);
        blk_idx.nHeight = coinbase_height;
        blk_idx.phashBlock = &blk_hash;
        blk_idx.pprev = &parent_idx;
        blk_idx.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        blk_idx.nTx = 1;

        /* Seed the parent with the coinbase — simulates a successful
         * prior connect_block at h=coinbase_height. */
        update_coins(&blk.vtx[0], &parent, coinbase_height);
        coins_view_cache_set_best_block(&parent, &blk_hash);

        /* Sanity: parent has the coinbase as unspent. */
        ASSERT(coins_view_cache_have_coins(&parent, &blk.vtx[0].hash));

        /* Now the interesting part: the scratch view wrapping the
         * parent, exactly as `disconnect_tip` builds it at
         * process_block.c:1669-1674. */
        struct coins_view parent_as_view;
        coins_view_cache_as_view(&parent_as_view, &parent);
        struct coins_view_cache scratch;
        coins_view_cache_init(&scratch, &parent_as_view);

        /* disconnect_block on the scratch. empty undo data is fine
         * for a coinbase-only block (the coinbase has no restorable
         * inputs). */
        struct block_undo empty_undo;
        block_undo_init(&empty_undo);
        struct validation_state vs;
        validation_state_init(&vs);
        bool disc_ok = disconnect_block(&blk, &vs, &blk_idx,
                                         &scratch, &empty_undo);
        ASSERT(disc_ok);

        /* Flush the scratch into the parent — the propagation step
         * that, per the postmortem, must purge the coinbase. */
        ASSERT(coins_view_cache_flush(&scratch));

        /* The invariant — TODAY THIS FAILS.  The parent still
         * reports the coinbase as unspent because
         * disconnect_block's coins_map_erase at connect_block.c:639
         * ran on the (empty) scratch map and never emitted a DELETE
         * signal into the parent.
         *
         * P10.1.4 minimal fix: emit a DIRTY+pruned tombstone from
         * disconnect_block so cvc_batch_write propagates a PRUNED
         * entry into the parent, and coins_view_cache_have_coins
         * returns false. When the fix lands this assertion flips
         * from RED to GREEN. */
        if (coins_view_cache_have_coins(&parent, &blk.vtx[0].hash)) {
            printf("FAIL (RED — parent still has coinbase_%d after "
                   "disconnect+flush; invariant violated at "
                   "connect_block.c:639)\n",
                   coinbase_height);
            failures++;
            goto _p103_cleanup;
        }
        PASS();

    _p103_cleanup:
        block_undo_free(&empty_undo);
        coins_view_cache_free(&scratch);
        coins_view_cache_free(&parent);
        free_block(&blk);
    } _test_next:;

    return failures;
}

int test_chain_stall_repro(void);

int test_chain_stall_repro(void)
{
    printf("\n=== P10.1.1 chain stall repro ===\n");
    int failures = 0;
    failures += t_stale_coinbase_trips_bip30();
    failures += t_clean_view_advances();
    failures += t_disconnect_block_purges_coinbase_from_backing();
    return failures;
}
