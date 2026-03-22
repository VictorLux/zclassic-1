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

    /* ================================================================
     * ZCLASSIC C++ COMPATIBILITY: compress_amount / decompress_amount
     * Test vectors from zclassic compress_tests.cpp (identical to Bitcoin Core)
     * COIN = 100000000, CENT = 1000000
     * ================================================================ */
    printf("zclassic compat: compress_amount test pairs... ");
    {
        /* Exact pairs from C++ compress_tests.cpp TestPair() calls */
        bool ok = true;
        ok = ok && (compress_amount(0) == 0x0);
        ok = ok && (decompress_amount(0x0) == 0);

        ok = ok && (compress_amount(1) == 0x1);
        ok = ok && (decompress_amount(0x1) == 1);

        ok = ok && (compress_amount(1000000) == 0x7);       /* CENT */
        ok = ok && (decompress_amount(0x7) == 1000000);

        ok = ok && (compress_amount(100000000) == 0x9);      /* COIN */
        ok = ok && (decompress_amount(0x9) == 100000000);

        ok = ok && (compress_amount((uint64_t)50 * 100000000) == 0x32);  /* 50*COIN */
        ok = ok && (decompress_amount(0x32) == (uint64_t)50 * 100000000);

        ok = ok && (compress_amount((uint64_t)21000000 * 100000000) == 0x1406f40);
        ok = ok && (decompress_amount(0x1406f40) == (uint64_t)21000000 * 100000000);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: compress_amount roundtrip 1..100000... ");
    {
        bool ok = true;
        /* C++ test: every value 1..NUM_MULTIPLES_UNIT (100000) */
        for (uint64_t i = 1; i <= 100000 && ok; i++)
            ok = (decompress_amount(compress_amount(i)) == i);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: compress_amount roundtrip CENT multiples... ");
    {
        bool ok = true;
        for (uint64_t i = 1; i <= 10000 && ok; i++)
            ok = (decompress_amount(compress_amount(i * 1000000)) == i * 1000000);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: compress_amount roundtrip COIN multiples... ");
    {
        bool ok = true;
        for (uint64_t i = 1; i <= 10000 && ok; i++)
            ok = (decompress_amount(compress_amount(i * 100000000)) == i * 100000000);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: compress_amount roundtrip 50*COIN multiples... ");
    {
        bool ok = true;
        for (uint64_t i = 1; i <= 420000 && ok; i++)
            ok = (decompress_amount(compress_amount(i * (uint64_t)50 * 100000000))
                  == i * (uint64_t)50 * 100000000);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: decompress_amount roundtrip 0..100000... ");
    {
        bool ok = true;
        /* C++ test: every compressed value 0..100000 */
        for (uint64_t i = 0; i < 100000 && ok; i++)
            ok = (compress_amount(decompress_amount(i)) == i);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPATIBILITY: VARINT encoding
     * Bitcoin/Zcash VARINT uses 7-bit groups with continuation bits.
     * From C++ coins_tests.cpp: VARINT(3000000000) == "8a95c0bb00"
     * ================================================================ */
    printf("zclassic compat: varint encoding of 3000000000... ");
    {
        struct byte_stream s;
        stream_init(&s, 16);
        stream_write_varint(&s, 3000000000ULL);
        /* C++ test: VARINT(3000000000) == hex "8a95c0bb00" */
        bool ok = (s.size == 5);
        ok = ok && (s.data[0] == 0x8a);
        ok = ok && (s.data[1] == 0x95);
        ok = ok && (s.data[2] == 0xc0);
        ok = ok && (s.data[3] == 0xbb);
        ok = ok && (s.data[4] == 0x00);
        stream_free(&s);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: varint roundtrip... ");
    {
        uint64_t test_vals[] = {0, 1, 127, 128, 255, 256, 16383, 16384,
                                 65535, 100000, 1000000, 100000000,
                                 3000000000ULL, 0xFFFFFFFFULL,
                                 0xFFFFFFFFFFULL, 0xFFFFFFFFFFFFULL};
        bool ok = true;
        for (int i = 0; i < 16 && ok; i++) {
            struct byte_stream ws;
            stream_init(&ws, 16);
            stream_write_varint(&ws, test_vals[i]);
            struct byte_stream rs;
            stream_init_from_data(&rs, ws.data, ws.size);
            uint64_t decoded = 0;
            ok = stream_read_varint(&rs, &decoded);
            ok = ok && (decoded == test_vals[i]);
            stream_free(&ws);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPATIBILITY: CCoins serialization
     * Test vectors from zclassic coins_tests.cpp ccoins_serialization
     * These are the EXACT hex blobs from the C++ test suite.
     * ================================================================ */
    printf("zclassic compat: CCoins deserialize (600 BTC vout[1])... ");
    {
        /* From C++: "0104835800816115944e077fe7c803cfa57f29b36bf87c1d358bb85e"
         * version=1, code=4 (vout[1] present, not coinbase), vout[1]=60000000000,
         * P2PKH to 816115944e077fe7c803cfa57f29b36bf87c1d35, height=203998 */
        uint8_t hex[] = {
            0x01, 0x04, 0x83, 0x58, 0x00, 0x81, 0x61, 0x15,
            0x94, 0x4e, 0x07, 0x7f, 0xe7, 0xc8, 0x03, 0xcf,
            0xa5, 0x7f, 0x29, 0xb3, 0x6b, 0xf8, 0x7c, 0x1d,
            0x35, 0x8b, 0xb8, 0x5e
        };
        struct byte_stream s;
        stream_init_from_data(&s, hex, sizeof(hex));

        /* Read version */
        uint64_t nVersion = 0;
        bool ok = stream_read_varint(&s, &nVersion);
        ok = ok && (nVersion == 1);

        /* Read nCode */
        uint64_t nCode = 0;
        ok = ok && stream_read_varint(&s, &nCode);
        bool is_coinbase = (nCode & 1) != 0;
        bool vout0_present = (nCode & 2) != 0;
        bool vout1_present = (nCode & 4) != 0;
        unsigned int nMaskCode = (unsigned int)(nCode / 8) +
            ((vout0_present || vout1_present) ? 0 : 1);

        ok = ok && !is_coinbase;
        ok = ok && !vout0_present;
        ok = ok && vout1_present;
        ok = ok && (nMaskCode == 0);

        /* Read spentness bitmask (0 bytes since nMaskCode=0) */
        /* Build availability */
        size_t num_avail = 2;
        bool avail[256] = {false};
        avail[0] = vout0_present;
        avail[1] = vout1_present;

        /* Allocate and read outputs */
        struct coins cc;
        coins_init(&cc);
        coins_alloc(&cc, num_avail);
        for (size_t i = 0; i < num_avail; i++) {
            if (avail[i])
                ok = ok && compressed_txout_deserialize(&cc.vout[i], &s);
        }

        /* Read height */
        uint64_t h = 0;
        ok = ok && stream_read_varint(&s, &h);

        ok = ok && !coins_is_available(&cc, 0);  /* vout[0] spent */
        ok = ok && coins_is_available(&cc, 1);   /* vout[1] unspent */
        ok = ok && (cc.vout[1].value == 60000000000LL);
        ok = ok && (h == 203998);

        /* Verify P2PKH script: OP_DUP OP_HASH160 <20:hash> OP_EQUALVERIFY OP_CHECKSIG */
        ok = ok && (cc.vout[1].script_pub_key.size == 25);
        ok = ok && (cc.vout[1].script_pub_key.data[0] == 0x76);  /* OP_DUP */
        ok = ok && (cc.vout[1].script_pub_key.data[1] == 0xa9);  /* OP_HASH160 */
        ok = ok && (cc.vout[1].script_pub_key.data[2] == 0x14);  /* push 20 */
        uint8_t expected_hash[20] = {
            0x81, 0x61, 0x15, 0x94, 0x4e, 0x07, 0x7f, 0xe7,
            0xc8, 0x03, 0xcf, 0xa5, 0x7f, 0x29, 0xb3, 0x6b,
            0xf8, 0x7c, 0x1d, 0x35
        };
        ok = ok && (memcmp(cc.vout[1].script_pub_key.data + 3, expected_hash, 20) == 0);
        ok = ok && (cc.vout[1].script_pub_key.data[23] == 0x88);  /* OP_EQUALVERIFY */
        ok = ok && (cc.vout[1].script_pub_key.data[24] == 0xac);  /* OP_CHECKSIG */

        coins_free(&cc);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: CCoins deserialize (coinbase, vout[4]+vout[16])... ");
    {
        /* From C++: "0109044086ef97d5790061b01caab50f1b8e9c50a5057eb43c2d9563a4ee
         *            bbd123008c988f1a4a4de2161e0f50aac7f17e7f9555caa486af3b"
         * version=1, coinbase=true, height=120891
         * vout[4]=234925952, vout[16]=110397 */
        uint8_t hex[] = {
            0x01, 0x09, 0x04, 0x40, 0x86, 0xef, 0x97, 0xd5,
            0x79, 0x00, 0x61, 0xb0, 0x1c, 0xaa, 0xb5, 0x0f,
            0x1b, 0x8e, 0x9c, 0x50, 0xa5, 0x05, 0x7e, 0xb4,
            0x3c, 0x2d, 0x95, 0x63, 0xa4, 0xee, 0xbb, 0xd1,
            0x23, 0x00, 0x8c, 0x98, 0x8f, 0x1a, 0x4a, 0x4d,
            0xe2, 0x16, 0x1e, 0x0f, 0x50, 0xaa, 0xc7, 0xf1,
            0x7e, 0x7f, 0x95, 0x55, 0xca, 0xa4, 0x86, 0xaf,
            0x3b
        };
        struct byte_stream s;
        stream_init_from_data(&s, hex, sizeof(hex));

        /* Read version */
        uint64_t nVersion = 0;
        bool ok = stream_read_varint(&s, &nVersion);
        ok = ok && (nVersion == 1);

        /* Read nCode */
        uint64_t nCode = 0;
        ok = ok && stream_read_varint(&s, &nCode);
        /* nCode=9: coinbase(1), vout[0] not present(0), vout[1] not present(0),
         * 8*(2-1)=8, total=9.  nMaskCode = 9/8 + 1 = 2 (since neither vout0/vout1) */
        bool is_coinbase = (nCode & 1) != 0;
        bool vout0_present = (nCode & 2) != 0;
        bool vout1_present = (nCode & 4) != 0;
        unsigned int nMaskCode = (unsigned int)(nCode / 8) +
            ((vout0_present || vout1_present) ? 0 : 1);

        ok = ok && is_coinbase;
        ok = ok && !vout0_present;
        ok = ok && !vout1_present;
        ok = ok && (nMaskCode == 2);

        /* Read spentness bitmask */
        size_t num_avail = 2;
        bool avail[256] = {false};
        avail[0] = vout0_present;
        avail[1] = vout1_present;
        unsigned int mask_remaining = nMaskCode;
        while (mask_remaining > 0) {
            unsigned char ch = 0;
            ok = ok && stream_read_bytes(&s, &ch, 1);
            for (unsigned int p = 0; p < 8 && num_avail < 256; p++)
                avail[num_avail++] = (ch & (1 << p)) != 0;
            if (ch != 0) mask_remaining--;
        }

        /* Should have vout[4] and vout[16] available */
        ok = ok && !avail[0] && !avail[1];  /* neither vout0/vout1 */
        ok = ok && avail[4];                  /* vout[4] set (bit 2 of byte 0 = 0x04) */
        ok = ok && avail[16];                 /* vout[16] set (bit 6 of byte 1 = 0x40) */

        /* Allocate coins and read available outputs */
        struct coins cc;
        coins_init(&cc);
        coins_alloc(&cc, num_avail);
        for (size_t i = 0; i < num_avail; i++) {
            if (avail[i])
                ok = ok && compressed_txout_deserialize(&cc.vout[i], &s);
        }

        /* Read height */
        uint64_t h = 0;
        ok = ok && stream_read_varint(&s, &h);

        /* Verify all 17 outputs: only vout[4] and vout[16] available */
        for (int i = 0; i < 17 && (size_t)i < num_avail; i++)
            ok = ok && (coins_is_available(&cc, (unsigned)i) == (i == 4 || i == 16));

        ok = ok && (cc.vout[4].value == 234925952);
        ok = ok && (cc.vout[16].value == 110397);
        ok = ok && (h == 120891);

        /* Verify vout[4] P2PKH address hash */
        ok = ok && (cc.vout[4].script_pub_key.size == 25);
        uint8_t addr4[20] = {
            0x61, 0xb0, 0x1c, 0xaa, 0xb5, 0x0f, 0x1b, 0x8e,
            0x9c, 0x50, 0xa5, 0x05, 0x7e, 0xb4, 0x3c, 0x2d,
            0x95, 0x63, 0xa4, 0xee
        };
        ok = ok && (memcmp(cc.vout[4].script_pub_key.data + 3, addr4, 20) == 0);

        /* Verify vout[16] P2PKH address hash */
        ok = ok && (cc.vout[16].script_pub_key.size == 25);
        uint8_t addr16[20] = {
            0x8c, 0x98, 0x8f, 0x1a, 0x4a, 0x4d, 0xe2, 0x16,
            0x1e, 0x0f, 0x50, 0xaa, 0xc7, 0xf1, 0x7e, 0x7f,
            0x95, 0x55, 0xca, 0xa4
        };
        ok = ok && (memcmp(cc.vout[16].script_pub_key.data + 3, addr16, 20) == 0);

        coins_free(&cc);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: CCoins deserialize smallest... ");
    {
        /* From C++: "0002000600"
         * version=0, code=2 (vout[0] present), compressed amount=0,
         * script type 6 (nSize=6, special_size=0 → raw script len=0),
         * height=0 */
        uint8_t hex[] = {0x00, 0x02, 0x00, 0x06, 0x00};
        struct byte_stream s;
        stream_init_from_data(&s, hex, sizeof(hex));

        uint64_t nVersion = 0;
        bool ok = stream_read_varint(&s, &nVersion);
        ok = ok && (nVersion == 0);

        uint64_t nCode = 0;
        ok = ok && stream_read_varint(&s, &nCode);
        bool is_coinbase = (nCode & 1) != 0;
        bool vout0_present = (nCode & 2) != 0;
        bool vout1_present = (nCode & 4) != 0;

        ok = ok && !is_coinbase;
        ok = ok && vout0_present;
        ok = ok && !vout1_present;

        /* No mask bytes (nMaskCode = 0) */

        struct coins cc;
        coins_init(&cc);
        coins_alloc(&cc, 2);
        /* Only vout[0] is present */
        ok = ok && compressed_txout_deserialize(&cc.vout[0], &s);

        uint64_t h = 0;
        ok = ok && stream_read_varint(&s, &h);

        ok = ok && coins_is_available(&cc, 0);
        ok = ok && (cc.vout[0].value == 0);
        ok = ok && (cc.vout[0].script_pub_key.size == 0);
        ok = ok && (h == 0);

        coins_free(&cc);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPATIBILITY: CCoins serialize roundtrip
     * Write coins to stream using our serializer, verify bytes match
     * C++ output, then read back and verify.
     * ================================================================ */
    printf("zclassic compat: CCoins serialize/deserialize roundtrip... ");
    {
        /* Build coins matching first test vector: version=1, not coinbase,
         * vout[0] spent, vout[1] = 60000000000 P2PKH to 816115..1d35,
         * height = 203998 */
        struct coins cc;
        coins_init(&cc);
        cc.version = 1;
        cc.is_coinbase = false;
        cc.height = 203998;
        coins_alloc(&cc, 2);
        /* vout[0] stays null (spent) */
        /* vout[1] = 60000000000 with P2PKH script */
        cc.vout[1].value = 60000000000LL;
        uint8_t p2pkh_script[25] = {
            0x76, 0xa9, 0x14,
            0x81, 0x61, 0x15, 0x94, 0x4e, 0x07, 0x7f, 0xe7,
            0xc8, 0x03, 0xcf, 0xa5, 0x7f, 0x29, 0xb3, 0x6b,
            0xf8, 0x7c, 0x1d, 0x35,
            0x88, 0xac
        };
        script_set(&cc.vout[1].script_pub_key, p2pkh_script, 25);

        /* Serialize using the same logic as coins_view_db_batch_write */
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_varint(&ws, (uint64_t)cc.version);

        bool vout0 = cc.num_vout > 0 && !tx_out_is_null(&cc.vout[0]);
        bool vout1 = cc.num_vout > 1 && !tx_out_is_null(&cc.vout[1]);

        unsigned int nMaskSize = 0, nMaskCodeW = 0;
        for (size_t vi = 2; vi < cc.num_vout; vi++) {
            if (!tx_out_is_null(&cc.vout[vi])) {
                unsigned int bp = (unsigned int)((vi - 2) / 8) + 1;
                if (bp > nMaskSize) nMaskSize = bp;
            }
        }
        for (unsigned int mi = 0; mi < nMaskSize; mi++) {
            unsigned char ch = 0;
            for (unsigned int p = 0; p < 8; p++) {
                size_t idx = 2 + mi * 8 + p;
                if (idx < cc.num_vout && !tx_out_is_null(&cc.vout[idx]))
                    ch |= (1 << p);
            }
            if (ch != 0) nMaskCodeW++;
        }

        uint64_t nCodeW = 8 * (nMaskCodeW - ((vout0 || vout1) ? 0 : 1))
                         + (cc.is_coinbase ? 1 : 0)
                         + (vout0 ? 2 : 0)
                         + (vout1 ? 4 : 0);
        stream_write_varint(&ws, nCodeW);

        for (unsigned int mi = 0; mi < nMaskSize; mi++) {
            unsigned char ch = 0;
            for (unsigned int p = 0; p < 8; p++) {
                size_t idx = 2 + mi * 8 + p;
                if (idx < cc.num_vout && !tx_out_is_null(&cc.vout[idx]))
                    ch |= (1 << p);
            }
            stream_write_bytes(&ws, &ch, 1);
        }
        for (size_t vi = 0; vi < cc.num_vout; vi++) {
            if (!tx_out_is_null(&cc.vout[vi]))
                compressed_txout_serialize(&cc.vout[vi], &ws);
        }
        stream_write_varint(&ws, (uint64_t)cc.height);

        /* Compare with expected C++ output */
        uint8_t expected[] = {
            0x01, 0x04, 0x83, 0x58, 0x00, 0x81, 0x61, 0x15,
            0x94, 0x4e, 0x07, 0x7f, 0xe7, 0xc8, 0x03, 0xcf,
            0xa5, 0x7f, 0x29, 0xb3, 0x6b, 0xf8, 0x7c, 0x1d,
            0x35, 0x8b, 0xb8, 0x5e
        };
        bool ok = (ws.size == sizeof(expected));
        ok = ok && (memcmp(ws.data, expected, sizeof(expected)) == 0);

        /* Now deserialize and verify roundtrip */
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);

        uint64_t ver2 = 0;
        stream_read_varint(&rs, &ver2);
        ok = ok && (ver2 == 1);

        uint64_t code2 = 0;
        stream_read_varint(&rs, &code2);
        ok = ok && ((code2 & 2) == 0);  /* vout[0] not present */
        ok = ok && ((code2 & 4) != 0);  /* vout[1] present */

        struct tx_out out1;
        tx_out_set_null(&out1);
        ok = ok && compressed_txout_deserialize(&out1, &rs);
        ok = ok && (out1.value == 60000000000LL);

        uint64_t h2 = 0;
        stream_read_varint(&rs, &h2);
        ok = ok && (h2 == 203998);

        stream_free(&ws);
        coins_free(&cc);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPATIBILITY: compressed_txout roundtrip
     * ================================================================ */
    printf("zclassic compat: compressed_txout P2PKH roundtrip... ");
    {
        struct tx_out orig;
        tx_out_set_null(&orig);
        orig.value = 123456789;
        uint8_t pkh[25] = {
            0x76, 0xa9, 0x14,
            0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
            0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
            0x0D, 0x0E, 0x0F, 0x10,
            0x88, 0xac
        };
        script_set(&orig.script_pub_key, pkh, 25);

        struct byte_stream ws;
        stream_init(&ws, 64);
        bool ok = compressed_txout_serialize(&orig, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct tx_out decoded;
        tx_out_set_null(&decoded);
        ok = ok && compressed_txout_deserialize(&decoded, &rs);
        ok = ok && (decoded.value == 123456789);
        ok = ok && (decoded.script_pub_key.size == 25);
        ok = ok && (memcmp(decoded.script_pub_key.data, pkh, 25) == 0);

        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: compressed_txout P2SH roundtrip... ");
    {
        struct tx_out orig;
        tx_out_set_null(&orig);
        orig.value = 5000000000LL;
        uint8_t p2sh[23] = {
            0xa9, 0x14,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
            0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
            0x01, 0x02, 0x03, 0x04,
            0x87
        };
        script_set(&orig.script_pub_key, p2sh, 23);

        struct byte_stream ws;
        stream_init(&ws, 64);
        bool ok = compressed_txout_serialize(&orig, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct tx_out decoded;
        tx_out_set_null(&decoded);
        ok = ok && compressed_txout_deserialize(&decoded, &rs);
        ok = ok && (decoded.value == 5000000000LL);
        ok = ok && (decoded.script_pub_key.size == 23);
        ok = ok && (memcmp(decoded.script_pub_key.data, p2sh, 23) == 0);

        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: compressed_txout non-standard script roundtrip... ");
    {
        /* A non-standard script that doesn't match P2PKH/P2SH/P2PK patterns */
        struct tx_out orig;
        tx_out_set_null(&orig);
        orig.value = 42;
        uint8_t weird[10] = {0x51, 0x52, 0x93, 0x87, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
        script_set(&orig.script_pub_key, weird, 10);

        struct byte_stream ws;
        stream_init(&ws, 64);
        bool ok = compressed_txout_serialize(&orig, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct tx_out decoded;
        tx_out_set_null(&decoded);
        ok = ok && compressed_txout_deserialize(&decoded, &rs);
        ok = ok && (decoded.value == 42);
        ok = ok && (decoded.script_pub_key.size == 10);
        ok = ok && (memcmp(decoded.script_pub_key.data, weird, 10) == 0);

        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPATIBILITY: script_compress / script_decompress
     * Verify all 5 special script types match C++ behavior
     * ================================================================ */
    printf("zclassic compat: script_compress P2SH... ");
    {
        struct script s;
        script_init(&s);
        uint8_t raw[23] = {
            0xa9, 0x14,
            0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
            0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB,
            0xAB, 0xAB, 0xAB, 0xAB,
            0x87
        };
        script_set(&s, raw, 23);
        unsigned char out[33];
        size_t out_len = 0;
        bool ok = script_compress(&s, out, &out_len);
        ok = ok && (out_len == 21);
        ok = ok && (out[0] == 0x01);  /* P2SH type */
        uint8_t hash20[20];
        memset(hash20, 0xAB, 20);
        ok = ok && (memcmp(out + 1, hash20, 20) == 0);

        /* Decompress back */
        struct script decoded;
        ok = ok && script_decompress(&decoded, 0x01, out + 1, 20);
        ok = ok && (decoded.size == 23);
        ok = ok && (memcmp(decoded.data, raw, 23) == 0);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: CalcMaskSize matches C++... ");
    {
        /* Test the nMaskSize/nMaskCode calculation matches C++ CalcMaskSize.
         * C++: nBytes = last non-zero byte position, nNonzeroBytes = count.
         * With vout[4] and vout[16] present (the coinbase test vector):
         * Byte 0 covers vout[2..9]: vout[4] present → byte has bit set → nonzero
         * Byte 1 covers vout[10..17]: vout[16] present → byte has bit set → nonzero
         * So nMaskSize=2 (highest nonzero byte pos), nMaskCode=2 (count of nonzero) */
        struct coins cc;
        coins_init(&cc);
        coins_alloc(&cc, 17);
        cc.vout[4].value = 234925952;
        uint8_t pk[1] = {0x76};
        script_set(&cc.vout[4].script_pub_key, pk, 1);
        cc.vout[16].value = 110397;
        script_set(&cc.vout[16].script_pub_key, pk, 1);

        unsigned int nMaskSize = 0, nMaskCode = 0;
        for (size_t vi = 2; vi < cc.num_vout; vi++) {
            if (!tx_out_is_null(&cc.vout[vi])) {
                unsigned int byte_pos = (unsigned int)((vi - 2) / 8) + 1;
                if (byte_pos > nMaskSize) nMaskSize = byte_pos;
            }
        }
        for (unsigned int mi = 0; mi < nMaskSize; mi++) {
            unsigned char ch = 0;
            for (unsigned int p = 0; p < 8; p++) {
                size_t idx = 2 + mi * 8 + p;
                if (idx < cc.num_vout && !tx_out_is_null(&cc.vout[idx]))
                    ch |= (1 << p);
            }
            if (ch != 0) nMaskCode++;
        }

        bool ok = (nMaskSize == 2) && (nMaskCode == 2);

        coins_free(&cc);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
