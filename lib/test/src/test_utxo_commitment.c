/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for the incremental UTXO set commitment (XOR-hash accumulator). */

#include "test/test_helpers.h"
#include "coins/utxo_commitment.h"
#include <string.h>

int test_utxo_commitment(void)
{
    int failures = 0;

    printf("utxo_commitment: init is zero... ");
    {
        struct utxo_commitment uc;
        utxo_commitment_init(&uc);
        uint8_t zero[32] = {0};
        bool ok = (uc.count == 0 && memcmp(uc.accumulator, zero, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("utxo_commitment: add then remove returns to zero... ");
    {
        struct utxo_commitment uc;
        utxo_commitment_init(&uc);
        uint8_t txid[32];
        memset(txid, 0xAB, 32);
        utxo_commitment_add(&uc, txid, 0, 50000000, 100);

        /* Should be non-zero after add */
        uint8_t zero[32] = {0};
        bool nonzero = (memcmp(uc.accumulator, zero, 32) != 0);

        utxo_commitment_remove(&uc, txid, 0, 50000000, 100);
        bool back_to_zero = (memcmp(uc.accumulator, zero, 32) == 0);
        bool ok = nonzero && back_to_zero && uc.count == 0;
        if (ok) printf("OK\n");
        else { printf("FAIL (nonzero=%d back=%d count=%llu)\n",
                       nonzero, back_to_zero, (unsigned long long)uc.count);
               failures++; }
    }

    printf("utxo_commitment: order independent... ");
    {
        uint8_t txid1[32], txid2[32];
        memset(txid1, 0x11, 32);
        memset(txid2, 0x22, 32);

        struct utxo_commitment a, b;
        utxo_commitment_init(&a);
        utxo_commitment_init(&b);

        /* Add in different orders */
        utxo_commitment_add(&a, txid1, 0, 1000, 10);
        utxo_commitment_add(&a, txid2, 1, 2000, 20);

        utxo_commitment_add(&b, txid2, 1, 2000, 20);
        utxo_commitment_add(&b, txid1, 0, 1000, 10);

        bool ok = utxo_commitment_equal(&a, &b);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("utxo_commitment: different UTXOs produce different hashes... ");
    {
        struct utxo_commitment a, b;
        utxo_commitment_init(&a);
        utxo_commitment_init(&b);
        uint8_t txid[32];
        memset(txid, 0x33, 32);

        utxo_commitment_add(&a, txid, 0, 1000, 10);
        utxo_commitment_add(&b, txid, 0, 1001, 10); /* different value */

        bool ok = !utxo_commitment_equal(&a, &b);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("utxo_commitment: serialize/deserialize roundtrip... ");
    {
        struct utxo_commitment uc;
        utxo_commitment_init(&uc);
        uint8_t txid[32];
        memset(txid, 0x44, 32);
        utxo_commitment_add(&uc, txid, 5, 99999, 500);
        utxo_commitment_add(&uc, txid, 6, 88888, 501);

        uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE];
        utxo_commitment_serialize(&uc, buf);

        struct utxo_commitment uc2;
        bool ok = utxo_commitment_deserialize(&uc2, buf, sizeof(buf));
        ok = ok && utxo_commitment_equal(&uc, &uc2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("utxo_commitment: deserialize rejects short buffer... ");
    {
        struct utxo_commitment uc;
        uint8_t buf[10] = {0};
        bool ok = !utxo_commitment_deserialize(&uc, buf, sizeof(buf));
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("utxo_commitment: merge combines two sets... ");
    {
        uint8_t txid1[32], txid2[32];
        memset(txid1, 0x55, 32);
        memset(txid2, 0x66, 32);

        /* Build combined set */
        struct utxo_commitment combined;
        utxo_commitment_init(&combined);
        utxo_commitment_add(&combined, txid1, 0, 1000, 10);
        utxo_commitment_add(&combined, txid2, 0, 2000, 20);

        /* Build two separate sets and merge */
        struct utxo_commitment a, b;
        utxo_commitment_init(&a);
        utxo_commitment_init(&b);
        utxo_commitment_add(&a, txid1, 0, 1000, 10);
        utxo_commitment_add(&b, txid2, 0, 2000, 20);
        utxo_commitment_merge(&a, &b);

        bool ok = utxo_commitment_equal(&a, &combined);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("utxo_commitment: large set add/remove consistency... ");
    {
        struct utxo_commitment uc;
        utxo_commitment_init(&uc);

        /* Add 1000 UTXOs */
        for (uint32_t i = 0; i < 1000; i++) {
            uint8_t txid[32] = {0};
            memcpy(txid, &i, sizeof(i));
            utxo_commitment_add(&uc, txid, i, (int64_t)i * 100000, (int32_t)i);
        }

        /* Remove first 500 */
        for (uint32_t i = 0; i < 500; i++) {
            uint8_t txid[32] = {0};
            memcpy(txid, &i, sizeof(i));
            utxo_commitment_remove(&uc, txid, i, (int64_t)i * 100000, (int32_t)i);
        }

        /* Build from scratch with just 500-999 */
        struct utxo_commitment expected;
        utxo_commitment_init(&expected);
        for (uint32_t i = 500; i < 1000; i++) {
            uint8_t txid[32] = {0};
            memcpy(txid, &i, sizeof(i));
            utxo_commitment_add(&expected, txid, i, (int64_t)i * 100000, (int32_t)i);
        }

        bool ok = utxo_commitment_equal(&uc, &expected);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
