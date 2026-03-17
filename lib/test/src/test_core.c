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

    /* ================================================================
     * compact_size: canonical encoding roundtrip
     * ================================================================ */
    printf("compact_size: small value roundtrip... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_compact_size(&ws, 42);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint64_t val = 0;
        bool ok = stream_read_compact_size(&rs, &val) && val == 42;
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL (val=%llu)\n", (unsigned long long)val); failures++; }
    }

    printf("compact_size: 16-bit value roundtrip... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_compact_size(&ws, 1000);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint64_t val = 0;
        bool ok = stream_read_compact_size(&rs, &val) && val == 1000;
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL (val=%llu)\n", (unsigned long long)val); failures++; }
    }

    printf("compact_size: 32-bit value roundtrip... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_compact_size(&ws, 100000);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint64_t val = 0;
        bool ok = stream_read_compact_size(&rs, &val) && val == 100000;
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL (val=%llu)\n", (unsigned long long)val); failures++; }
    }

    printf("compact_size: accepts non-canonical encoding (wire compat)... ");
    {
        /* Non-canonical: value 50 encoded as 3-byte (marker=253).
         * Bitcoin wire protocol accepts these for backwards compatibility. */
        uint8_t nc[] = {0xfd, 50, 0};
        struct byte_stream rs;
        stream_init_from_data(&rs, nc, sizeof(nc));
        uint64_t val = 0;
        bool ok = stream_read_compact_size(&rs, &val) && val == 50;
        if (ok) printf("OK\n");
        else { printf("FAIL (val=%llu)\n", (unsigned long long)val); failures++; }
    }

    printf("compact_size: 64-bit value roundtrip... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_compact_size(&ws, 0x200000000ULL);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint64_t val = 0;
        bool ok = stream_read_compact_size(&rs, &val) && val == 0x200000000ULL;
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL (val=%llu)\n", (unsigned long long)val); failures++; }
    }

    printf("compact_size: boundary value 253 roundtrip... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_compact_size(&ws, 253);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint64_t val = 0;
        bool ok = stream_read_compact_size(&rs, &val) && val == 253;
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL (val=%llu)\n", (unsigned long long)val); failures++; }
    }

    /* ================================================================
     * stream_grow: overflow protection
     * ================================================================ */
    printf("stream_grow: handles large allocation gracefully... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 8);
        /* Write enough to trigger growth */
        uint8_t buf[256];
        memset(buf, 0xAA, sizeof(buf));
        bool ok = stream_write(&ws, buf, sizeof(buf));
        ok = ok && (ws.size == 256) && (ws.capacity >= 256);
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("stream_write: rejects write to read-only stream... ");
    {
        uint8_t data[] = {1, 2, 3};
        struct byte_stream rs;
        stream_init_from_data(&rs, data, sizeof(data));
        uint8_t extra[] = {4};
        bool ok = !stream_write(&rs, extra, 1); /* should fail - read-only */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * varint roundtrip
     * ================================================================ */
    printf("varint: roundtrip large value... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 64);
        stream_write_varint(&ws, 0xDEADBEEFULL);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint64_t val = 0;
        bool ok = stream_read_varint(&rs, &val) && val == 0xDEADBEEFULL;
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL (val=0x%llx)\n", (unsigned long long)val); failures++; }
    }

    return failures;
}
