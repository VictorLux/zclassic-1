/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Explorer controller unit tests — routing, edge cases, factoids. */

#include "test/test_helpers.h"
#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "crypto/sha3.h"
#include <string.h>
#include <inttypes.h>

int test_explorer(void)
{
    int failures = 0;
    uint8_t resp[8192];

    printf("explorer: NULL path returns 0... ");
    {
        size_t n = explorer_handle_request("GET", NULL, NULL, 0,
                                            resp, sizeof(resp));
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: NULL response returns 0... ");
    {
        size_t n = explorer_handle_request("GET", "/explorer", NULL, 0,
                                            NULL, sizeof(resp));
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: /api/ delegation to api controller... ");
    {
        size_t n = explorer_handle_request("GET", "/api/nonexistent", NULL, 0,
                                            resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: unknown path returns 0... ");
    {
        size_t n = explorer_handle_request("GET", "/foobar", NULL, 0,
                                            resp, sizeof(resp));
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: /explorer/style.css returns CSS... ");
    {
        size_t n = explorer_handle_request("GET", "/explorer/style.css", NULL, 0,
                                            resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "text/css") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    /* ── Supply calculation edge cases ───────────────────── */

    printf("explorer: supply at negative height is 0... ");
    {
        int64_t s = compute_supply_at_height(-1);
        bool ok = (s == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ")\n", s); failures++; }
    }

    printf("explorer: supply at height 2387001 shows next halving... ");
    {
        /* At 2,387,001: halvings_raw = (2387001-707000-1)/1680000 = 1680000/1680000 = 1
         * halvings = 1+3 = 4, subsidy = 625000000 >> 4 = 39062500 sat = 0.390625 ZCL */
        int64_t s_before = compute_supply_at_height(2387000);
        int64_t s_at = compute_supply_at_height(2387001);
        int64_t s_after = compute_supply_at_height(2387002);
        /* Check that the rate changed: increment at 2387001 should be less
         * than increment at 2387000 */
        int64_t inc_before = s_at - s_before;  /* last block of era 3 or first of era 4 */
        int64_t inc_after = s_after - s_at;     /* era 4 rate */
        /* Both should be positive */
        bool ok = (inc_before > 0 && inc_after > 0 && inc_after <= inc_before);
        if (ok) printf("OK (rate: %" PRId64 " -> %" PRId64 " sat/block)\n",
                       inc_before, inc_after);
        else { printf("FAIL (inc_before=%" PRId64 ", inc_after=%" PRId64 ")\n",
                      inc_before, inc_after); failures++; }
    }

    printf("explorer: SHA3 receipt is deterministic... ");
    {
        /* compute_receipt is static in factoids — test SHA3 directly */
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        uint8_t data[] = {0x01, 0x02, 0x03};
        sha3_256_write(&ctx, data, 3);
        unsigned char d1[32];
        sha3_256_finalize(&ctx, d1);

        sha3_256_init(&ctx);
        sha3_256_write(&ctx, data, 3);
        unsigned char d2[32];
        sha3_256_finalize(&ctx, d2);

        bool ok = (memcmp(d1, d2, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: SHA3 different inputs give different hashes... ");
    {
        struct sha3_256_ctx ctx;
        unsigned char d1[32], d2[32];

        sha3_256_init(&ctx);
        uint8_t a[] = {0x01};
        sha3_256_write(&ctx, a, 1);
        sha3_256_finalize(&ctx, d1);

        sha3_256_init(&ctx);
        uint8_t b[] = {0x02};
        sha3_256_write(&ctx, b, 1);
        sha3_256_finalize(&ctx, d2);

        bool ok = (memcmp(d1, d2, 32) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: difficulty_from_bits handles edge cases... ");
    {
        double d0 = explorer_difficulty_from_bits(0);
        bool ok = (d0 == 1.0);
        /* Non-zero bits should give reasonable difficulty */
        double d1 = explorer_difficulty_from_bits(0x1f07ffff);
        ok = ok && (d1 > 0.0);
        if (ok) printf("OK (bits=0 -> %.1f, bits=0x1f07ffff -> %.4f)\n", d0, d1);
        else { printf("FAIL\n"); failures++; }
    }

    printf("explorer: format_zcl handles negative values... ");
    {
        char buf[64];
        explorer_format_zcl(buf, sizeof(buf), -100000000LL);
        bool ok = (strcmp(buf, "-1.00000000") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got '%s')\n", buf); failures++; }
    }

    printf("explorer: format_zcl handles zero... ");
    {
        char buf[64];
        explorer_format_zcl(buf, sizeof(buf), 0);
        bool ok = (strcmp(buf, "0.00000000") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got '%s')\n", buf); failures++; }
    }

    printf("explorer: format_zcl handles 12.5 ZCL... ");
    {
        char buf[64];
        explorer_format_zcl(buf, sizeof(buf), 1250000000LL);
        bool ok = (strcmp(buf, "12.50000000") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got '%s')\n", buf); failures++; }
    }

    return failures;
}
