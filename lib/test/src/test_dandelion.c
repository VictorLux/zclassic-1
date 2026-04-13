/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "net/dandelion.h"
#include "core/uint256.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static struct uint256 make_test_hash(uint8_t seed)
{
    struct uint256 h;
    memset(h.data, seed, 32);
    return h;
}

int test_dandelion(void)
{
    int failures = 0;

    /* ── init / free ───────────────────────────────────────────── */
    printf("dandelion_init / dandelion_free... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        if (ds.enabled && ds.num_stem_peers == 0 && ds.stempool_count == 0)
            printf("OK\n");
        else {
            printf("FAIL\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── should_stem returns false when disabled ───────────────── */
    printf("dandelion_should_stem disabled... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.enabled = false;
        bool stem = dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE);
        if (!stem)
            printf("OK\n");
        else {
            printf("FAIL (expected false when disabled)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── should_stem returns false when no stem peers ──────────── */
    printf("dandelion_should_stem no peers... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.enabled = true;
        ds.num_stem_peers = 0;
        bool stem = dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE);
        if (!stem)
            printf("OK\n");
        else {
            printf("FAIL (expected false with no stem peers)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── should_stem returns true ~90% when stem peers exist ───── */
    printf("dandelion_should_stem probability... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.enabled = true;
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 100;
        ds.stem_peers[1] = 200;

        int stem_count = 0;
        int trials = 1000;
        for (int i = 0; i < trials; i++) {
            if (dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE))
                stem_count++;
        }
        /* Should be ~90% stem (DANDELION_FLUFF_PROB = 10%) */
        double pct = (double)stem_count / trials * 100.0;
        if (pct > 80.0 && pct < 97.0)
            printf("OK (%.1f%% stem)\n", pct);
        else {
            printf("FAIL (%.1f%% stem, expected ~90%%)\n", pct); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── get_stem_peer round-robin ─────────────────────────────── */
    printf("dandelion_get_stem_peer round-robin... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;
        ds.stem_rr_index = 0;

        node_id_t p1 = dandelion_get_stem_peer(&ds, DANDELION_NODE_ID_NONE);
        node_id_t p2 = dandelion_get_stem_peer(&ds, DANDELION_NODE_ID_NONE);
        /* Round-robin should alternate */
        if (p1 == 10 && p2 == 20)
            printf("OK\n");
        else {
            printf("FAIL (got %d, %d expected 10, 20)\n", p1, p2); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── get_stem_peer skips from_peer ─────────────────────────── */
    printf("dandelion_get_stem_peer skips sender... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;
        ds.stem_rr_index = 0;

        /* from_peer=10, should skip to 20 */
        node_id_t p = dandelion_get_stem_peer(&ds, 10);
        if (p == 20)
            printf("OK\n");
        else {
            printf("FAIL (got %d, expected 20)\n", p); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── get_stem_peer returns NONE when only peer is sender ───── */
    printf("dandelion_get_stem_peer all excluded... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 1;
        ds.stem_peers[0] = 10;

        node_id_t p = dandelion_get_stem_peer(&ds, 10);
        if (p == DANDELION_NODE_ID_NONE)
            printf("OK\n");
        else {
            printf("FAIL (got %d, expected NONE)\n", p); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── stempool add / contains / remove ──────────────────────── */
    printf("dandelion_stempool basic ops... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h1 = make_test_hash(0xAA);
        struct uint256 h2 = make_test_hash(0xBB);

        dandelion_stempool_add(&ds, &h1, 1);
        dandelion_stempool_add(&ds, &h2, 2);

        bool c1 = dandelion_stempool_contains(&ds, &h1);
        bool c2 = dandelion_stempool_contains(&ds, &h2);
        struct uint256 h3 = make_test_hash(0xCC);
        bool c3 = dandelion_stempool_contains(&ds, &h3);

        if (c1 && c2 && !c3 && ds.stempool_count == 2)
            printf("OK\n");
        else {
            printf("FAIL (c1=%d c2=%d c3=%d count=%d)\n",
                   c1, c2, c3, ds.stempool_count);
            failures++;
        }

        /* Remove h1 */
        bool removed = dandelion_stempool_remove(&ds, &h1);
        bool still = dandelion_stempool_contains(&ds, &h1);
        if (removed && !still && ds.stempool_count == 1)
            printf("  remove OK\n");
        else {
            printf("  remove FAIL\n"); failures++;
        }

        dandelion_free(&ds);
    }

    /* ── stempool duplicate rejection ──────────────────────────── */
    printf("dandelion_stempool dedup... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xDD);
        dandelion_stempool_add(&ds, &h, 1);
        dandelion_stempool_add(&ds, &h, 2); /* duplicate */

        if (ds.stempool_count == 1)
            printf("OK\n");
        else {
            printf("FAIL (count=%d, expected 1)\n", ds.stempool_count);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── stempool embargo expiry ───────────────────────────────── */
    printf("dandelion_stempool embargo check... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xEE);
        dandelion_stempool_add(&ds, &h, 1);

        /* Force the embargo time to be in the past */
        for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
            if (ds.stempool[i].active) {
                ds.stempool[i].embargo_time = (int64_t)time(NULL) - 1;
                break;
            }
        }

        struct uint256 expired[8];
        int nexp = dandelion_stempool_check_embargo(&ds, expired, 8);
        if (nexp == 1 && uint256_eq(&expired[0], &h) &&
            ds.stempool_count == 0)
            printf("OK\n");
        else {
            printf("FAIL (nexp=%d count=%d)\n", nexp, ds.stempool_count);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── stempool eviction when full ───────────────────────────── */
    printf("dandelion_stempool eviction... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        /* Fill the stempool — use unique hashes via 4-byte encoding */
        for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
            struct uint256 h;
            memset(h.data, 0, 32);
            memcpy(h.data, &i, sizeof(i)); /* unique via int bytes */
            dandelion_stempool_add(&ds, &h, 1);
        }

        if (ds.stempool_count != DANDELION_MAX_STEMPOOL) {
            printf("FAIL (initial count=%d)\n", ds.stempool_count);
            failures++;
        } else {
            /* Add one more — should evict oldest */
            struct uint256 extra = make_test_hash(0xFF);
            dandelion_stempool_add(&ds, &extra, 1);

            bool has_extra = dandelion_stempool_contains(&ds, &extra);
            if (has_extra && ds.stempool_count == DANDELION_MAX_STEMPOOL)
                printf("OK\n");
            else {
                printf("FAIL (has_extra=%d count=%d)\n",
                       has_extra, ds.stempool_count);
                failures++;
            }
        }
        dandelion_free(&ds);
    }

    /* ── NULL safety ───────────────────────────────────────────── */
    printf("dandelion NULL safety... ");
    {
        /* These should not crash */
        dandelion_should_stem(NULL, 0);
        dandelion_get_stem_peer(NULL, 0);
        dandelion_stempool_add(NULL, NULL, 0);
        dandelion_stempool_remove(NULL, NULL);
        dandelion_stempool_contains(NULL, NULL);
        dandelion_stempool_check_embargo(NULL, NULL, 0);
        dandelion_maybe_rotate_epoch(NULL, NULL);
        printf("OK\n");
    }

    /* ── stats tracking ────────────────────────────────────────── */
    printf("dandelion stats... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 1;
        ds.stem_peers[0] = 42;
        ds.enabled = true;

        /* Run some decisions */
        for (int i = 0; i < 100; i++)
            dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE);

        /* Stats are tracked by the caller (process_tx_msg), not by
         * should_stem directly, so just verify fields exist and are
         * zero-initialized. */
        if (ds.stat_stem_sent == 0 && ds.stat_fluffed == 0 &&
            ds.stat_embargo_fluff == 0)
            printf("OK\n");
        else {
            printf("FAIL (stats not zero-initialized)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    return failures;
}
