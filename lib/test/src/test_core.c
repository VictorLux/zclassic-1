/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Core module tests: uint256, hashing, serialization, amount. */

#include "test/test_helpers.h"

int test_core(void)
{
    int failures = 0;

    printf("uint256 set/get hex roundtrip... ");
    {
        struct uint256 v;
        uint256_set_hex(&v, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
        char hex[65];
        uint256_get_hex(&v, hex);
        bool ok = (strcmp(hex, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", hex); failures++; }
    }

    printf("uint256 null check... ");
    {
        struct uint256 v;
        uint256_set_null(&v);
        bool ok = uint256_is_null(&v);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("uint256 comparison equal... ");
    {
        struct uint256 a, b;
        uint256_set_hex(&a, "1111111111111111111111111111111111111111111111111111111111111111");
        uint256_set_hex(&b, "1111111111111111111111111111111111111111111111111111111111111111");
        bool ok = (uint256_cmp(&a, &b) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("uint256 comparison less... ");
    {
        struct uint256 a, b;
        uint256_set_hex(&a, "0000000000000000000000000000000000000000000000000000000000000001");
        uint256_set_hex(&b, "0000000000000000000000000000000000000000000000000000000000000002");
        bool ok = (uint256_cmp(&a, &b) < 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("uint256 eq/neq... ");
    {
        struct uint256 a, b, c;
        memset(a.data, 0x42, 32);
        memset(b.data, 0x42, 32);
        memset(c.data, 0x43, 32);
        bool ok = uint256_eq(&a, &b) && !uint256_eq(&a, &c);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }



    printf("MoneyRange accepts valid... ");
    {
        bool ok = MoneyRange(0) && MoneyRange(100000000LL) && MoneyRange(MAX_MONEY);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("MoneyRange rejects invalid... ");
    {
        bool ok = !MoneyRange(-1) && !MoneyRange(MAX_MONEY + 1);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("byte_stream write/read roundtrip... ");
    {
        struct byte_stream s;
        stream_init(&s, 64);
        stream_write_u8(&s, 0x42);
        stream_write_u32_le(&s, 0xDEADBEEF);
        stream_write_u64_le(&s, 0x123456789ABCDEF0ULL);
        stream_write_compact_size(&s, 300);

        struct byte_stream rs;
        stream_init_from_data(&rs, s.data, s.size);
        uint8_t v8;
        stream_read(&rs, &v8, 1);
        uint32_t v32;
        stream_read_u32_le(&rs, &v32);
        uint64_t v64;
        stream_read_u64_le(&rs, &v64);
        uint64_t vcs;
        stream_read_compact_size(&rs, &vcs);

        bool ok = (v8 == 0x42) && (v32 == 0xDEADBEEF) &&
                  (v64 == 0x123456789ABCDEF0ULL) && (vcs == 300);
        stream_free(&s);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("compact_size encoding edge cases... ");
    {
        struct byte_stream s;
        stream_init(&s, 64);

        /* 0xFC = 252 (1 byte) */
        stream_write_compact_size(&s, 252);
        /* 0xFD = 253 (3 bytes) */
        stream_write_compact_size(&s, 253);
        /* 0x10000 = 65536 (5 bytes) */
        stream_write_compact_size(&s, 65536);

        struct byte_stream rs;
        stream_init_from_data(&rs, s.data, s.size);
        uint64_t a, b, c;
        stream_read_compact_size(&rs, &a);
        stream_read_compact_size(&rs, &b);
        stream_read_compact_size(&rs, &c);

        bool ok = (a == 252) && (b == 253) && (c == 65536);
        stream_free(&s);
        if (ok) printf("OK\n");
        else { printf("FAIL (a=%llu b=%llu c=%llu)\n",
            (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
            failures++; }
    }

    printf("GetRand produces different values... ");
    {
        uint64_t a = GetRand(UINT64_MAX);
        uint64_t b = GetRand(UINT64_MAX);
        uint64_t c = GetRand(UINT64_MAX);
        bool ok = (a != b) || (b != c); /* astronomically unlikely to collide */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("GetRandBytes fills buffer... ");
    {
        uint8_t buf[32] = {0};
        GetRandBytes(buf, 32);
        bool nonzero = false;
        for (int i = 0; i < 32; i++) if (buf[i]) nonzero = true;
        if (nonzero) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
