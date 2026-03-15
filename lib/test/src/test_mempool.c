/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

int test_mempool(void)
{
    int failures = 0;

    printf("txmempool init/free... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);
        bool ok = tx_mempool_size(&pool) == 0;
        ok = ok && tx_mempool_total_size(&pool) == 0;
        ok = ok && tx_mempool_txs_updated(&pool) == 0;
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool add/exists/lookup... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0xAB, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 50 * COIN_VALUE;
        tx.lock_time = 0;
        transaction_compute_hash(&tx);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 10000, 1700000000, 1e9, 100,
                           true, false, 0);

        bool ok = tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
        ok = ok && tx_mempool_size(&pool) == 1;
        ok = ok && tx_mempool_exists(&pool, &tx.hash);
        ok = ok && tx_mempool_total_size(&pool) > 0;

        struct transaction found;
        transaction_init(&found);
        ok = ok && tx_mempool_lookup(&pool, &tx.hash, &found);
        ok = ok && uint256_eq(&found.hash, &tx.hash);
        ok = ok && found.vout[0].value == 50 * COIN_VALUE;

        transaction_free(&found);
        mempool_entry_free(&entry);
        transaction_free(&tx);
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool remove... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0xCD, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 25 * COIN_VALUE;
        transaction_compute_hash(&tx);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 5000, 1700000000, 1e8, 200,
                           true, false, 0);
        tx_mempool_add_unchecked(&pool, &tx.hash, &entry);

        bool ok = tx_mempool_size(&pool) == 1;
        tx_mempool_remove(&pool, &tx.hash);
        ok = ok && tx_mempool_size(&pool) == 0;
        ok = ok && !tx_mempool_exists(&pool, &tx.hash);

        mempool_entry_free(&entry);
        transaction_free(&tx);
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool clear... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        for (int i = 0; i < 5; i++) {
            struct transaction tx;
            transaction_init(&tx);
            transaction_alloc(&tx, 1, 1);
            memset(tx.vin[0].prevout.hash.data, (unsigned char)(i + 1), 32);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].sequence = 0xFFFFFFFF;
            tx.vout[0].value = (int64_t)(i + 1) * COIN_VALUE;
            transaction_compute_hash(&tx);

            struct mempool_entry entry;
            mempool_entry_init(&entry, &tx, 1000, 1700000000, 1e6, 100,
                               true, false, 0);
            tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
            mempool_entry_free(&entry);
            transaction_free(&tx);
        }

        bool ok = tx_mempool_size(&pool) == 5;
        tx_mempool_clear(&pool);
        ok = ok && tx_mempool_size(&pool) == 0;
        ok = ok && tx_mempool_total_size(&pool) == 0;

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool prioritise/apply_deltas... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct uint256 hash;
        memset(hash.data, 0xEE, 32);

        tx_mempool_prioritise(&pool, &hash, 100.0, 5000);

        double pd = 0.0;
        int64_t fd = 0;
        tx_mempool_apply_deltas(&pool, &hash, &pd, &fd);
        bool ok = (pd == 100.0 && fd == 5000);

        tx_mempool_prioritise(&pool, &hash, 50.0, 2000);
        pd = 0.0; fd = 0;
        tx_mempool_apply_deltas(&pool, &hash, &pd, &fd);
        ok = ok && (pd == 150.0 && fd == 7000);

        tx_mempool_clear_prioritisation(&pool, &hash);
        pd = 0.0; fd = 0;
        tx_mempool_apply_deltas(&pool, &hash, &pd, &fd);
        ok = ok && (pd == 0.0 && fd == 0);

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool query_hashes... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        for (int i = 0; i < 3; i++) {
            struct transaction tx;
            transaction_init(&tx);
            transaction_alloc(&tx, 1, 1);
            memset(tx.vin[0].prevout.hash.data, (unsigned char)(0x10 + i), 32);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].sequence = 0xFFFFFFFF;
            tx.vout[0].value = COIN_VALUE;
            transaction_compute_hash(&tx);

            struct mempool_entry entry;
            mempool_entry_init(&entry, &tx, 1000, 1700000000, 1e6, 100,
                               true, false, 0);
            tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
            mempool_entry_free(&entry);
            transaction_free(&tx);
        }

        struct uint256 out[10];
        size_t num_out = 0;
        tx_mempool_query_hashes(&pool, out, 10, &num_out);
        bool ok = (num_out == 3);

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool remove_without_branch_id... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        for (int i = 0; i < 4; i++) {
            struct transaction tx;
            transaction_init(&tx);
            transaction_alloc(&tx, 1, 1);
            memset(tx.vin[0].prevout.hash.data, (unsigned char)(0x20 + i), 32);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].sequence = 0xFFFFFFFF;
            tx.vout[0].value = COIN_VALUE;
            transaction_compute_hash(&tx);

            struct mempool_entry entry;
            mempool_entry_init(&entry, &tx, 1000, 1700000000, 1e6, 100,
                               true, false, (i < 2) ? 0x76b809bbU : 0x892f2085U);
            tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
            mempool_entry_free(&entry);
            transaction_free(&tx);
        }

        bool ok = tx_mempool_size(&pool) == 4;
        tx_mempool_remove_without_branch_id(&pool, 0x892f2085U);
        ok = ok && tx_mempool_size(&pool) == 2;

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool has_no_inputs_of... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct transaction tx1;
        transaction_init(&tx1);
        transaction_alloc(&tx1, 1, 1);
        memset(tx1.vin[0].prevout.hash.data, 0x30, 32);
        tx1.vin[0].prevout.n = 0;
        tx1.vin[0].sequence = 0xFFFFFFFF;
        tx1.vout[0].value = COIN_VALUE;
        transaction_compute_hash(&tx1);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx1, 1000, 1700000000, 1e6, 100,
                           true, false, 0);
        tx_mempool_add_unchecked(&pool, &tx1.hash, &entry);

        struct transaction tx2;
        transaction_init(&tx2);
        transaction_alloc(&tx2, 1, 1);
        tx2.vin[0].prevout.hash = tx1.hash;
        tx2.vin[0].prevout.n = 0;
        tx2.vin[0].sequence = 0xFFFFFFFF;
        tx2.vout[0].value = COIN_VALUE;
        transaction_compute_hash(&tx2);

        bool ok = !tx_mempool_has_no_inputs_of(&pool, &tx2);

        struct transaction tx3;
        transaction_init(&tx3);
        transaction_alloc(&tx3, 1, 1);
        memset(tx3.vin[0].prevout.hash.data, 0xFF, 32);
        tx3.vin[0].prevout.n = 0;
        tx3.vin[0].sequence = 0xFFFFFFFF;
        tx3.vout[0].value = COIN_VALUE;
        transaction_compute_hash(&tx3);

        ok = ok && tx_mempool_has_no_inputs_of(&pool, &tx3);

        mempool_entry_free(&entry);
        transaction_free(&tx1);
        transaction_free(&tx2);
        transaction_free(&tx3);
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("mempool_entry get_priority... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0x40, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 10 * COIN_VALUE;
        transaction_compute_hash(&tx);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 50000, 1700000000, 1000.0, 100,
                           true, false, 0);

        double p = mempool_entry_get_priority(&entry, 200);
        bool ok = (p > 1000.0);

        mempool_entry_free(&entry);
        transaction_free(&tx);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("tx_confirm_stats init/setup/find_bucket... ");
    {
        struct tx_confirm_stats s;
        tx_confirm_stats_init(&s);
        double bkts[] = {10.0, 100.0, 1000.0, 10000.0};
        tx_confirm_stats_setup(&s, bkts, 4, 10, 0.998);

        bool ok = s.num_buckets == 5;
        ok = ok && s.max_confirms == 10;

        ok = ok && tx_confirm_stats_find_bucket(&s, 5.0) == 0;
        ok = ok && tx_confirm_stats_find_bucket(&s, 10.0) == 0;
        ok = ok && tx_confirm_stats_find_bucket(&s, 50.0) == 1;
        ok = ok && tx_confirm_stats_find_bucket(&s, 5000.0) == 3;
        ok = ok && tx_confirm_stats_find_bucket(&s, 99999.0) == 4;

        tx_confirm_stats_free(&s);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("tx_confirm_stats record/update... ");
    {
        struct tx_confirm_stats s;
        tx_confirm_stats_init(&s);
        double bkts[] = {100.0, 1000.0, 10000.0};
        tx_confirm_stats_setup(&s, bkts, 3, 5, 0.998);

        tx_confirm_stats_clear_current(&s, 1);
        tx_confirm_stats_record(&s, 1, 500.0);
        tx_confirm_stats_record(&s, 2, 500.0);
        tx_confirm_stats_update_averages(&s);

        bool ok = s.tx_ct_avg[1] > 0;
        ok = ok && s.avg[1] > 0;

        tx_confirm_stats_free(&s);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("block_policy_estimator init/free... ");
    {
        struct fee_rate min_fee;
        min_fee.satoshis_per_k = 1000;
        struct block_policy_estimator est;
        block_policy_estimator_init(&est, &min_fee);

        bool ok = est.best_seen_height == 0;
        ok = ok && est.fee_stats.num_buckets > 0;
        ok = ok && est.pri_stats.num_buckets > 0;
        ok = ok && est.min_tracked_fee.satoshis_per_k >= 10;

        block_policy_estimator_free(&est);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("block_policy_estimator estimate_fee empty... ");
    {
        struct fee_rate min_fee;
        min_fee.satoshis_per_k = 1000;
        struct block_policy_estimator est;
        block_policy_estimator_init(&est, &min_fee);

        struct fee_rate r = policy_estimate_fee(&est, 2);
        bool ok = r.satoshis_per_k == 0;
        double p = policy_estimate_priority(&est, 2);
        ok = ok && p == -1;

        block_policy_estimator_free(&est);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("policy is_fee/pri_data_point... ");
    {
        struct fee_rate min_fee;
        min_fee.satoshis_per_k = 1000;
        struct block_policy_estimator est;
        block_policy_estimator_init(&est, &min_fee);

        struct fee_rate high_fee;
        high_fee.satoshis_per_k = 50000;
        bool ok = policy_is_fee_data_point(&est, &high_fee, 0.0);

        struct fee_rate zero_fee;
        zero_fee.satoshis_per_k = 0;
        ok = ok && policy_is_pri_data_point(&est, &zero_fee, 1e12);

        block_policy_estimator_free(&est);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
