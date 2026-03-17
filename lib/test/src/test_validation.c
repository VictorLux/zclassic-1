/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Validation pipeline tests: check_transaction, check_block, consensus rules. */

#include "test/test_helpers.h"

/* From consensus/consensus.h - avoid re-include due to triple MAX_BLOCK_SIZE definitions */
#ifndef TX_EXPIRY_HEIGHT_THRESHOLD
#define TX_EXPIRY_HEIGHT_THRESHOLD 500000000U
#endif
#ifndef MAX_TX_SIZE_BEFORE_SAPLING
#define MAX_TX_SIZE_BEFORE_SAPLING 100000
#endif

static struct transaction make_simple_tx(void)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.overwintered = false;
    tx.num_vin = 1;
    tx.vin = calloc(1, sizeof(struct tx_in));
    memset(tx.vin[0].prevout.hash.data, 0xAA, 32);
    tx.vin[0].prevout.n = 0;
    uint8_t sig[] = {0x00, 0x00};
    script_set(&tx.vin[0].script_sig, sig, 2);
    tx.vin[0].sequence = 0xFFFFFFFF;
    tx.num_vout = 1;
    tx.vout = calloc(1, sizeof(struct tx_out));
    tx.vout[0].value = 50 * 100000000LL;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, 3);
    return tx;
}

static void free_simple_tx(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
}

int test_validation(void)
{
    int failures = 0;

    printf("check_transaction: valid simple tx... ");
    {
        struct transaction tx = make_simple_tx();
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("check_transaction: rejects empty vin... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 0;
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk1[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk1, 1);

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vout);
        if (ok && strstr(vs.reject_reason, "vin-empty"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: rejects empty vout... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0xAA, 32);
        uint8_t sig2[] = {0x00, 0x00};
        script_set(&tx.vin[0].script_sig, sig2, 2);
        tx.num_vout = 0;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        if (ok && strstr(vs.reject_reason, "vout-empty"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: rejects negative output... ");
    {
        struct transaction tx = make_simple_tx();
        tx.vout[0].value = -1;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "vout-negative"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: rejects output > MAX_MONEY... ");
    {
        struct transaction tx = make_simple_tx();
        tx.vout[0].value = MAX_MONEY + 1;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "toolarge"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: rejects duplicate inputs... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 2;
        tx.vin = calloc(2, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0xDD, 32);
        tx.vin[0].prevout.n = 0;
        uint8_t sig3[] = {0x00, 0x00};
        script_set(&tx.vin[0].script_sig, sig3, 2);
        tx.vin[1] = tx.vin[0];
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk2[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk2, 1);

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        free(tx.vout);
        if (ok && strstr(vs.reject_reason, "duplicate"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: rejects bad overwinter version... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 1;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "version"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: rejects nonzero value_balance without shielded... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.value_balance = 500;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "valuebalance"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_transaction: valid sapling v4 with shielded spend... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.num_vin = 0;
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk3[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk3, 1);
        tx.num_shielded_spend = 1;
        tx.v_shielded_spend = calloc(1, sizeof(struct spend_description));
        tx.value_balance = 100;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = check_transaction(&tx, &vs);
        free(tx.vout);
        free(tx.v_shielded_spend);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("check_transaction: coinbase rejects joinsplits... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = 0xFFFFFFFF;
        uint8_t cb[] = {0x04, 0xff, 0xff, 0xff};
        script_set(&tx.vin[0].script_sig, cb, 4);
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 1000000000LL;
        uint8_t pk4[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk4, 1);
        tx.num_joinsplit = 1;
        tx.v_joinsplit = calloc(1, sizeof(struct js_description));

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        free(tx.vout);
        free(tx.v_joinsplit);
        if (ok && strstr(vs.reject_reason, "joinsplit"))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("MAX_MONEY = 21M * 100000000... ");
    {
        bool ok = (MAX_MONEY == (int64_t)2100000000000000LL);
        if (ok) printf("OK\n");
        else { printf("FAIL (MAX_MONEY=%lld)\n", (long long)MAX_MONEY); failures++; }
    }

    printf("COINBASE_MATURITY = 100... ");
    {
        bool ok = (COINBASE_MATURITY == 100);
        if (ok) printf("OK\n");
        else { printf("FAIL (%d)\n", COINBASE_MATURITY); failures++; }
    }

    printf("MAX_BLOCK_SIZE = 2000000... ");
    {
        bool ok = (MAX_BLOCK_SIZE == 2000000);
        if (ok) printf("OK\n");
        else { printf("FAIL (%d)\n", MAX_BLOCK_SIZE); failures++; }
    }

    /* ================================================================
     * check_transaction: duplicate Sapling nullifiers
     * ================================================================ */
    printf("check_transaction: rejects duplicate sapling nullifiers... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.num_vin = 0;
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk5[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk5, 1);
        tx.num_shielded_spend = 2;
        tx.v_shielded_spend = calloc(2, sizeof(struct spend_description));
        /* Same nullifier in both spend descriptions */
        memset(tx.v_shielded_spend[0].nullifier.data, 0xBB, 32);
        memset(tx.v_shielded_spend[1].nullifier.data, 0xBB, 32);
        tx.value_balance = 100;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vout);
        free(tx.v_shielded_spend);
        if (ok && strstr(vs.reject_reason, "nullifiers-duplicate"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: coinbase rejects shielded spends
     * ================================================================ */
    printf("check_transaction: coinbase rejects shielded spends... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = 0xFFFFFFFF;
        uint8_t cb2[] = {0x04, 0xff, 0xff, 0xff};
        script_set(&tx.vin[0].script_sig, cb2, 4);
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 1000000000LL;
        uint8_t pk6[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk6, 1);
        tx.num_shielded_spend = 1;
        tx.v_shielded_spend = calloc(1, sizeof(struct spend_description));
        tx.value_balance = 1000;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        free(tx.vout);
        free(tx.v_shielded_spend);
        if (ok && strstr(vs.reject_reason, "spend-description"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: coinbase rejects shielded outputs
     * ================================================================ */
    printf("check_transaction: coinbase rejects shielded outputs... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = 0xFFFFFFFF;
        uint8_t cb3[] = {0x04, 0xff, 0xff, 0xff};
        script_set(&tx.vin[0].script_sig, cb3, 4);
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 1000000000LL;
        uint8_t pk7[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk7, 1);
        tx.num_shielded_output = 1;
        tx.v_shielded_output = calloc(1, sizeof(struct output_description));

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        free(tx.vout);
        free(tx.v_shielded_output);
        if (ok && strstr(vs.reject_reason, "output-description"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: joinsplit vpub_old and vpub_new both nonzero
     * ================================================================ */
    printf("check_transaction: rejects joinsplit both vpubs nonzero... ");
    {
        struct transaction tx = make_simple_tx();
        tx.num_joinsplit = 1;
        tx.v_joinsplit = calloc(1, sizeof(struct js_description));
        tx.v_joinsplit[0].vpub_old = 1000;
        tx.v_joinsplit[0].vpub_new = 2000;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        free(tx.v_joinsplit);
        if (ok && strstr(vs.reject_reason, "vpubs-both-nonzero"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: joinsplit vpub_old > MAX_MONEY
     * ================================================================ */
    printf("check_transaction: rejects joinsplit vpub_old > MAX_MONEY... ");
    {
        struct transaction tx = make_simple_tx();
        tx.num_joinsplit = 1;
        tx.v_joinsplit = calloc(1, sizeof(struct js_description));
        tx.v_joinsplit[0].vpub_old = MAX_MONEY + 1;
        tx.v_joinsplit[0].vpub_new = 0;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        free(tx.v_joinsplit);
        if (ok && strstr(vs.reject_reason, "vpub_old-toolarge"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: value_balance overflow
     * ================================================================ */
    printf("check_transaction: rejects value_balance overflow... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.num_vin = 0;
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk8[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk8, 1);
        tx.num_shielded_spend = 1;
        tx.v_shielded_spend = calloc(1, sizeof(struct spend_description));
        tx.value_balance = MAX_MONEY + 1;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vout);
        free(tx.v_shielded_spend);
        if (ok && strstr(vs.reject_reason, "valuebalance-toolarge"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: rejects bad version group id
     * ================================================================ */
    printf("check_transaction: rejects bad version group id... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 3;
        tx.version_group_id = 0xDEADBEEF;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "version-group-id"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: rejects expiry height too high
     * ================================================================ */
    printf("check_transaction: rejects expiry height too high... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 3;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        tx.expiry_height = TX_EXPIRY_HEIGHT_THRESHOLD;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "expiry-height"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: valid overwinter v3 transaction
     * ================================================================ */
    printf("check_transaction: valid overwinter v3... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 3;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        tx.expiry_height = 500000;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: coinbase script length bounds
     * ================================================================ */
    printf("check_transaction: coinbase rejects script too short... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = 0xFFFFFFFF;
        uint8_t cb4[] = {0x04};
        script_set(&tx.vin[0].script_sig, cb4, 1);
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 1000000000LL;
        uint8_t pk9[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk9, 1);

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        free(tx.vout);
        if (ok && strstr(vs.reject_reason, "cb-length"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: non-coinbase rejects null prevout
     * ================================================================ */
    printf("check_transaction: non-coinbase rejects null prevout... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        /* null outpoint = hash all zeros + n = UINT32_MAX */
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = 0xFFFFFFFF;
        uint8_t sig4[] = {0x00, 0x00};
        script_set(&tx.vin[0].script_sig, sig4, 2);
        /* Need >1 vin or a non-coinbase scriptsig to avoid being treated as coinbase.
         * Actually with hash=0, n=0xFFFFFFFF, and 1 input, this IS a coinbase.
         * Use 2 inputs so it's not a coinbase (coinbase must have exactly 1 input). */
        tx.num_vin = 2;
        tx.vin = realloc(tx.vin, 2 * sizeof(struct tx_in));
        memset(&tx.vin[1], 0, sizeof(struct tx_in));
        memset(tx.vin[1].prevout.hash.data, 0xAA, 32);
        tx.vin[1].prevout.n = 0;
        uint8_t sig4b[] = {0x00, 0x00};
        script_set(&tx.vin[1].script_sig, sig4b, 2);
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk10[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk10, 1);

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free(tx.vin);
        free(tx.vout);
        if (ok && strstr(vs.reject_reason, "prevout-null"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * contextual_check_transaction tests
     * ================================================================ */
    const struct chain_params *mainparams = chain_params_get();

    printf("contextual_check_tx: sprout rejects overwinter tx... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 3;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        struct validation_state vs;
        validation_state_init(&vs);
        /* Height 1 is before Overwinter activation */
        bool ok = !contextual_check_transaction(&tx, &vs, &mainparams->consensus, 1, 100);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "overwinter-not-active"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("contextual_check_tx: sapling accepts valid v4... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 500001; /* must be > nHeight to not be expired */
        struct validation_state vs;
        validation_state_init(&vs);
        /* Height 500000 is after Sapling activation (476969) */
        bool ok = contextual_check_transaction(&tx, &vs, &mainparams->consensus, 500000, 100);
        free_simple_tx(&tx);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("contextual_check_tx: rejects expired tx... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 499999;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !contextual_check_transaction(&tx, &vs, &mainparams->consensus, 500000, 100);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "expired"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("contextual_check_tx: sapling rejects bad version group... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID; /* wrong for sapling */
        tx.expiry_height = 500000;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !contextual_check_transaction(&tx, &vs, &mainparams->consensus, 500000, 100);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "version-group-id"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("contextual_check_tx: sapling rejects version too low... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = true;
        tx.version = 3; /* too low for sapling */
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 500000;
        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !contextual_check_transaction(&tx, &vs, &mainparams->consensus, 500000, 100);
        free_simple_tx(&tx);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("contextual_check_tx: overwinter requires overwintered flag... ");
    {
        struct transaction tx = make_simple_tx();
        tx.overwintered = false;
        tx.version = 1;
        struct validation_state vs;
        validation_state_init(&vs);
        /* Height 476970 is after Overwinter activation */
        bool ok = !contextual_check_transaction(&tx, &vs, &mainparams->consensus, 476970, 100);
        free_simple_tx(&tx);
        if (ok && strstr(vs.reject_reason, "overwinter-active"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    printf("contextual_check_tx: post-sapling allows larger tx... ");
    {
        /* On mainnet, Overwinter and Sapling activate at the same height (476969).
         * After Sapling, the MAX_TX_SIZE_BEFORE_SAPLING limit no longer applies.
         * Verify a large tx is accepted post-Sapling. */
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 500001;
        tx.num_vin = 1;
        tx.vin = calloc(1, sizeof(struct tx_in));
        memset(tx.vin[0].prevout.hash.data, 0xAA, 32);
        uint8_t sig5[] = {0x00, 0x00};
        script_set(&tx.vin[0].script_sig, sig5, 2);
        /* ~3000 outputs * ~34 bytes each ≈ 102000 > MAX_TX_SIZE_BEFORE_SAPLING */
        tx.num_vout = 3000;
        tx.vout = calloc(tx.num_vout, sizeof(struct tx_out));
        for (size_t i = 0; i < tx.num_vout; i++) {
            tx.vout[i].value = 1;
            uint8_t pk11[] = {0x76,0xa9,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x88,0xac};
            script_set(&tx.vout[i].script_pub_key, pk11, 25);
        }

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = contextual_check_transaction(&tx, &vs, &mainparams->consensus, 500000, 100);
        free(tx.vin);
        free(tx.vout);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: vin empty but joinsplits exist is valid
     * ================================================================ */
    printf("check_transaction: empty vin with joinsplits is valid... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.version = 1;
        tx.num_vin = 0;
        tx.num_vout = 1;
        tx.vout = calloc(1, sizeof(struct tx_out));
        tx.vout[0].value = 100;
        uint8_t pk12[] = {0x00};
        script_set(&tx.vout[0].script_pub_key, pk12, 1);
        tx.num_joinsplit = 1;
        tx.v_joinsplit = calloc(1, sizeof(struct js_description));
        tx.v_joinsplit[0].vpub_new = 100;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = check_transaction(&tx, &vs);
        free(tx.vout);
        free(tx.v_joinsplit);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * check_transaction: duplicate joinsplit nullifiers
     * ================================================================ */
    printf("check_transaction: rejects duplicate joinsplit nullifiers... ");
    {
        struct transaction tx = make_simple_tx();
        tx.num_joinsplit = 2;
        tx.v_joinsplit = calloc(2, sizeof(struct js_description));
        /* Set same nullifier in both joinsplits */
        memset(tx.v_joinsplit[0].nullifiers[0].data, 0xCC, 32);
        memset(tx.v_joinsplit[1].nullifiers[0].data, 0xCC, 32);
        tx.v_joinsplit[0].vpub_old = 100;
        tx.v_joinsplit[1].vpub_old = 100;

        struct validation_state vs;
        validation_state_init(&vs);
        bool ok = !check_transaction(&tx, &vs);
        free_simple_tx(&tx);
        free(tx.v_joinsplit);
        if (ok && strstr(vs.reject_reason, "nullifiers-duplicate"))
            printf("OK\n");
        else { printf("FAIL (%s)\n", vs.reject_reason); failures++; }
    }

    /* ================================================================
     * is_expired_tx tests
     * ================================================================ */
    printf("is_expired_tx: not expired when expiry_height > nHeight... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 1000;
        bool ok = !is_expired_tx(&tx, 999);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("is_expired_tx: expired when expiry_height <= nHeight... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 1000;
        bool ok = is_expired_tx(&tx, 1000);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("is_expired_tx: zero expiry_height means no expiry... ");
    {
        struct transaction tx;
        memset(&tx, 0, sizeof(tx));
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 0;
        bool ok = !is_expired_tx(&tx, 999999);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
