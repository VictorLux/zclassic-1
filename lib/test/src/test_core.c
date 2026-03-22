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
        uint8_t v8 = 0;
        stream_read(&rs, &v8, 1);
        uint32_t v32 = 0;
        stream_read_u32_le(&rs, &v32);
        uint64_t v64 = 0;
        stream_read_u64_le(&rs, &v64);
        uint64_t vcs = 0;
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

    /* ================================================================
     * ZCLASSIC C++ COMPAT: CompactSize encoding
     * Must match C++ WriteCompactSize / ReadCompactSize exactly.
     * ================================================================ */
    printf("zclassic compat: compact_size encoding... ");
    {
        bool ok = true;
        struct {
            uint64_t value;
            size_t expected_len;
            uint8_t expected_bytes[9];
        } tests[] = {
            {0,          1, {0x00}},
            {1,          1, {0x01}},
            {252,        1, {0xfc}},
            {253,        3, {0xfd, 0xfd, 0x00}},
            {254,        3, {0xfd, 0xfe, 0x00}},
            {0xff,       3, {0xfd, 0xff, 0x00}},
            {0x100,      3, {0xfd, 0x00, 0x01}},
            {0xffff,     3, {0xfd, 0xff, 0xff}},
            {0x10000,    5, {0xfe, 0x00, 0x00, 0x01, 0x00}},
            {0xffffffff, 5, {0xfe, 0xff, 0xff, 0xff, 0xff}},
            {0x100000000ULL, 9, {0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}},
        };
        for (int i = 0; i < 11 && ok; i++) {
            struct byte_stream ws;
            stream_init(&ws, 16);
            stream_write_compact_size(&ws, tests[i].value);
            ok = (ws.size == tests[i].expected_len);
            ok = ok && (memcmp(ws.data, tests[i].expected_bytes, tests[i].expected_len) == 0);
            /* Roundtrip */
            struct byte_stream rs;
            stream_init_from_data(&rs, ws.data, ws.size);
            uint64_t decoded = 0;
            ok = ok && stream_read_compact_size(&rs, &decoded);
            ok = ok && (decoded == tests[i].value);
            stream_free(&ws);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPAT: integer serialization (little-endian)
     * Must match C++ ser_writedata32/ser_readdata32 (htole32/le32toh)
     * ================================================================ */
    printf("zclassic compat: u32_le serialization... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 16);
        stream_write_u32_le(&ws, 0xDEADBEEF);
        /* Little-endian: 0xEF 0xBE 0xAD 0xDE */
        bool ok = (ws.size == 4);
        ok = ok && (ws.data[0] == 0xEF);
        ok = ok && (ws.data[1] == 0xBE);
        ok = ok && (ws.data[2] == 0xAD);
        ok = ok && (ws.data[3] == 0xDE);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        uint32_t val = 0;
        ok = ok && stream_read_u32_le(&rs, &val);
        ok = ok && (val == 0xDEADBEEF);
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: i64_le serialization... ");
    {
        struct byte_stream ws;
        stream_init(&ws, 16);
        int64_t amount = -100000000LL; /* negative CAmount */
        stream_write_i64_le(&ws, amount);
        bool ok = (ws.size == 8);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        int64_t decoded = 0;
        ok = ok && stream_read_i64_le(&rs, &decoded);
        ok = ok && (decoded == amount);
        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPAT: Transaction serialization roundtrip
     * Build a v1 transaction and verify serialize→deserialize matches.
     * ================================================================ */
    printf("zclassic compat: tx v1 serialize/deserialize roundtrip... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        tx.version = 1;
        tx.lock_time = 500000;
        transaction_alloc(&tx, 1, 2);

        /* Input: prevout hash + index + scriptsig + sequence */
        memset(tx.vin[0].prevout.hash.data, 0xAA, 32);
        tx.vin[0].prevout.n = 0;
        uint8_t sig[] = {0x48, 0x30, 0x45};  /* fake sig prefix */
        script_set(&tx.vin[0].script_sig, sig, 3);
        tx.vin[0].sequence = 0xFFFFFFFE;

        /* Outputs */
        tx.vout[0].value = 100000000LL;
        uint8_t p2pkh[25] = {
            0x76, 0xa9, 0x14,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
            0x11, 0x12, 0x13, 0x14,
            0x88, 0xac
        };
        script_set(&tx.vout[0].script_pub_key, p2pkh, 25);
        tx.vout[1].value = 50000000LL;
        script_set(&tx.vout[1].script_pub_key, p2pkh, 25);

        /* Serialize */
        struct byte_stream ws;
        stream_init(&ws, 256);
        bool ok = transaction_serialize(&tx, &ws);

        /* Verify header bytes: version=1, no overwintered flag */
        ok = ok && (ws.data[0] == 0x01 && ws.data[1] == 0x00 &&
                    ws.data[2] == 0x00 && ws.data[3] == 0x00);

        /* Deserialize */
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct transaction tx2;
        ok = ok && transaction_deserialize(&tx2, &rs);
        ok = ok && (tx2.version == 1);
        ok = ok && !tx2.overwintered;
        ok = ok && (tx2.num_vin == 1);
        ok = ok && (tx2.num_vout == 2);
        ok = ok && (tx2.lock_time == 500000);
        ok = ok && (tx2.vout[0].value == 100000000LL);
        ok = ok && (tx2.vout[1].value == 50000000LL);
        ok = ok && (memcmp(tx2.vin[0].prevout.hash.data,
                           tx.vin[0].prevout.hash.data, 32) == 0);
        ok = ok && (tx2.vin[0].sequence == 0xFFFFFFFE);

        stream_free(&ws);
        transaction_free(&tx);
        transaction_free(&tx2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zclassic compat: tx sapling v4 header encoding... ");
    {
        /* Sapling v4: header = version | (1<<31) for overwintered */
        struct transaction tx;
        transaction_init(&tx);
        tx.overwintered = true;
        tx.version = 4;
        tx.version_group_id = 0x892F2085; /* SAPLING_VERSION_GROUP_ID */
        tx.lock_time = 0;
        tx.expiry_height = 500000;
        tx.value_balance = 0;
        transaction_alloc(&tx, 0, 0);

        struct byte_stream ws;
        stream_init(&ws, 128);
        bool ok = transaction_serialize(&tx, &ws);

        /* Header: 0x80000004 in little-endian = 04 00 00 80 */
        ok = ok && (ws.data[0] == 0x04);
        ok = ok && (ws.data[1] == 0x00);
        ok = ok && (ws.data[2] == 0x00);
        ok = ok && (ws.data[3] == 0x80);
        /* Version group ID: 0x892F2085 in LE */
        ok = ok && (ws.data[4] == 0x85);
        ok = ok && (ws.data[5] == 0x20);
        ok = ok && (ws.data[6] == 0x2F);
        ok = ok && (ws.data[7] == 0x89);

        /* Deserialize and verify */
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct transaction tx2;
        ok = ok && transaction_deserialize(&tx2, &rs);
        ok = ok && tx2.overwintered;
        ok = ok && (tx2.version == 4);
        ok = ok && (tx2.version_group_id == 0x892F2085);
        ok = ok && (tx2.expiry_height == 500000);

        stream_free(&ws);
        transaction_free(&tx);
        transaction_free(&tx2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================
     * ZCLASSIC C++ COMPAT: Block header field order
     * Must match: nVersion, hashPrevBlock, hashMerkleRoot,
     * hashFinalSaplingRoot, nTime, nBits, nNonce, nSolution
     * ================================================================ */
    printf("zclassic compat: block header field order... ");
    {
        struct block_header h;
        memset(&h, 0, sizeof(h));
        h.nVersion = 4;
        memset(h.hashPrevBlock.data, 0x11, 32);
        memset(h.hashMerkleRoot.data, 0x22, 32);
        memset(h.hashFinalSaplingRoot.data, 0x33, 32);
        h.nTime = 1478403829;
        h.nBits = 0x2007ffff;
        memset(h.nNonce.data, 0x44, 32);
        h.nSolutionSize = 0;

        struct byte_stream ws;
        stream_init(&ws, 256);
        bool ok = block_header_serialize(&h, &ws);

        /* Verify field order:
         * [0..3] nVersion LE (4,0,0,0)
         * [4..35] hashPrevBlock (32 bytes of 0x11)
         * [36..67] hashMerkleRoot (32 bytes of 0x22)
         * [68..99] hashFinalSaplingRoot (32 bytes of 0x33)
         * [100..103] nTime LE
         * [104..107] nBits LE
         * [108..139] nNonce (32 bytes of 0x44)
         * [140] compact_size(0) = 0x00
         */
        ok = ok && (ws.size == 141);
        ok = ok && (ws.data[0] == 4 && ws.data[1] == 0 && ws.data[2] == 0 && ws.data[3] == 0);
        ok = ok && (ws.data[4] == 0x11);   /* hashPrevBlock start */
        ok = ok && (ws.data[36] == 0x22);  /* hashMerkleRoot start */
        ok = ok && (ws.data[68] == 0x33);  /* hashFinalSaplingRoot start */
        /* nTime = 1478403829 = 0x581EA6F5 LE: F5 A6 1E 58 */
        ok = ok && (ws.data[100] == 0xF5);
        ok = ok && (ws.data[101] == 0xA6);
        ok = ok && (ws.data[102] == 0x1E);
        ok = ok && (ws.data[103] == 0x58);
        /* nBits = 0x2007ffff LE: FF FF 07 20 */
        ok = ok && (ws.data[104] == 0xFF);
        ok = ok && (ws.data[105] == 0xFF);
        ok = ok && (ws.data[106] == 0x07);
        ok = ok && (ws.data[107] == 0x20);
        ok = ok && (ws.data[108] == 0x44); /* nNonce start */
        ok = ok && (ws.data[140] == 0x00); /* empty solution */

        /* Deserialize roundtrip */
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct block_header h2;
        memset(&h2, 0, sizeof(h2));
        ok = ok && block_header_deserialize(&h2, &rs);
        ok = ok && (h2.nVersion == 4);
        ok = ok && (h2.nTime == 1478403829);
        ok = ok && (h2.nBits == 0x2007ffff);
        ok = ok && (memcmp(h2.hashPrevBlock.data, h.hashPrevBlock.data, 32) == 0);
        ok = ok && (memcmp(h2.hashMerkleRoot.data, h.hashMerkleRoot.data, 32) == 0);
        ok = ok && (memcmp(h2.hashFinalSaplingRoot.data, h.hashFinalSaplingRoot.data, 32) == 0);

        stream_free(&ws);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
