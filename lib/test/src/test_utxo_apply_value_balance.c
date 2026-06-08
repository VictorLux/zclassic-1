/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_utxo_apply_value_balance - focused regression for the per-transaction
 * no-inflation check in utxo_apply_compute_block_delta.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * compute_block_delta's per-tx money rule is the ONLY no-inflation guard on
 * the reducer path a connected block takes (connect_block.c's
 * value_in<value_out check has no production caller). The prior check was
 * transparent-only (tx_output_value > tx_input_value) and therefore
 * false-rejected a legitimate shielded->transparent UNSHIELD: a tx whose
 * transparent outputs exceed its transparent inputs because the shielded pool
 * (value_balance > 0, or a JoinSplit vpub_new) funds the difference. That
 * false rejection froze the live node at height 3,138,977.
 *
 * The fix is the full Zcash money rule:
 *   value_in  = transparent_in + max(0, value_balance) + Σ vpub_new
 *   value_out = transparent_out + max(0,-value_balance) + Σ vpub_old
 *   reject (status "value_overflow") iff !coinbase && value_in < value_out.
 *
 * This test calls compute_block_delta DIRECTLY with hand-built blocks and a
 * trivial in-test lookup, and asserts:
 *   (a) UNSHIELD via value_balance PASSES  (transparent_out > transparent_in,
 *       value_balance = +D funds the gap)            -> out.ok == true
 *   (b) the SAME shape with value_balance = 0 (no shielded funding) is real
 *       inflation and FAILS                          -> out.ok == false,
 *                                                       status "value_overflow"
 *   (c) UNSHIELD via a JoinSplit vpub_new = D PASSES -> out.ok == true
 *
 * Only the per-tx money check is under test; the block is a bare {coinbase,
 * spend} pair (compute_block_delta reads only vtx values + the lookup; it does
 * not touch the header), and the spent input coin is supplied by the lookup.
 */

#include "test/test_helpers.h"

#include "core/uint256.h"
#include "jobs/utxo_apply_delta.h"
#include "jobs/utxo_apply_stage.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <string.h>

#define UAVB_CHECK(name, expr) do {                  \
    printf("utxo_apply_value_balance: %s... ", (name));\
    if ((expr)) printf("OK\n");                       \
    else { printf("FAIL\n"); failures++; }            \
} while (0)

/* The single external input coin every spend tx consumes. The lookup below
 * returns it for the matching outpoint and "not found" for anything else. */
struct uavb_coin {
    struct uint256 txid;
    uint32_t vout;
    int64_t value;
};

static bool uavb_lookup(const struct uint256 *txid, uint32_t vout,
                        struct utxo_apply_lookup *out, void *user)
{
    const struct uavb_coin *c = user;
    memset(out, 0, sizeof(*out));
    if (c && c->vout == vout && uint256_eq(&c->txid, txid)) {
        out->found = true;
        out->value = c->value;
        out->height = 0;
        out->is_coinbase = false;
        out->script_len = 0;     /* no restore script needed for this test */
    }
    return true; /* lookup itself never errors */
}

/* Coinbase vtx[0]: compute_block_delta skips the money check for it, so its
 * value is irrelevant to the rule under test. Distinct hash per `tag`. */
static void uavb_make_coinbase(struct transaction *tx, uint8_t tag)
{
    transaction_init(tx);
    (void)transaction_alloc(tx, 1, 1);
    outpoint_set_null(&tx->vin[0].prevout);
    tx->vout[0].value = 1000000000LL;
    uint8_t pk[3] = { 0x76, 0xa9, tag };
    script_set(&tx->vout[0].script_pub_key, pk, 3);
    uint256_set_null(&tx->hash);
    tx->hash.data[0] = 0xC0;
    tx->hash.data[1] = tag;
}

/* A non-coinbase tx spending `coin` (transparent_in = coin->value) and
 * creating ONE transparent output of `tout` (transparent_out = tout). With
 * tout > coin->value the tx is transparent-inflationary; the shielded funding
 * (value_balance / joinsplit) decides whether the money rule accepts it. */
static void uavb_make_spend(struct transaction *tx, uint8_t tag,
                            const struct uavb_coin *coin, int64_t tout)
{
    transaction_init(tx);
    (void)transaction_alloc(tx, 1, 1);
    tx->vin[0].prevout.hash = coin->txid;
    tx->vin[0].prevout.n = coin->vout;
    tx->vout[0].value = tout;
    uint8_t pk[4] = { 0x76, 0xa9, 0xBB, tag };
    script_set(&tx->vout[0].script_pub_key, pk, 4);
    uint256_set_null(&tx->hash);
    tx->hash.data[0] = 0x5E;
    tx->hash.data[1] = tag;
}

/* Build a {coinbase, spend} block. The caller fills the shielded fields of
 * vtx[1] (value_balance and/or joinsplit) after this returns. */
static bool uavb_build_block(struct block *b, uint8_t tag,
                             const struct uavb_coin *coin, int64_t tout)
{
    block_init(b);
    b->num_vtx = 2;
    b->vtx = zcl_calloc(b->num_vtx, sizeof(struct transaction), "uavb_vtx");
    if (!b->vtx) return false;
    uavb_make_coinbase(&b->vtx[0], tag);
    uavb_make_spend(&b->vtx[1], tag, coin, tout);
    return true;
}

/* Attach one JoinSplit with vpub_new = D to vtx[1] (Sprout->transparent
 * unshield). transaction_free() frees v_joinsplit via plain free(), so a
 * zcl_calloc'd array is released correctly at block_free. */
static bool uavb_add_joinsplit(struct transaction *tx, int64_t vpub_new)
{
    tx->v_joinsplit =
        zcl_calloc(1, sizeof(struct js_description), "uavb_js");
    if (!tx->v_joinsplit) return false;
    tx->num_joinsplit = 1;
    tx->v_joinsplit[0].vpub_old = 0;
    tx->v_joinsplit[0].vpub_new = vpub_new;
    return true;
}

int test_utxo_apply_value_balance(void);
int test_utxo_apply_value_balance(void)
{
    printf("\n=== utxo_apply value-balance money-rule test ===\n");
    int failures = 0;

    /* The input coin every spend consumes: 500,000,000 zat at vout 0. */
    struct uavb_coin coin;
    memset(&coin, 0, sizeof(coin));
    coin.txid.data[0] = 0xE7;
    coin.txid.data[1] = 0x0A;
    coin.vout = 0;
    coin.value = 500000000LL;

    const int64_t D = 1000LL;                 /* unshield amount */
    const int64_t tout = coin.value + D;      /* transparent_out > in by D */

    /* (a) UNSHIELD via value_balance PASSES.
     * transparent_in = coin.value, transparent_out = coin.value + D,
     * value_balance = +D funds the gap => value_in == value_out => accept. */
    {
        struct block b;
        bool built = uavb_build_block(&b, 0xA1, &coin, tout);
        UAVB_CHECK("(a) unshield block builds", built);
        if (built) {
            b.vtx[1].value_balance = +D;      /* shielded pool funds the +D */

            struct delta_summary out;
            utxo_apply_compute_block_delta(&b, 1, uavb_lookup, &coin, &out);
            UAVB_CHECK("(a) unshield with value_balance=+D PASSES (out.ok)",
                       out.ok == true);
            free_delta(&out);
        }
        block_free(&b);
    }

    /* (b) INFLATION FAILS.
     * Same shape, value_balance = 0: transparent_out exceeds transparent_in
     * by D with NO shielded funding => value_in < value_out => reject with
     * status "value_overflow". */
    {
        struct block b;
        bool built = uavb_build_block(&b, 0xB2, &coin, tout);
        UAVB_CHECK("(b) inflation block builds", built);
        if (built) {
            b.vtx[1].value_balance = 0;       /* no shielded funding */

            struct delta_summary out;
            utxo_apply_compute_block_delta(&b, 1, uavb_lookup, &coin, &out);
            UAVB_CHECK("(b) inflation with value_balance=0 FAILS (!out.ok)",
                       out.ok == false);
            UAVB_CHECK("(b) inflation status is value_overflow",
                       out.status != NULL &&
                       strcmp(out.status, "value_overflow") == 0);
            free_delta(&out);
        }
        block_free(&b);
    }

    /* (c) UNSHIELD via JoinSplit vpub_new PASSES.
     * Same transparent shape (transparent_out exceeds in by D), funded by a
     * single JoinSplit with vpub_new = D (value FROM the Sprout pool) and
     * value_balance = 0 => value_in == value_out => accept. */
    {
        struct block b;
        bool built = uavb_build_block(&b, 0xC3, &coin, tout);
        UAVB_CHECK("(c) joinsplit block builds", built);
        if (built) {
            b.vtx[1].value_balance = 0;
            bool js = uavb_add_joinsplit(&b.vtx[1], D);
            UAVB_CHECK("(c) joinsplit attaches", js);
            if (js) {
                struct delta_summary out;
                utxo_apply_compute_block_delta(&b, 1, uavb_lookup, &coin, &out);
                UAVB_CHECK("(c) unshield with joinsplit vpub_new=D PASSES "
                           "(out.ok)", out.ok == true);
                free_delta(&out);
            }
        }
        block_free(&b);
    }

    printf("=== utxo_apply value-balance money-rule: %d failures ===\n",
           failures);
    return failures;
}
