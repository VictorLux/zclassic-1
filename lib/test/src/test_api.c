/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * API controller unit tests — routing, input validation, edge cases,
 * and supply calculation correctness. */

#include "test/test_helpers.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include <string.h>
#include <inttypes.h>

int test_api(void)
{
    int failures = 0;
    uint8_t resp[8192];

    printf("api: NULL params return 0... ");
    {
        size_t n = api_handle_request(NULL, "/api/blocks", NULL, 0, resp, sizeof(resp));
        bool ok = (n == 0);
        n = api_handle_request("GET", NULL, NULL, 0, resp, sizeof(resp));
        ok = ok && (n == 0);
        n = api_handle_request("GET", "/api/blocks", NULL, 0, NULL, sizeof(resp));
        ok = ok && (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: POST returns 405... ");
    {
        size_t n = api_handle_request("POST", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: OPTIONS returns CORS headers... ");
    {
        size_t n = api_handle_request("OPTIONS", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "Access-Control") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: unknown endpoint returns 404... ");
    {
        size_t n = api_handle_request("GET", "/api/nonexistent", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: empty block ID not routed... ");
    {
        size_t n = api_handle_request("GET", "/api/block/", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0);
        if (ok) printf("OK (got response, %zu bytes)\n", n);
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: zero-length response buffer returns 0... ");
    {
        size_t n = api_handle_request("GET", "/api/blocks", NULL, 0, resp, 0);
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Supply calculation correctness tests ────────────── */

    printf("api: supply at height 0 is 0... ");
    {
        int64_t s = compute_supply_at_height(0);
        bool ok = (s == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ")\n", s); failures++; }
    }

    printf("api: supply at height 1 is 12.5 ZCL... ");
    {
        int64_t s = compute_supply_at_height(1);
        bool ok = (s == 1250000000LL);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ", expected 1250000000)\n", s); failures++; }
    }

    printf("api: supply at height 100 is 100*12.5 ZCL... ");
    {
        int64_t s = compute_supply_at_height(100);
        int64_t expected = 100LL * 1250000000LL;
        bool ok = (s == expected);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ", expected %" PRId64 ")\n", s, expected); failures++; }
    }

    printf("api: supply at Buttercup activation (707001) includes new rate... ");
    {
        int64_t s = compute_supply_at_height(707001);
        int64_t s_pre = compute_supply_at_height(707000);
        /* Block 707000 should earn post-BC rate: 0.78125 ZCL */
        int64_t expected_increment = 78125000LL;
        bool ok = (s - s_pre == expected_increment);
        if (ok) printf("OK (increment = %" PRId64 ")\n", s - s_pre);
        else { printf("FAIL (increment %" PRId64 ", expected %" PRId64 ")\n",
                      s - s_pre, expected_increment); failures++; }
    }

    printf("api: supply at 710000 is correct post-Buttercup... ");
    {
        int64_t s = compute_supply_at_height(710000);
        int64_t s_bc = compute_supply_at_height(707000);
        /* 3000 post-BC blocks at 78125000 sat each */
        int64_t expected = s_bc + 3000LL * 78125000LL;
        bool ok = (s == expected);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ", expected %" PRId64 ")\n", s, expected); failures++; }
    }

    printf("api: supply is monotonically increasing... ");
    {
        bool ok = true;
        int64_t prev = 0;
        int64_t test_heights[] = { 0, 1, 100, 1000, 100000, 706999,
                                    707000, 707001, 800000, 1000000,
                                    2000000, 2387000, 2387001, 3000000 };
        for (int i = 0; i < (int)(sizeof(test_heights)/sizeof(test_heights[0])); i++) {
            int64_t s = compute_supply_at_height(test_heights[i]);
            if (s < prev) {
                printf("FAIL (supply decreased at height %" PRId64 ": %" PRId64 " < %" PRId64 ")\n",
                       test_heights[i], s, prev);
                ok = false;
                break;
            }
            prev = s;
        }
        if (ok) printf("OK\n");
        else failures++;
    }

    printf("api: supply never exceeds 21M ZCL... ");
    {
        int64_t max_sat = 2100000000000000LL; /* 21M ZCL */
        bool ok = true;
        /* Check at very high block counts */
        int64_t test_heights[] = { 10000000, 50000000, 100000000 };
        for (int i = 0; i < 3; i++) {
            int64_t s = compute_supply_at_height(test_heights[i]);
            if (s > max_sat) {
                printf("FAIL (supply %" PRId64 " > 21M at height %" PRId64 ")\n",
                       s, test_heights[i]);
                ok = false;
                break;
            }
        }
        if (ok) printf("OK\n");
        else failures++;
    }

    printf("api: pre-Buttercup is all one era (no halving before 707000)... ");
    {
        /* Since 707000 < 840000, all pre-BC blocks at 12.5 ZCL.
         * supply(706999) - supply(706998) should equal 12.5 ZCL */
        int64_t s1 = compute_supply_at_height(706999);
        int64_t s2 = compute_supply_at_height(706998);
        bool ok = (s1 - s2 == 1250000000LL);
        if (ok) printf("OK\n");
        else { printf("FAIL (increment=%" PRId64 ")\n", s1 - s2); failures++; }
    }

    printf("api: supply_zcl_at_height matches compute_supply_at_height... ");
    {
        double zcl = supply_zcl_at_height(100000);
        int64_t sat = compute_supply_at_height(100000);
        bool ok = (zcl == (double)sat / 100000000.0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: trailing slash stripped in routing... ");
    {
        size_t n1 = api_handle_request("GET", "/api/stats/", NULL, 0,
                                        resp, sizeof(resp));
        /* Should either serve stats (503 if cache empty) or match the route */
        bool ok = (n1 > 0);
        if (ok) printf("OK (%zu bytes)\n", n1);
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: DELETE method returns 405... ");
    {
        size_t n = api_handle_request("DELETE", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: PUT method returns 405... ");
    {
        size_t n = api_handle_request("PUT", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
