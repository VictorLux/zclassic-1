/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Validation pipeline tests: check_transaction, check_block, consensus rules. */

#include "test/test_helpers.h"

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

    return failures;
}
