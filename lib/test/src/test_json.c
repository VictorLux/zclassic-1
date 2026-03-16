/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
#include "test/test_helpers.h"

int test_json(void)
{
    int failures = 0;

    printf("json parse integer... ");
    {
        struct json_value v;
        bool ok = json_read(&v, "42", 2);
        ok = ok && (v.type == JSON_INT) && (json_get_int(&v) == 42);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json parse string... ");
    {
        struct json_value v;
        bool ok = json_read(&v, "\"hello\"", 7);
        ok = ok && (v.type == JSON_STR) && (strcmp(json_get_str(&v), "hello") == 0);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json parse bool... ");
    {
        struct json_value v;
        bool ok = json_read(&v, "true", 4) && json_get_bool(&v);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json parse null... ");
    {
        struct json_value v;
        bool ok = json_read(&v, "null", 4) && (v.type == JSON_NULL);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json parse array... ");
    {
        struct json_value v;
        bool ok = json_read(&v, "[1,2,3]", 7);
        ok = ok && (v.type == JSON_ARR) && (json_size(&v) == 3);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json parse object... ");
    {
        struct json_value v;
        const char *s = "{\"name\":\"ZCL\",\"height\":3045000}";
        bool ok = json_read(&v, s, strlen(s));
        ok = ok && (v.type == JSON_OBJ);
        const struct json_value *h = json_get(&v, "height");
        ok = ok && h && (json_get_int(h) == 3045000);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json build object + write... ");
    {
        struct json_value v;
        json_set_object(&v);
        json_push_kv_str(&v, "ticker", "ZTEST");
        json_push_kv_int(&v, "supply", 1000);
        char buf[256];
        json_write(&v, buf, sizeof(buf));
        bool ok = (strstr(buf, "ZTEST") != NULL) && (strstr(buf, "1000") != NULL);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL (%s)\n", buf); failures++; }
    }

    printf("json parse nested object... ");
    {
        const char *s = "{\"a\":{\"b\":42}}";
        struct json_value v;
        bool ok = json_read(&v, s, strlen(s));
        const struct json_value *a = json_get(&v, "a");
        ok = ok && a && (a->type == JSON_OBJ);
        const struct json_value *b = json_get(a, "b");
        ok = ok && b && (json_get_int(b) == 42);
        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
