/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * P14.3 RED: `getsyncdiag` RPC crashes via `json_free` on uninitialized
 * stack memory.
 *
 * The bug: `rpc_getsyncdiag` in `app/controllers/src/health_controller.c`
 * declares `struct json_value wd;` (and `hdr`) without `json_init()` or
 * `= {0}`. `json_set_object(&wd)` internally calls `json_free(&wd)`,
 * which reads uninitialized `type`, `num_children`, and `children` —
 * typically crashing with SIGSEGV/SIGABRT once the stack region holds
 * non-zero residue from earlier RPCs (which is always the case on a
 * live node).
 *
 * This test dirties the lower stack with 0xCC before calling the RPC
 * to force the uninitialized read to observe garbage in a fresh test
 * process, making the repro deterministic. Post-fix (wd/hdr explicitly
 * zero-initialized), the RPC must return a well-formed JSON object
 * containing non-empty `watchdog` and `headers` sub-objects. */

#include "test/test_helpers.h"
#include "controllers/health_controller.h"
#include "rpc/httpserver.h"
#include "rpc/server.h"
#include "json/json.h"
#include <string.h>
#include <stdio.h>

/* Push a 64 KiB frame filled with 0xCC onto the stack, then return.
 * The frame is freed on return but the bytes persist in memory — any
 * subsequent callee with a smaller combined frame size reuses that
 * region, observing 0xCC where `= {0}` would have given zeros. */
static __attribute__((noinline)) void dirty_stack_region(void)
{
    volatile unsigned char junk[65536];
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = 0xCC;
    /* Force the compiler to materialize the writes. */
    __asm__ volatile("" : : "r"(junk) : "memory");
}

int test_syncdiag_rpc(void)
{
    int failures = 0;

    printf("rpc_getsyncdiag: returns valid JSON without abort "
           "(P14.3 RED)... ");
    {
        dirty_stack_region();

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        rpc_health_set_state(NULL, NULL, NULL, NULL);
        register_health_rpc_commands(&tbl);

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "getsyncdiag",
                                          &params, &result);

        bool ok = executed && result.type == JSON_OBJ;

        const struct json_value *wd  = json_get(&result, "watchdog");
        const struct json_value *hdr = json_get(&result, "headers");
        ok = ok && wd  && wd->type  == JSON_OBJ && wd->num_children  > 0;
        ok = ok && hdr && hdr->type == JSON_OBJ && hdr->num_children > 0;

        ok = ok && json_get(&result, "sync_state")         != NULL;
        ok = ok && json_get(&result, "chain_height")       != NULL;
        ok = ok && json_get(&result, "best_header_height") != NULL;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_http response envelope: dirty stack still builds JSON "
           "(P24.11 RED)... ");
    {
        dirty_stack_region();

        struct json_value result;
        json_init(&result);
        json_set_object(&result);
        json_push_kv_str(&result, "watchdog", "ok");

        struct json_value id;
        json_init(&id);
        json_set_int(&id, 1);

        struct json_value response;
        bool ok = rpc_http_test_build_response_envelope(
            true, "getsyncdiag", &result, &id, &response);

        ok = ok && response.type == JSON_OBJ;
        ok = ok && json_get(&response, "result") != NULL;
        ok = ok && json_get(&response, "error") != NULL;
        ok = ok && json_get(&response, "id") != NULL;

        json_free(&result);
        json_free(&id);
        json_free(&response);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    return failures;
}
