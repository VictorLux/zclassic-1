/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

int test_coins(void)
{
    int failures = 0;

    printf("coins_init... ");
    {
        struct coins c;
        coins_init(&c);
        if (!c.is_coinbase && c.vout == NULL && c.num_vout == 0 &&
            c.height == 0 && c.version == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins_alloc/free... ");
    {
        struct coins c;
        coins_init(&c);
        bool ok = coins_alloc(&c, 3);
        if (ok && c.num_vout == 3 && c.vout != NULL) {
            coins_free(&c);
            if (c.vout == NULL && c.num_vout == 0)
                printf("OK\n");
            else { printf("FAIL (free)\n"); failures++; }
        } else {
            printf("FAIL (alloc)\n"); failures++;
        }
    }

    printf("coins_alloc outputs are null... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 4);
        bool all_null = true;
        for (size_t i = 0; i < c.num_vout; i++) {
            if (!tx_out_is_null(&c.vout[i])) {
                all_null = false;
                break;
            }
        }
        if (all_null)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    printf("tx_out_is_null... ");
    {
        struct tx_out out;
        tx_out_set_null(&out);
        bool null_check = tx_out_is_null(&out);
        out.value = 100;
        bool non_null_check = !tx_out_is_null(&out);
        if (null_check && non_null_check)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins_is_available... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 3);
        if (coins_is_available(&c, 0)) {
            printf("FAIL (null output reported available)\n");
            failures++;
        } else {
            c.vout[1].value = 5000;
            if (coins_is_available(&c, 1) && !coins_is_available(&c, 0) &&
                !coins_is_available(&c, 99))
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        }
        coins_free(&c);
    }

    printf("coins_is_pruned all null... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 3);
        if (coins_is_pruned(&c))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_is_pruned with live output... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 3);
        c.vout[1].value = 1000;
        if (!coins_is_pruned(&c))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_is_pruned empty... ");
    {
        struct coins c;
        coins_init(&c);
        if (coins_is_pruned(&c))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins_spend... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 2);
        c.vout[0].value = 500;
        c.vout[1].value = 1000;
        bool spent = coins_spend(&c, 0);
        if (spent && !coins_is_available(&c, 0) && coins_is_available(&c, 1))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_spend out of range... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 2);
        c.vout[0].value = 500;
        bool spent = coins_spend(&c, 5);
        if (!spent)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_spend already spent... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 1);
        c.vout[0].value = 500;
        coins_spend(&c, 0);
        bool double_spend = coins_spend(&c, 0);
        if (!double_spend)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_cleanup trims trailing nulls... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 5);
        c.vout[0].value = 100;
        c.vout[1].value = 200;
        coins_cleanup(&c);
        if (c.num_vout == 2)
            printf("OK (num_vout=%zu)\n", c.num_vout);
        else {
            printf("FAIL (num_vout=%zu, expected 2)\n", c.num_vout);
            failures++;
        }
        coins_free(&c);
    }

    printf("coins_copy... ");
    {
        struct coins src, dst;
        coins_init(&src);
        coins_init(&dst);
        coins_alloc(&src, 2);
        src.is_coinbase = true;
        src.height = 12345;
        src.version = 4;
        src.vout[0].value = 9999;
        src.vout[1].value = 8888;
        coins_copy(&dst, &src);
        if (dst.is_coinbase && dst.height == 12345 && dst.version == 4 &&
            dst.num_vout == 2 && dst.vout[0].value == 9999 &&
            dst.vout[1].value == 8888 && dst.vout != src.vout)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&src);
        coins_free(&dst);
    }

    printf("coins_from_transaction... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 3);
        tx.version = 4;
        tx.vout[0].value = 100;
        tx.vout[1].value = 200;
        tx.vout[2].value = 300;
        struct coins c;
        coins_init(&c);
        coins_from_transaction(&c, &tx, 500);
        if (c.height == 500 && c.version == 4 &&
            coins_is_available(&c, 0) && coins_is_available(&c, 1) &&
            coins_is_available(&c, 2) &&
            c.vout[0].value == 100 && c.vout[2].value == 300)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
        transaction_free(&tx);
    }

    printf("coins_stats_init... ");
    {
        struct coins_stats s;
        coins_stats_init(&s);
        if (s.height == 0 && s.num_transactions == 0 &&
            s.num_tx_outputs == 0 && s.total_amount == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins_alloc zero... ");
    {
        struct coins c;
        coins_init(&c);
        bool ok = coins_alloc(&c, 0);
        if (ok && c.num_vout == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
    }

    /* ================================================================
     * coins_map: insert, find, erase, count
     * ================================================================ */
    printf("coins_map: insert/find/count... ");
    {
        struct coins_map m;
        coins_map_init(&m);

        struct uint256 txid1, txid2, txid3;
        memset(txid1.data, 0x11, 32);
        memset(txid2.data, 0x22, 32);
        memset(txid3.data, 0x33, 32);

        struct coins_cache_entry *e1 = coins_map_insert(&m, &txid1);
        struct coins_cache_entry *e2 = coins_map_insert(&m, &txid2);
        bool ok = (e1 != NULL) && (e2 != NULL) && (coins_map_count(&m) == 2);

        /* Find existing */
        ok = ok && (coins_map_find(&m, &txid1) == e1);
        ok = ok && (coins_map_find(&m, &txid2) == e2);
        /* Find non-existing */
        ok = ok && (coins_map_find(&m, &txid3) == NULL);
        /* Insert duplicate returns same entry */
        ok = ok && (coins_map_insert(&m, &txid1) == e1);
        ok = ok && (coins_map_count(&m) == 2);

        coins_map_free(&m);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins_map: erase... ");
    {
        struct coins_map m;
        coins_map_init(&m);

        struct uint256 txid1, txid2;
        memset(txid1.data, 0xAA, 32);
        memset(txid2.data, 0xBB, 32);

        coins_map_insert(&m, &txid1);
        coins_map_insert(&m, &txid2);

        bool ok = coins_map_erase(&m, &txid1);
        ok = ok && (coins_map_count(&m) == 1);
        ok = ok && (coins_map_find(&m, &txid1) == NULL);
        ok = ok && (coins_map_find(&m, &txid2) != NULL);

        /* Erase non-existing returns false */
        ok = ok && !coins_map_erase(&m, &txid1);

        coins_map_free(&m);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins_map: handles many insertions (rehash)... ");
    {
        struct coins_map m;
        coins_map_init(&m);

        for (int i = 0; i < 100; i++) {
            struct uint256 txid;
            memset(txid.data, 0, 32);
            memcpy(txid.data, &i, sizeof(i));
            coins_map_insert(&m, &txid);
        }
        bool ok = (coins_map_count(&m) == 100);

        /* Verify we can find them all */
        for (int i = 0; i < 100; i++) {
            struct uint256 txid;
            memset(txid.data, 0, 32);
            memcpy(txid.data, &i, sizeof(i));
            ok = ok && (coins_map_find(&m, &txid) != NULL);
        }

        coins_map_free(&m);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * coins_view_cache: init, modify_new, get_coins, have_coins
     * ================================================================ */
    printf("coins_view_cache: init and basic operations... ");
    {
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        struct coins_view_cache cache;
        coins_view_cache_init(&cache, &null_view);

        bool ok = (coins_map_count(&cache.cache_coins) == 0);

        /* modify_new creates a fresh entry */
        struct uint256 txid;
        memset(txid.data, 0x42, 32);
        struct coins_cache_entry *entry = coins_view_cache_modify_new(&cache, &txid);
        ok = ok && (entry != NULL);
        ok = ok && (entry->flags & COINS_CACHE_DIRTY);
        ok = ok && (entry->flags & COINS_CACHE_FRESH);

        /* Populate the coins */
        coins_alloc(&entry->coins, 2);
        entry->coins.vout[0].value = 100;
        uint8_t pk1[] = {0x76};
        script_set(&entry->coins.vout[0].script_pub_key, pk1, 1);
        entry->coins.vout[1].value = 200;
        script_set(&entry->coins.vout[1].script_pub_key, pk1, 1);
        entry->coins.height = 500;
        entry->coins.version = 4;

        /* have_coins should find it */
        ok = ok && coins_view_cache_have_coins(&cache, &txid);

        /* get_coins should retrieve it */
        struct coins retrieved;
        coins_init(&retrieved);
        ok = ok && coins_view_cache_get_coins(&cache, &txid, &retrieved);
        ok = ok && (retrieved.vout[0].value == 100);
        ok = ok && (retrieved.vout[1].value == 200);
        ok = ok && (retrieved.height == 500);
        coins_free(&retrieved);

        /* Unknown txid should not be found */
        struct uint256 unknown;
        memset(unknown.data, 0xFF, 32);
        ok = ok && !coins_view_cache_have_coins(&cache, &unknown);

        coins_view_cache_free(&cache);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * coins_view_cache: modify existing entry
     * ================================================================ */
    printf("coins_view_cache: modify marks dirty... ");
    {
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        struct coins_view_cache cache;
        coins_view_cache_init(&cache, &null_view);

        struct uint256 txid;
        memset(txid.data, 0x55, 32);
        struct coins_cache_entry *entry = coins_view_cache_modify_new(&cache, &txid);
        coins_alloc(&entry->coins, 1);
        entry->coins.vout[0].value = 50;
        uint8_t pk2[] = {0x76};
        script_set(&entry->coins.vout[0].script_pub_key, pk2, 1);

        /* modify should return same entry and mark dirty */
        entry->flags = 0; /* clear flags */
        struct coins_cache_entry *e2 = coins_view_cache_modify(&cache, &txid);
        bool ok = (e2 == entry) && (e2->flags & COINS_CACHE_DIRTY);

        coins_view_cache_free(&cache);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * coins_view_cache: set/get best block
     * ================================================================ */
    printf("coins_view_cache: set/get best block... ");
    {
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        struct coins_view_cache cache;
        coins_view_cache_init(&cache, &null_view);

        struct uint256 block_hash;
        memset(block_hash.data, 0xAA, 32);
        coins_view_cache_set_best_block(&cache, &block_hash);

        struct uint256 retrieved;
        coins_view_cache_get_best_block(&cache, &retrieved);
        bool ok = uint256_eq(&retrieved, &block_hash);

        coins_view_cache_free(&cache);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * coins_view_cache: have_inputs for coinbase is always true
     * ================================================================ */
    printf("coins_view_cache: have_inputs coinbase always true... ");
    {
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        struct coins_view_cache cache;
        coins_view_cache_init(&cache, &null_view);

        /* Create a coinbase tx (prevout hash all zeros, n=UINT32_MAX) */
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = UINT32_MAX;
        tx.vout[0].value = 1250000000LL;

        bool ok = coins_view_cache_have_inputs(&cache, &tx);

        transaction_free(&tx);
        coins_view_cache_free(&cache);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * coins_view_cache: flush to parent cache
     * ================================================================ */
    printf("coins_view_cache: flush to parent... ");
    {
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));

        struct coins_view_cache parent;
        coins_view_cache_init(&parent, &null_view);

        /* Make parent usable as a backing view */
        struct coins_view parent_as_view;
        coins_view_cache_as_view(&parent_as_view, &parent);

        struct coins_view_cache child;
        coins_view_cache_init(&child, &parent_as_view);

        /* Add a coin to child */
        struct uint256 txid;
        memset(txid.data, 0x77, 32);
        struct coins_cache_entry *entry = coins_view_cache_modify_new(&child, &txid);
        coins_alloc(&entry->coins, 1);
        entry->coins.vout[0].value = 999;
        uint8_t pk3[] = {0x76};
        script_set(&entry->coins.vout[0].script_pub_key, pk3, 1);

        struct uint256 block_hash;
        memset(block_hash.data, 0x88, 32);
        coins_view_cache_set_best_block(&child, &block_hash);

        /* Flush child to parent */
        bool ok = coins_view_cache_flush(&child);

        /* Child should be empty after flush */
        ok = ok && (coins_map_count(&child.cache_coins) == 0);

        /* Parent should now have the coin */
        ok = ok && coins_view_cache_have_coins(&parent, &txid);
        struct coins retrieved;
        coins_init(&retrieved);
        ok = ok && coins_view_cache_get_coins(&parent, &txid, &retrieved);
        ok = ok && (retrieved.vout[0].value == 999);
        coins_free(&retrieved);

        /* Parent should have the best block hash */
        struct uint256 parent_hash;
        coins_view_cache_get_best_block(&parent, &parent_hash);
        ok = ok && uint256_eq(&parent_hash, &block_hash);

        coins_view_cache_free(&child);
        coins_view_cache_free(&parent);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * coins_view_cache: get_value_in for coinbase returns 0
     * ================================================================ */
    printf("coins_view_cache: get_value_in coinbase returns 0... ");
    {
        struct coins_view null_view;
        memset(&null_view, 0, sizeof(null_view));
        struct coins_view_cache cache;
        coins_view_cache_init(&cache, &null_view);

        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0, 32);
        tx.vin[0].prevout.n = UINT32_MAX;
        tx.vout[0].value = 1250000000LL;

        int64_t val = coins_view_cache_get_value_in(&cache, &tx);
        bool ok = (val == 0);

        transaction_free(&tx);
        coins_view_cache_free(&cache);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
