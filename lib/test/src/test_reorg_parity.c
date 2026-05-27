/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reorg parity proof — the cutover keystone.
 *
 * docs/work/cutover.md §1 PROVE requires a "reorg corpus": prove that a
 * chain REORG produces a byte-identical UTXO/coin state regardless of
 * path. That is, unwinding a lighter branch A and applying a heavier
 * branch B must yield the EXACT SAME coin set as building branch B
 * directly from the fork point — never having seen A at all.
 *
 * This is the disconnect/UTXO-unwind parity proof. The forward-only
 * linear replay (shadow_replay_proof.c) is NOT sufficient: a node that
 * can only roll forward halts on the first mainnet reorg. The new tip
 * path must unwind to byte-exact state, and this test is the offline
 * proof of that invariant before the cutover deletes the legacy
 * disconnect_tip path.
 *
 * Strategy
 * --------
 *   Fork point at height H (genesis chain genesis..H built once, shared).
 *
 *   Branch A: H+1..H+3 coinbases, with H+2 spending H+1's coinbase output.
 *   Branch B: H+1..H+4 coinbases (heavier), with H+2 differently-spending
 *             — it spends the SHARED fork-point coinbase that A never
 *             touched, exercising a divergent spend pattern.
 *
 * Byte-exact comparison primitive
 * --------------------------------
 *   The `utxo_commitment` (XOR-hash accumulator of SHA256(txid||vout||
 *   value||height) over the whole UTXO set, plus a count) is the
 *   canonical byte-level fingerprint of the coin set. Two views with
 *   equal commitments hold byte-identical UTXO sets.
 *
 *   The in-memory incremental accumulator (`cache->commitment`) is
 *   maintained only on the forward path (update_coins add/remove);
 *   disconnect_block does NOT decrement it. Trusting it directly after a
 *   reorg would be path-DEPENDENT (stale). The fix (CLOSED — see below)
 *   is an authoritative QUERY that recomputes from the coin set:
 *   coins_view_cache_recompute_commitment() — the in-memory analogue of
 *   the live node's utxo_commitment_compute_db() (which iterates the
 *   SQLite `utxos` table). This test exercises that query and asserts the
 *   reorged view and the direct-build view converge to an identical
 *   fingerprint, plus per-outpoint presence/value/height equality so a
 *   commitment collision cannot mask a divergence.
 *
 *   BUG (now CLOSED): "cache->commitment is path-dependent across reorgs."
 *   ROOT: disconnect_block never decrements the XOR accumulator, so the
 *   incremental field encodes connect/disconnect HISTORY rather than the
 *   current coin set. FIX (this branch): rather than mutate the validation
 *   hot path (disconnect_block), the authoritative commitment is now a
 *   path-INDEPENDENT recompute over the live coin SET
 *   (coins_view_cache_recompute_commitment in lib/coins). The forward-only
 *   incremental value is unchanged byte-for-byte (same per-UTXO hash
 *   inputs; XOR is commutative), so persisted snapshots stay valid; the
 *   recompute is O(N) on-demand, off the per-block hot path. This test now
 *   ASSERTS path-independence: the queried recompute over the reorged view
 *   equals a from-scratch recompute, and equals the direct-build view.
 *
 *   This mirrors shadow_replay_proof's convergence idiom: the proof
 *   passes only when the two paths converge to an identical fingerprint
 *   (every outpoint that should exist does, and nothing extra leaks).
 *
 * Reuses the block/tx/coinbase builders and disconnect/connect helpers
 * patterned on test_reorg_safety.c (the 50-block fork capstone) — same
 * real consensus paths (disconnect_block / update_coins /
 * update_coins_with_undo), no stubs.
 */

#include "test/test_helpers.h"
#include "validation/connect_block.h"
#include "validation/update_coins.h"
#include "coins/coins_view.h"
#include "coins/coins.h"
#include "coins/utxo_commitment.h"
#include "coins/undo.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "script/script.h"
#include "bloom/merkle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"

/* Pass/fail wrapper consistent with test_reorg_safety.c. */
#define RP_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* ── Builders (patterned on test_reorg_safety.c) ─────────────── */

static struct transaction make_coinbase_seeded(int height, uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "rp_cb_vin");

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
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "rp_cb_vout");
    tx.vout[0].value = 1000000000LL;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, 3);

    transaction_compute_hash(&tx);
    return tx;
}

/* A simple spend of one prior output. Produces one new output for
 * `value`. `seed` makes the txid distinct across branches. */
static struct transaction make_spend(const struct uint256 *prev_txid,
                                     uint32_t prev_n, int64_t value,
                                     uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "rp_sp_vin");
    tx.vin[0].prevout.hash = *prev_txid;
    tx.vin[0].prevout.n = prev_n;
    uint8_t sig[2] = {0x48, seed};
    script_set(&tx.vin[0].script_sig, sig, 2);
    tx.vin[0].sequence = 0xFFFFFFFF;

    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "rp_sp_vout");
    tx.vout[0].value = value;
    uint8_t pk[4] = {0x76, 0xa9, 0x14, seed};
    script_set(&tx.vout[0].script_pub_key, pk, 4);

    transaction_compute_hash(&tx);
    return tx;
}

static void free_tx(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
}

static void free_block(struct block *blk)
{
    for (size_t i = 0; i < blk->num_vtx; i++) {
        free(blk->vtx[i].vin);
        free(blk->vtx[i].vout);
    }
    free(blk->vtx);
    blk->vtx = NULL;
    blk->num_vtx = 0;
}

/* Build a block holding the supplied transactions (caller owns the
 * tx array contents; block takes ownership by copy of the structs,
 * so the caller must NOT free the originals — free via free_block). */
static void make_block_txs(struct block *blk, int height,
                           const struct uint256 *prev_hash,
                           uint8_t seed,
                           struct transaction *txs, size_t ntx)
{
    memset(blk, 0, sizeof(*blk));
    blk->num_vtx = ntx;
    blk->vtx = zcl_calloc(ntx, sizeof(struct transaction), "rp_blk_vtx");
    struct uint256 *leaf_hashes =
        zcl_calloc(ntx, sizeof(struct uint256), "rp_blk_leaves");
    for (size_t i = 0; i < ntx; i++) {
        blk->vtx[i] = txs[i];
        leaf_hashes[i] = txs[i].hash;
    }
    blk->header.nVersion = 4;
    if (prev_hash)
        blk->header.hashPrevBlock = *prev_hash;
    blk->header.nTime = 1000000 + (uint32_t)height * 150 + seed;
    blk->header.hashMerkleRoot = compute_merkle_root(leaf_hashes, ntx);
    free(leaf_hashes);
}

/* Connect every tx in a block via update_coins_with_undo, accumulating
 * undo into the block_undo so disconnect_block can reverse it.
 * Coinbase (vtx[0]) is connected with plain update_coins (no inputs). */
static bool connect_block_with_undo(struct block *blk, int height,
                                    struct coins_view_cache *cache,
                                    struct block_undo *bu)
{
    block_undo_init(bu);
    if (blk->num_vtx > 1)
        block_undo_alloc(bu, blk->num_vtx - 1);

    /* coinbase */
    update_coins(&blk->vtx[0], cache, height);

    /* non-coinbase spends */
    for (size_t i = 1; i < blk->num_vtx; i++) {
        struct tx_undo txundo;
        memset(&txundo, 0, sizeof(txundo));
        if (!update_coins_with_undo(&blk->vtx[i], cache, &txundo, height)) {
            printf("[connect h=%d tx=%zu spend failed] ", height, i);
            return false;
        }
        bu->vtxundo[i - 1] = txundo;
    }
    return true;
}

/* ── Commitment / per-outpoint parity comparison ─────────────── */

/* Recompute a PATH-INDEPENDENT UTXO commitment for `view` by XOR-ing in
 * every still-available output across the supplied candidate txid set.
 *
 * This is the explicit-universe twin of the production query
 * coins_view_cache_recompute_commitment() (which iterates the cache's own
 * live entries). Both derive the fingerprint from the coin SET, not the
 * incremental `view->commitment` accumulator, so both are independent of
 * any reorg history. We keep this universe-driven helper to cross-check the
 * production query against an INDEPENDENT recompute over a known outpoint
 * set: if a cache entry leaked or went missing, the universe-driven count
 * and the cache-iterating count would disagree. */
static void recompute_commitment(struct coins_view_cache *view,
                                 const struct uint256 *txids, size_t ntx,
                                 struct utxo_commitment *out)
{
    utxo_commitment_init(out);
    for (size_t t = 0; t < ntx; t++) {
        struct coins c;
        coins_init(&c);
        if (coins_view_cache_get_coins(view, &txids[t], &c)) {
            for (size_t i = 0; i < c.num_vout; i++) {
                if (coins_is_available(&c, (unsigned)i))
                    utxo_commitment_add(out, txids[t].data, (uint32_t)i,
                                        c.vout[i].value, (int32_t)c.height);
            }
        }
        coins_free(&c);
    }
}

/* For a specific txid: both views agree on presence, and if present, on
 * the full coin contents (coinbase flag, height, each output's value
 * and spend-availability). This guards against an (astronomically
 * unlikely) commitment collision masking a real divergence. */
static bool coins_identical(struct coins_view_cache *x,
                            struct coins_view_cache *y,
                            const struct uint256 *txid)
{
    bool hx = coins_view_cache_have_coins(x, txid);
    bool hy = coins_view_cache_have_coins(y, txid);
    if (hx != hy) return false;
    if (!hx) return true;

    struct coins cx, cy;
    coins_init(&cx);
    coins_init(&cy);
    bool ok = coins_view_cache_get_coins(x, txid, &cx) &&
              coins_view_cache_get_coins(y, txid, &cy);
    if (ok) {
        ok = (cx.is_coinbase == cy.is_coinbase) &&
             (cx.height == cy.height) &&
             (cx.num_vout == cy.num_vout);
        for (size_t i = 0; ok && i < cx.num_vout; i++) {
            bool ax = coins_is_available(&cx, (unsigned)i);
            bool ay = coins_is_available(&cy, (unsigned)i);
            ok = (ax == ay);
            if (ok && ax)
                ok = (cx.vout[i].value == cy.vout[i].value);
        }
    }
    coins_free(&cx);
    coins_free(&cy);
    return ok;
}

/* ── Test implementation ─────────────────────────────────────── */

int test_reorg_parity(void)
{
    printf("\n=== reorg parity test (byte-exact unwind) ===\n");
    int failures = 0;

    /* ── Shared genesis fork point (height 0) ─────────────────── */

    struct transaction g_cb = make_coinbase_seeded(0, 0x00);
    struct block genesis;
    struct uint256 g_hash;
    struct transaction g_txs[1] = { g_cb };
    make_block_txs(&genesis, 0, NULL, 0x00, g_txs, 1);
    block_header_get_hash(&genesis.header, &g_hash);
    struct uint256 g_cb_hash = genesis.vtx[0].hash;

    /* ── Branch A: H+1..H+3 ───────────────────────────────────
     *   A1: coinbase (seed 0x10)
     *   A2: coinbase + spend of A1's coinbase output (n=0)
     *   A3: coinbase
     */
    struct block a_blk[4];   /* index by height: a_blk[1..3] */
    struct uint256 a_hash[4];
    a_hash[0] = g_hash;

    /* A1 */
    {
        struct transaction txs[1] = { make_coinbase_seeded(1, 0x10) };
        make_block_txs(&a_blk[1], 1, &a_hash[0], 0x10, txs, 1);
        block_header_get_hash(&a_blk[1].header, &a_hash[1]);
    }
    struct uint256 a1_cb_hash = a_blk[1].vtx[0].hash;

    /* A2: coinbase + spend A1 coinbase output 0 */
    {
        struct transaction cb = make_coinbase_seeded(2, 0x11);
        struct transaction sp = make_spend(&a1_cb_hash, 0, 900000000LL, 0x1A);
        struct transaction txs[2] = { cb, sp };
        make_block_txs(&a_blk[2], 2, &a_hash[1], 0x11, txs, 2);
        block_header_get_hash(&a_blk[2].header, &a_hash[2]);
    }

    /* A3 */
    {
        struct transaction txs[1] = { make_coinbase_seeded(3, 0x12) };
        make_block_txs(&a_blk[3], 3, &a_hash[2], 0x12, txs, 1);
        block_header_get_hash(&a_blk[3].header, &a_hash[3]);
    }

    /* ── Branch B: H+1..H+4 (heavier, divergent spends) ────────
     *   B1: coinbase (seed 0x20)
     *   B2: coinbase + spend of the SHARED genesis coinbase output
     *       (something branch A never spent — divergent pattern)
     *   B3: coinbase
     *   B4: coinbase
     */
    struct block b_blk[5];   /* index by height: b_blk[1..4] */
    struct uint256 b_hash[5];
    b_hash[0] = g_hash;

    /* B1 */
    {
        struct transaction txs[1] = { make_coinbase_seeded(1, 0x20) };
        make_block_txs(&b_blk[1], 1, &b_hash[0], 0x20, txs, 1);
        block_header_get_hash(&b_blk[1].header, &b_hash[1]);
    }

    /* B2: coinbase + spend genesis coinbase output 0 */
    {
        struct transaction cb = make_coinbase_seeded(2, 0x21);
        struct transaction sp = make_spend(&g_cb_hash, 0, 950000000LL, 0x2B);
        struct transaction txs[2] = { cb, sp };
        make_block_txs(&b_blk[2], 2, &b_hash[1], 0x21, txs, 2);
        block_header_get_hash(&b_blk[2].header, &b_hash[2]);
    }

    /* B3 */
    {
        struct transaction txs[1] = { make_coinbase_seeded(3, 0x22) };
        make_block_txs(&b_blk[3], 3, &b_hash[2], 0x22, txs, 1);
        block_header_get_hash(&b_blk[3].header, &b_hash[3]);
    }

    /* B4 */
    {
        struct transaction txs[1] = { make_coinbase_seeded(4, 0x23) };
        make_block_txs(&b_blk[4], 4, &b_hash[3], 0x23, txs, 1);
        block_header_get_hash(&b_blk[4].header, &b_hash[4]);
    }

    /* block_index records needed by disconnect_block (height + pprev). */
    struct block_index gi;
    block_index_init(&gi);
    gi.nHeight = 0;
    gi.phashBlock = &a_hash[0];

    struct block_index a_idx[4];
    for (int h = 1; h <= 3; h++) {
        block_index_init(&a_idx[h]);
        a_idx[h].nHeight = h;
        a_idx[h].phashBlock = &a_hash[h];
        a_idx[h].pprev = (h == 1) ? &gi : &a_idx[h - 1];
    }

    struct block_index b_idx[5];
    for (int h = 1; h <= 4; h++) {
        block_index_init(&b_idx[h]);
        b_idx[h].nHeight = h;
        b_idx[h].phashBlock = &b_hash[h];
        b_idx[h].pprev = (h == 1) ? &gi : &b_idx[h - 1];
    }

    /* ── VIEW 1: build A, then reorg to B ─────────────────────── */

    struct coins_view_cache v_reorg;
    struct coins_view nv1;
    memset(&nv1, 0, sizeof(nv1));
    coins_view_cache_init(&v_reorg, &nv1);
    update_coins(&genesis.vtx[0], &v_reorg, 0);

    struct block_undo a_undo[4];
    bool built_a = true;
    for (int h = 1; h <= 3 && built_a; h++)
        built_a = connect_block_with_undo(&a_blk[h], h, &v_reorg, &a_undo[h]);
    RP_CHECK("parity: branch A connects (with spend in A2)", built_a);

    /* Disconnect A3..A1 (reverse order), reversing each block's undo. */
    bool unwound = true;
    for (int h = 3; h >= 1 && unwound; h--) {
        struct validation_state vs;
        validation_state_init(&vs);
        unwound = disconnect_block(&a_blk[h], &vs, &a_idx[h],
                                   &v_reorg, &a_undo[h]);
    }
    RP_CHECK("parity: branch A fully disconnects back to fork point",
             unwound);

    for (int h = 1; h <= 3; h++)
        block_undo_free(&a_undo[h]);

    /* Connect heavier branch B onto the unwound view. */
    struct block_undo b_undo_r[5];
    bool reapplied = true;
    for (int h = 1; h <= 4 && reapplied; h++)
        reapplied = connect_block_with_undo(&b_blk[h], h, &v_reorg,
                                            &b_undo_r[h]);
    RP_CHECK("parity: branch B connects after reorg", reapplied);

    /* ── VIEW 2: build B directly from genesis, never seeing A ── */

    struct coins_view_cache v_direct;
    struct coins_view nv2;
    memset(&nv2, 0, sizeof(nv2));
    coins_view_cache_init(&v_direct, &nv2);
    update_coins(&genesis.vtx[0], &v_direct, 0);

    struct block_undo b_undo_d[5];
    bool direct = true;
    for (int h = 1; h <= 4 && direct; h++)
        direct = connect_block_with_undo(&b_blk[h], h, &v_direct,
                                         &b_undo_d[h]);
    RP_CHECK("parity: branch B builds directly from fork point", direct);

    /* Candidate txid universe: every coinbase + spend output either path
     * could have created. The recomputed (path-independent) commitment is
     * XOR'd over exactly the still-available outputs among these. */
    struct uint256 universe[16];
    size_t nu = 0;
    universe[nu++] = g_cb_hash;                  /* genesis coinbase */
    universe[nu++] = a1_cb_hash;                 /* A1 coinbase */
    universe[nu++] = a_blk[2].vtx[0].hash;       /* A2 coinbase */
    universe[nu++] = a_blk[2].vtx[1].hash;       /* A2 spend output */
    universe[nu++] = a_blk[3].vtx[0].hash;       /* A3 coinbase */
    for (int h = 1; h <= 4; h++)
        universe[nu++] = b_blk[h].vtx[0].hash;   /* B1..B4 coinbases */
    universe[nu++] = b_blk[2].vtx[1].hash;       /* B2 spend output */

    /* ── CORE PARITY PROOF (byte-exact UTXO set) ───────────────
     * Reorged view (A → unwind → B) must hold a byte-identical coin
     * SET to the direct-built B. Proven via a path-independent
     * recomputed commitment (the live node's authoritative model). */

    struct utxo_commitment c_reorg, c_direct;
    recompute_commitment(&v_reorg, universe, nu, &c_reorg);
    recompute_commitment(&v_direct, universe, nu, &c_direct);

    RP_CHECK("parity: recomputed UTXO commitment matches (byte-exact set)",
             utxo_commitment_equal(&c_reorg, &c_direct));
    RP_CHECK("parity: recomputed UTXO count matches",
             c_reorg.count == c_direct.count);

    /* ── PATH-INDEPENDENCE (BUG CLOSED) ────────────────────────────
     * The production authoritative query coins_view_cache_recompute_
     * commitment() derives the fingerprint from the live coin SET, so it
     * must be path-INDEPENDENT: the reorged view (A → unwind → B) and the
     * direct-built B (never saw A) must query IDENTICAL commitment + count,
     * and each must equal the universe-driven recompute above. This is the
     * fix for the formerly-open bug "cache->commitment path-dependent
     * across reorgs": we never read the stale incremental accumulator for
     * the authoritative answer — we recompute from the set. */
    struct utxo_commitment q_reorg, q_direct;
    coins_view_cache_recompute_commitment(&v_reorg, &q_reorg);
    coins_view_cache_recompute_commitment(&v_direct, &q_direct);

    RP_CHECK("parity: queried commitment is path-independent "
             "(reorg view == direct view)",
             utxo_commitment_equal(&q_reorg, &q_direct));
    RP_CHECK("parity: queried count is path-independent",
             q_reorg.count == q_direct.count);
    /* The production query and the independent universe-driven recompute
     * must agree — neither leaks nor drops a coin. */
    RP_CHECK("parity: production query == universe recompute (reorg view)",
             utxo_commitment_equal(&q_reorg, &c_reorg) &&
             q_reorg.count == c_reorg.count);
    RP_CHECK("parity: production query == universe recompute (direct view)",
             utxo_commitment_equal(&q_direct, &c_direct) &&
             q_direct.count == c_direct.count);

    /* Sanity that the bug was REAL: the stale incremental accumulator on the
     * reorged view differs from the authoritative recompute (it still
     * carries branch-A's unwound entries). The fix is to NEVER trust this
     * field across a reorg — the assertions above use the recompute, which
     * is correct regardless. This is a diagnostic print, not a gate. */
    if (!utxo_commitment_equal(&v_reorg.commitment, &q_reorg)) {
        printf("[confirmed] stale incremental accumulator differs from "
               "authoritative recompute after reorg: incremental.count=%llu "
               "recompute.count=%llu (disconnect_block does not decrement the "
               "accumulator; the query recomputes from the coin set)\n",
               (unsigned long long)v_reorg.commitment.count,
               (unsigned long long)q_reorg.count);
    }

    /* ── FORWARD-ONLY VALUE UNCHANGED (snapshot compatibility) ─────
     * v_direct was built forward-only (no disconnect), so its incremental
     * accumulator IS the authoritative value. The recompute query must
     * return that SAME value byte-for-byte — proving the fix did not alter
     * the forward-only commitment that persisted snapshots are pinned to.
     * (Same per-UTXO hash inputs; XOR is commutative, so set-iteration
     * order is irrelevant.) */
    RP_CHECK("parity: forward-only incremental == recompute (byte-exact)",
             utxo_commitment_equal(&v_direct.commitment, &q_direct) &&
             v_direct.commitment.count == q_direct.count);

    /* Per-outpoint: genesis coinbase (spent in B2 on both paths). */
    RP_CHECK("parity: genesis coinbase identical in both views",
             coins_identical(&v_reorg, &v_direct, &g_cb_hash));

    /* Each B coinbase identical in both views. */
    {
        bool ok = true;
        for (int h = 1; h <= 4; h++)
            ok = ok && coins_identical(&v_reorg, &v_direct,
                                       &b_blk[h].vtx[0].hash);
        RP_CHECK("parity: every branch-B coinbase identical in both views",
                 ok);
    }

    /* B2's spend output identical in both views. */
    RP_CHECK("parity: B2 spend output identical in both views",
             coins_identical(&v_reorg, &v_direct, &b_blk[2].vtx[1].hash));

    /* ── NO LEAK: branch A's outputs must be ABSENT after reorg ──
     * A1/A3 coinbases and A2's spend output must not exist in the
     * reorged view (they were never re-created by B). The genesis and
     * B-chain coverage above proves presence; here we prove absence. */
    {
        bool gone = !coins_view_cache_have_coins(&v_reorg, &a1_cb_hash) &&
                    !coins_view_cache_have_coins(&v_reorg,
                                                 &a_blk[2].vtx[0].hash) &&
                    !coins_view_cache_have_coins(&v_reorg,
                                                 &a_blk[2].vtx[1].hash) &&
                    !coins_view_cache_have_coins(&v_reorg,
                                                 &a_blk[3].vtx[0].hash);
        RP_CHECK("parity: no branch-A coin leaks into reorged view", gone);
        /* Same absence in the direct view (sanity: B never created them). */
        bool gone_d = !coins_view_cache_have_coins(&v_direct, &a1_cb_hash) &&
                      !coins_view_cache_have_coins(&v_direct,
                                                   &a_blk[2].vtx[1].hash);
        RP_CHECK("parity: direct-build view also free of branch-A coins",
                 gone_d);
    }

    /* ── IDEMPOTENT BACK-AND-FORTH: A→B→A→B == direct B ────────
     * Start a fresh reorging view and cycle twice, ending on B.
     * Final state must still equal the direct-built B (no residue). */
    {
        struct coins_view_cache v_cyc;
        struct coins_view nvc;
        memset(&nvc, 0, sizeof(nvc));
        coins_view_cache_init(&v_cyc, &nvc);
        update_coins(&genesis.vtx[0], &v_cyc, 0);

        bool ok = true;
        for (int cycle = 0; cycle < 2 && ok; cycle++) {
            /* Connect A */
            struct block_undo au[4];
            for (int h = 1; h <= 3 && ok; h++)
                ok = connect_block_with_undo(&a_blk[h], h, &v_cyc, &au[h]);
            /* Disconnect A */
            for (int h = 3; h >= 1 && ok; h--) {
                struct validation_state vs;
                validation_state_init(&vs);
                ok = disconnect_block(&a_blk[h], &vs, &a_idx[h],
                                      &v_cyc, &au[h]);
            }
            for (int h = 1; h <= 3; h++)
                block_undo_free(&au[h]);

            /* Connect B */
            struct block_undo bu[5];
            for (int h = 1; h <= 4 && ok; h++)
                ok = connect_block_with_undo(&b_blk[h], h, &v_cyc, &bu[h]);
            /* Disconnect B (unless final cycle: leave B applied) */
            if (cycle < 1) {
                for (int h = 4; h >= 1 && ok; h--) {
                    struct validation_state vs;
                    validation_state_init(&vs);
                    ok = disconnect_block(&b_blk[h], &vs, &b_idx[h],
                                          &v_cyc, &bu[h]);
                }
            }
            for (int h = 1; h <= 4; h++)
                block_undo_free(&bu[h]);
        }
        RP_CHECK("parity: A->B->A->B cycle succeeds", ok);

        /* Recomputed (path-independent) commitment after two full
         * back-and-forth cycles must still equal the direct-built B: no
         * residue accumulates in the actual coin set. */
        struct utxo_commitment c_cyc, c_dir2;
        recompute_commitment(&v_cyc, universe, nu, &c_cyc);
        recompute_commitment(&v_direct, universe, nu, &c_dir2);
        RP_CHECK("parity: cycled recomputed commitment == direct-build",
                 ok && utxo_commitment_equal(&c_cyc, &c_dir2));
        RP_CHECK("parity: cycled recomputed count == direct-build count",
                 ok && c_cyc.count == c_dir2.count);

        /* Same invariant through the PRODUCTION query: after two full
         * A->B->A->B cycles the queried commitment must still equal the
         * direct build — path-independence holds no matter how convoluted
         * the connect/disconnect history. */
        struct utxo_commitment q_cyc, q_dir2;
        coins_view_cache_recompute_commitment(&v_cyc, &q_cyc);
        coins_view_cache_recompute_commitment(&v_direct, &q_dir2);
        RP_CHECK("parity: cycled queried commitment == direct-build "
                 "(path-independent after 2 cycles)",
                 ok && utxo_commitment_equal(&q_cyc, &q_dir2) &&
                 q_cyc.count == q_dir2.count);

        /* And every outpoint in the universe is identical between the
         * cycled view and the direct build. */
        {
            bool same = true;
            for (size_t t = 0; t < nu && same; t++)
                same = coins_identical(&v_cyc, &v_direct, &universe[t]);
            RP_CHECK("parity: every outpoint identical after 2 cycles",
                     ok && same);
        }

        coins_view_cache_free(&v_cyc);
    }

    /* ── Cleanup ──────────────────────────────────────────────── */

    for (int h = 1; h <= 4; h++) {
        block_undo_free(&b_undo_r[h]);
        block_undo_free(&b_undo_d[h]);
    }

    coins_view_cache_free(&v_reorg);
    coins_view_cache_free(&v_direct);

    free_block(&genesis);
    for (int h = 1; h <= 3; h++)
        free_block(&a_blk[h]);
    for (int h = 1; h <= 4; h++)
        free_block(&b_blk[h]);
    (void)free_tx;

    printf("=== reorg parity: %d failures ===\n", failures);
    return failures;
}
