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

    return failures;
}
