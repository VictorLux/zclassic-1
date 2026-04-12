/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for RPC error envelope consistency (wave 10 #8).
 * Verifies that json_rpc_error, json_rpc_error_full, and
 * json_rpc_error_response all produce the target shape:
 *   {error: {code, message[, method]}}
 */

#include "test/test_helpers.h"
#include "rpc/protocol.h"
#include "json/json.h"
#include <string.h>
#include <stdio.h>

int test_rpc_error_envelope(void)
{
    int failures = 0;

    /* ── json_rpc_error: basic {code, message} ──────────────── */

    printf("rpc_error basic shape... ");
    {
        struct json_value err;
        json_rpc_error(&err, -32601, "Method not found");
        bool ok = err.type == JSON_OBJ;
        const struct json_value *code = json_get(&err, "code");
        const struct json_value *msg = json_get(&err, "message");
        ok = ok && code && code->type == JSON_INT && json_get_int(code) == -32601;
        ok = ok && msg && msg->type == JSON_STR
                && strcmp(json_get_str(msg), "Method not found") == 0;
        /* No method field in basic version */
        ok = ok && json_get(&err, "method") == NULL;
        json_free(&err);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── json_rpc_error_full: {code, message, method} ────────── */

    printf("rpc_error_full with method... ");
    {
        struct json_value err;
        json_rpc_error_full(&err, -32601, "Method not found", "getinfo");
        bool ok = err.type == JSON_OBJ;
        const struct json_value *code = json_get(&err, "code");
        const struct json_value *msg = json_get(&err, "message");
        const struct json_value *meth = json_get(&err, "method");
        ok = ok && code && json_get_int(code) == -32601;
        ok = ok && msg && strcmp(json_get_str(msg), "Method not found") == 0;
        ok = ok && meth && strcmp(json_get_str(meth), "getinfo") == 0;
        json_free(&err);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("rpc_error_full with NULL method omits field... ");
    {
        struct json_value err;
        json_rpc_error_full(&err, -32700, "Parse error", NULL);
        bool ok = err.type == JSON_OBJ;
        ok = ok && json_get(&err, "code") != NULL;
        ok = ok && json_get(&err, "message") != NULL;
        ok = ok && json_get(&err, "method") == NULL;
        json_free(&err);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── json_rpc_error_response: full string output ──────────── */

    printf("error_response with method produces valid JSON... ");
    {
        char buf[512];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            -32601, "Method not found", "getinfo", "1");
        bool ok = n > 0 && n < sizeof(buf);
        /* Parse and verify structure */
        struct json_value v;
        ok = ok && json_read(&v, buf, n);
        if (ok) {
            const struct json_value *res = json_get(&v, "result");
            const struct json_value *err = json_get(&v, "error");
            const struct json_value *id = json_get(&v, "id");
            ok = ok && res && res->type == JSON_NULL;
            ok = ok && err && err->type == JSON_OBJ;
            ok = ok && id && json_get_int(id) == 1;
            if (err && err->type == JSON_OBJ) {
                ok = ok && json_get_int(json_get(err, "code")) == -32601;
                const struct json_value *m = json_get(err, "message");
                ok = ok && m && strcmp(json_get_str(m), "Method not found") == 0;
                const struct json_value *mt = json_get(err, "method");
                ok = ok && mt && strcmp(json_get_str(mt), "getinfo") == 0;
            }
        }
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("error_response without method omits method field... ");
    {
        char buf[512];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            -32700, "Parse error", NULL, NULL);
        bool ok = n > 0;
        struct json_value v;
        ok = ok && json_read(&v, buf, n);
        if (ok) {
            const struct json_value *err = json_get(&v, "error");
            ok = ok && err && err->type == JSON_OBJ;
            if (err) {
                ok = ok && json_get_int(json_get(err, "code")) == -32700;
                ok = ok && json_get(err, "method") == NULL;
            }
            const struct json_value *id = json_get(&v, "id");
            ok = ok && id && id->type == JSON_NULL;
        }
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("error_response with string id... ");
    {
        char buf[512];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            -32600, "Invalid request", NULL, "\"abc\"");
        bool ok = n > 0;
        struct json_value v;
        ok = ok && json_read(&v, buf, n);
        if (ok) {
            const struct json_value *id = json_get(&v, "id");
            ok = ok && id && id->type == JSON_STR
                  && strcmp(json_get_str(id), "abc") == 0;
        }
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Error code constants ─────────────────────────────────── */

    printf("RPC error code constants match JSON-RPC spec... ");
    {
        bool ok = (RPC_PARSE_ERROR == -32700);
        ok = ok && (RPC_INVALID_REQUEST == -32600);
        ok = ok && (RPC_METHOD_NOT_FOUND == -32601);
        ok = ok && (RPC_INVALID_PARAMS == -32602);
        ok = ok && (RPC_INTERNAL_ERROR == -32603);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── All pre-dispatch error shapes ─────────────────────────── */

    printf("ban error: has code + message... ");
    {
        char buf[256];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            -32003, "IP banned", NULL, NULL);
        struct json_value v;
        bool ok = json_read(&v, buf, n);
        if (ok) {
            const struct json_value *err = json_get(&v, "error");
            ok = ok && err && err->type == JSON_OBJ;
            ok = ok && json_get(err, "code") != NULL;
            ok = ok && json_get(err, "message") != NULL;
            ok = ok && json_get(&v, "result") != NULL;
        }
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("rate limit error: has code + message... ");
    {
        char buf[256];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            -32005, "Rate limit exceeded", NULL, NULL);
        struct json_value v;
        bool ok = json_read(&v, buf, n);
        if (ok) {
            const struct json_value *err = json_get(&v, "error");
            ok = ok && err && err->type == JSON_OBJ;
            ok = ok && json_get_int(json_get(err, "code")) == -32005;
        }
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("warmup error: includes method field... ");
    {
        struct json_value err;
        json_rpc_error_full(&err, RPC_IN_WARMUP, "Loading block index...",
                            "getblockcount");
        bool ok = err.type == JSON_OBJ;
        const struct json_value *meth = json_get(&err, "method");
        ok = ok && meth && strcmp(json_get_str(meth), "getblockcount") == 0;
        ok = ok && json_get_int(json_get(&err, "code")) == RPC_IN_WARMUP;
        json_free(&err);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("method-not-found error: includes method field... ");
    {
        struct json_value err;
        json_rpc_error_full(&err, RPC_METHOD_NOT_FOUND, "Method not found",
                            "nonexistent");
        bool ok = err.type == JSON_OBJ;
        const struct json_value *meth = json_get(&err, "method");
        ok = ok && meth && strcmp(json_get_str(meth), "nonexistent") == 0;
        json_free(&err);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("OOM error: includes method when available... ");
    {
        char buf[512];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            RPC_OUT_OF_MEMORY, "Internal error: out of memory",
            "z_sendmany", "42");
        struct json_value v;
        bool ok = json_read(&v, buf, n);
        if (ok) {
            const struct json_value *err = json_get(&v, "error");
            ok = ok && err && err->type == JSON_OBJ;
            const struct json_value *mt = json_get(err, "method");
            ok = ok && mt && strcmp(json_get_str(mt), "z_sendmany") == 0;
            ok = ok && json_get_int(json_get(&v, "id")) == 42;
        }
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── Buffer edge cases ────────────────────────────────────── */

    printf("error_response truncates safely on small buffer... ");
    {
        char buf[32];
        size_t n = json_rpc_error_response(buf, sizeof(buf),
            -32700, "Parse error", NULL, NULL);
        /* Should not crash, output truncated */
        bool ok = n <= sizeof(buf) - 1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("error_response zero buffer returns 0... ");
    {
        size_t n = json_rpc_error_response(NULL, 0, -1, "x", NULL, NULL);
        bool ok = (n == 0);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("\n%d rpc error envelope tests, %d failed\n",
           15, failures);
    return failures;
}
