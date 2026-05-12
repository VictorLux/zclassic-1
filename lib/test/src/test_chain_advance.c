/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Smoke tests for the chain_advance contract surface in
 * app/services/include/services/chain_advance.h.
 *
 * The body of chain_advance currently delegates to connect_tip
 * (Move 2 follow-up commits will replace it with the two-step
 * disk-write protocol described in the header). These tests
 * lock in the *contract*: result-enum names, null-input handling,
 * and the surface that future commits will preserve.
 *
 * Crash-injection atomicity tests live in
 * lib/test/src/test_chain_advance_atomicity.c (NOT YET LANDED —
 * arrives with the implementation that earns it). */

#include "test/test_helpers.h"
#include "services/chain_advance.h"

#include <string.h>

static int test_chain_advance_result_names(void)
{
    int failures = 0;
    TEST("chain_advance: result-name table covers every defined value") {
        for (int r = 0; r < CA_NUM_RESULTS; r++) {
            const char *n = chain_advance_result_name((enum chain_advance_result)r);
            if (strcmp(n, "unknown") == 0) {
                printf("FAIL (result %d returned 'unknown')\n", r);
                failures++; goto _test_next;
            }
        }
        /* Out-of-range must be reported as unknown. */
        ASSERT(strcmp(chain_advance_result_name((enum chain_advance_result)CA_NUM_RESULTS),
                      "unknown") == 0);
        ASSERT(strcmp(chain_advance_result_name((enum chain_advance_result)-1),
                      "unknown") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_chain_advance_null_input_rejected(void)
{
    int failures = 0;
    TEST("chain_advance: NULL required inputs return CA_REJECTED_VALIDATION") {
        /* Each required arg in turn nulled; never enters connect_tip
         * (which would dereference these). The guard at the top of
         * chain_advance is part of the contract — callers can pass
         * a partially-constructed state and expect a clean reject. */
        enum chain_advance_result r;

        r = chain_advance(NULL, NULL, NULL, NULL, NULL, NULL, NULL, "test");
        ASSERT(r == CA_REJECTED_VALIDATION);

        /* Provide ms but nothing else — still rejected. */
        struct main_state ms_stub;
        memset(&ms_stub, 0, sizeof(ms_stub));
        r = chain_advance(NULL, &ms_stub, NULL, NULL, NULL, NULL, NULL, "test");
        ASSERT(r == CA_REJECTED_VALIDATION);

        PASS();
    } _test_next:;
    return failures;
}

int test_chain_advance(void)
{
    int failures = 0;
    failures += test_chain_advance_result_names();
    failures += test_chain_advance_null_input_rejected();
    return failures;
}
