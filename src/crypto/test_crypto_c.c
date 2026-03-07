/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Standalone test for pure C crypto primitives. */

#include <stdio.h>
#include <string.h>
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "crypto/sha1.h"
#include "crypto/ripemd160.h"
#include "crypto/hmac_sha256.h"
#include "crypto/hmac_sha512.h"
#include "crypto/sha3.h"
#include "crypto/blake2b.h"
#include "uint256.h"
#include "hash.h"
#include "base58.h"
#include "bech32.h"
#include "arith_uint256.h"
#include "random.h"
#include "utiltime.h"
#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "clientversion.h"
#include "chainparamsbase.h"
#include "util.h"
#include "ui_interface.h"
#include "noui.h"
#include "deprecation.h"
#include "timedata.h"
#include "netaddr.h"
#include "protocol_c.h"
#include "pow_c.h"
#include "checkpoints_c.h"
#include "pubkey_c.h"
#include "key_c.h"
#include "script/script_c.h"
#include "compressor_c.h"
#include "script/standard_c.h"

static int check_hex(const unsigned char *data, size_t len, const char *expected)
{
    char buf[256];
    for (size_t i = 0; i < len; i++)
        snprintf(buf + i * 2, 3, "%02x", data[i]);
    if (strcmp(buf, expected) != 0) {
        printf("FAIL\n  got:      %s\n  expected: %s\n", buf, expected);
        return 1;
    }
    printf("OK\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    unsigned char hash[64];

    printf("SHA-256(\"\")... ");
    struct sha256_ctx sha256;
    sha256_init(&sha256);
    sha256_finalize(&sha256, hash);
    failures += check_hex(hash, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    printf("SHA-256(\"abc\")... ");
    sha256_init(&sha256);
    sha256_write(&sha256, (const unsigned char *)"abc", 3);
    sha256_finalize(&sha256, hash);
    failures += check_hex(hash, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    printf("SHA-512(\"\")... ");
    struct sha512_ctx sha512;
    sha512_init(&sha512);
    sha512_finalize(&sha512, hash);
    failures += check_hex(hash, 64, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

    printf("SHA-512(\"abc\")... ");
    sha512_init(&sha512);
    sha512_write(&sha512, (const unsigned char *)"abc", 3);
    sha512_finalize(&sha512, hash);
    failures += check_hex(hash, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

    printf("SHA-1(\"abc\")... ");
    struct sha1_ctx sha1;
    sha1_init(&sha1);
    sha1_write(&sha1, (const unsigned char *)"abc", 3);
    sha1_finalize(&sha1, hash);
    failures += check_hex(hash, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");

    printf("RIPEMD-160(\"abc\")... ");
    struct ripemd160_ctx rmd;
    ripemd160_init(&rmd);
    ripemd160_write(&rmd, (const unsigned char *)"abc", 3);
    ripemd160_finalize(&rmd, hash);
    failures += check_hex(hash, 20, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");

    printf("HMAC-SHA256(\"\",\"\")... ");
    struct hmac_sha256_ctx hmac256;
    hmac_sha256_init(&hmac256, (const unsigned char *)"", 0);
    hmac_sha256_finalize(&hmac256, hash);
    failures += check_hex(hash, 32, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");

    printf("BLAKE2b-256(\"\")... ");
    blake2b(hash, 32, NULL, 0, NULL, 0);
    failures += check_hex(hash, 32, "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");

    printf("BLAKE2b-256(\"abc\")... ");
    blake2b(hash, 32, "abc", 3, NULL, 0);
    failures += check_hex(hash, 32, "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");

    printf("Hash256(\"\")... ");
    hash256(NULL, 0, hash);
    failures += check_hex(hash, 32, "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");

    printf("Hash160(\"\")... ");
    hash160(NULL, 0, hash);
    failures += check_hex(hash, 20, "b472a266d0bd89c13706a4132ccfb16f7c3b9fcb");

    printf("uint256 hex... ");
    struct uint256 u;
    uint256_set_hex(&u, "00000000000000000000000000000000000000000000000000000000deadbeef");
    char hexbuf[65];
    uint256_get_hex(&u, hexbuf);
    if (strcmp(hexbuf, "00000000000000000000000000000000000000000000000000000000deadbeef") == 0) {
        printf("OK\n");
    } else {
        printf("FAIL: %s\n", hexbuf);
        failures++;
    }

    printf("base58 encode... ");
    {
        const unsigned char data[] = { 0x00, 0x01, 0x02, 0x03 };
        char b58[64];
        size_t b58_len;
        base58_encode(data, 4, b58, sizeof(b58), &b58_len);
        if (strcmp(b58, "1Ldp") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", b58);
            failures++;
        }
    }

    printf("base58 decode... ");
    {
        unsigned char out[64];
        size_t out_len;
        if (base58_decode("1Ldp", out, sizeof(out), &out_len) &&
            out_len == 4 && out[0] == 0x00 && out[1] == 0x01 && out[2] == 0x02 && out[3] == 0x03)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("base58check roundtrip... ");
    {
        const unsigned char payload[] = { 0x00, 0x14, 0x01, 0x02, 0x03 };
        char encoded[128];
        size_t enc_len;
        base58check_encode(payload, 5, encoded, sizeof(encoded), &enc_len);
        unsigned char decoded[128];
        size_t dec_len;
        if (base58check_decode(encoded, decoded, sizeof(decoded), &dec_len) &&
            dec_len == 5 && memcmp(decoded, payload, 5) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("bech32 encode... ");
    {
        uint8_t values[] = { 0, 14, 20, 15, 7, 13, 26, 0, 25, 18, 6, 11, 13, 8, 21, 4, 20, 3, 17, 2, 29, 3, 12, 29, 3, 4, 15, 24, 20, 6, 14, 30, 22 };
        char out[128];
        if (bech32_encode(out, sizeof(out), "bc", values, 33) && strlen(out) > 0)
            printf("OK (%s)\n", out);
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("bech32 decode... ");
    {
        char hrp[16];
        uint8_t data[128];
        size_t data_len;
        if (bech32_decode(hrp, sizeof(hrp), data, sizeof(data), &data_len, "a12uel5l") &&
            strcmp(hrp, "a") == 0 && data_len == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("bech32 roundtrip... ");
    {
        uint8_t values[] = { 1, 2, 3, 4, 5 };
        char encoded[128];
        bech32_encode(encoded, sizeof(encoded), "test", values, 5);
        char hrp[16];
        uint8_t decoded[128];
        size_t dec_len;
        if (bech32_decode(hrp, sizeof(hrp), decoded, sizeof(decoded), &dec_len, encoded) &&
            strcmp(hrp, "test") == 0 && dec_len == 5 &&
            memcmp(decoded, values, 5) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("arith_uint256 compact roundtrip... ");
    {
        struct arith_uint256 target;
        bool neg, ovf;
        arith_uint256_set_compact(&target, 0x1d00ffff, &neg, &ovf);
        uint32_t compact = arith_uint256_get_compact(&target, false);
        if (compact == 0x1d00ffff && !neg && !ovf)
            printf("OK\n");
        else {
            printf("FAIL: compact=0x%08x neg=%d ovf=%d\n", compact, neg, ovf);
            failures++;
        }
    }

    printf("arith_uint256 arithmetic... ");
    {
        struct arith_uint256 a, b, r;
        arith_uint256_set_u64(&a, 0xFFFFFFFF);
        arith_uint256_set_u64(&b, 2);
        arith_uint256_mul_u32(&r, &a, 2);
        if (arith_uint256_get_low64(&r) == 0x1FFFFFFFE)
            printf("OK\n");
        else {
            printf("FAIL: got 0x%llx\n", (unsigned long long)arith_uint256_get_low64(&r));
            failures++;
        }
    }

    printf("arith_uint256 shift... ");
    {
        struct arith_uint256 a, r;
        arith_uint256_set_u64(&a, 1);
        arith_uint256_shl(&r, &a, 64);
        if (r.pn[2] == 1 && r.pn[0] == 0 && r.pn[1] == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("arith_uint256 division... ");
    {
        struct arith_uint256 a, b, r;
        arith_uint256_set_u64(&a, 100);
        arith_uint256_set_u64(&b, 7);
        arith_uint256_div(&r, &a, &b);
        if (arith_uint256_get_low64(&r) == 14)
            printf("OK\n");
        else {
            printf("FAIL: got %llu\n", (unsigned long long)arith_uint256_get_low64(&r));
            failures++;
        }
    }

    printf("arith_uint256 <-> uint256 conversion... ");
    {
        struct uint256 u;
        uint256_set_hex(&u, "00000000000000000000000000000000000000000000000000000000deadbeef");
        struct arith_uint256 a;
        uint256_to_arith(&a, &u);
        struct uint256 u2;
        arith_to_uint256(&u2, &a);
        char hex[65];
        uint256_get_hex(&u2, hex);
        if (strcmp(hex, "00000000000000000000000000000000000000000000000000000000deadbeef") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", hex);
            failures++;
        }
    }

    printf("random bytes... ");
    {
        unsigned char buf[32];
        memset(buf, 0, 32);
        GetRandBytes(buf, 32);
        int nonzero = 0;
        for (int i = 0; i < 32; i++)
            if (buf[i] != 0) nonzero++;
        if (nonzero > 0)
            printf("OK (%d non-zero bytes)\n", nonzero);
        else {
            printf("FAIL: all zeros\n");
            failures++;
        }
    }

    printf("GetRand... ");
    {
        uint64_t r = GetRand(100);
        if (r < 100)
            printf("OK (%llu)\n", (unsigned long long)r);
        else {
            printf("FAIL: %llu >= 100\n", (unsigned long long)r);
            failures++;
        }
    }

    printf("GetTime... ");
    {
        int64_t t = GetTime();
        if (t > 1700000000)
            printf("OK (%lld)\n", (long long)t);
        else {
            printf("FAIL: %lld\n", (long long)t);
            failures++;
        }
    }

    printf("DateTimeStrFormat... ");
    {
        char buf[64];
        DateTimeStrFormat(buf, sizeof(buf), "%Y-%m-%d", 0);
        if (strcmp(buf, "1970-01-01") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", buf);
            failures++;
        }
    }

    printf("consensus upgrade state... ");
    {
        struct consensus_params params;
        memset(&params, 0, sizeof(params));
        params.vUpgrades[BASE_SPROUT].nActivationHeight = NETWORK_UPGRADE_ALWAYS_ACTIVE;
        params.vUpgrades[UPGRADE_OVERWINTER].nActivationHeight = 100;
        params.vUpgrades[UPGRADE_SAPLING].nActivationHeight = 200;
        params.vUpgrades[UPGRADE_TESTDUMMY].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
        params.vUpgrades[UPGRADE_BUBBLES].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
        params.vUpgrades[UPGRADE_DIFFADJ].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;
        params.vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight = NETWORK_UPGRADE_NO_ACTIVATION;

        if (consensus_upgrade_state(50, &params, UPGRADE_OVERWINTER) == UPGRADE_PENDING &&
            consensus_upgrade_state(100, &params, UPGRADE_OVERWINTER) == UPGRADE_ACTIVE &&
            consensus_upgrade_state(50, &params, UPGRADE_TESTDUMMY) == UPGRADE_DISABLED &&
            consensus_current_epoch(150, &params) == UPGRADE_OVERWINTER &&
            consensus_current_epoch(250, &params) == UPGRADE_SAPLING)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("FormatMoney... ");
    {
        char buf[64];
        FormatMoney(100000000, buf, sizeof(buf));
        if (strcmp(buf, "1.0") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", buf);
            failures++;
        }
    }

    printf("ParseMoney... ");
    {
        CAmount val = 0;
        if (ParseMoney("1.5", &val) && val == 150000000)
            printf("OK\n");
        else {
            printf("FAIL: %lld\n", (long long)val);
            failures++;
        }
    }

    printf("IsHex... ");
    {
        if (IsHex("deadbeef") && !IsHex("deadbee") && !IsHex("xyz"))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("ParseHex... ");
    {
        unsigned char out[32];
        size_t n = ParseHex("deadbeef", out, sizeof(out));
        if (n == 4 && out[0] == 0xde && out[1] == 0xad && out[2] == 0xbe && out[3] == 0xef)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("HexStr... ");
    {
        unsigned char data[] = { 0xde, 0xad, 0xbe, 0xef };
        char hexout[64];
        HexStr(data, 4, false, hexout, sizeof(hexout));
        if (strcmp(hexout, "deadbeef") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", hexout);
            failures++;
        }
    }

    printf("EncodeBase64... ");
    {
        char b64[64];
        EncodeBase64((const unsigned char *)"Hello", 5, b64, sizeof(b64));
        if (strcmp(b64, "SGVsbG8=") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", b64);
            failures++;
        }
    }

    printf("DecodeBase64... ");
    {
        unsigned char out[64];
        bool invalid = false;
        size_t n = DecodeBase64("SGVsbG8=", out, sizeof(out), &invalid);
        if (!invalid && n == 5 && memcmp(out, "Hello", 5) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("EncodeBase32... ");
    {
        char b32[64];
        EncodeBase32((const unsigned char *)"Hello", 5, b32, sizeof(b32));
        if (strcmp(b32, "jbswy3dp") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", b32);
            failures++;
        }
    }

    printf("ParseInt32... ");
    {
        int32_t val = 0;
        if (ParseInt32("12345", &val) && val == 12345 &&
            !ParseInt32("", &val) && !ParseInt32(" 1", &val))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("ParseFixedPoint... ");
    {
        int64_t amount = 0;
        if (ParseFixedPoint("1.5", 8, &amount) && amount == 150000000LL &&
            ParseFixedPoint("-0.5", 8, &amount) && amount == -50000000LL)
            printf("OK\n");
        else {
            printf("FAIL: %lld\n", (long long)amount);
            failures++;
        }
    }

    printf("SanitizeString... ");
    {
        char out[64];
        SanitizeString("hello<world>&test", SAFE_CHARS_DEFAULT, out, sizeof(out));
        if (strcmp(out, "helloworldtest") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", out);
            failures++;
        }
    }

    printf("FormatVersion... ");
    {
        char ver[64];
        FormatVersion(CLIENT_VERSION, ver, sizeof(ver));
        if (strstr(ver, "2.1.1") != NULL)
            printf("OK (%s)\n", ver);
        else {
            printf("FAIL: %s\n", ver);
            failures++;
        }
    }

    printf("CLIENT_NAME... ");
    {
        if (strcmp(CLIENT_NAME, "MagicBean") == 0)
            printf("OK\n");
        else {
            printf("FAIL: %s\n", CLIENT_NAME);
            failures++;
        }
    }

    printf("ParseParameters... ");
    {
        const char *argv[] = { "test", "-foo=bar", "-debug", "-baz=42" };
        ParseParameters(4, argv);
        if (strcmp(GetArg("-foo", ""), "bar") == 0 &&
            GetBoolArg("-debug", false) == true &&
            GetArgInt("-baz", 0) == 42 &&
            strcmp(GetArg("-noexist", "default"), "default") == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("GetNumCores... ");
    {
        int n = GetNumCores();
        if (n >= 1)
            printf("OK (%d)\n", n);
        else {
            printf("FAIL: %d\n", n);
            failures++;
        }
    }

    printf("chainparamsbase... ");
    {
        SelectBaseParams(CHAIN_MAIN);
        const struct base_chain_params *p = BaseParams();
        if (p->nRPCPort == 8023 && AreBaseParamsConfigured()) {
            SelectBaseParams(CHAIN_TESTNET);
            p = BaseParams();
            if (p->nRPCPort == 18023 && strcmp(p->strDataDir, "testnet3") == 0)
                printf("OK\n");
            else {
                printf("FAIL: testnet\n");
                failures++;
            }
        } else {
            printf("FAIL: main\n");
            failures++;
        }
    }

    printf("noui_connect... ");
    {
        noui_connect();
        if (uiInterface.ThreadSafeMessageBox != NULL &&
            uiInterface.InitMessage != NULL)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("deprecation... ");
    {
        /* Should not crash with very high height */
        EnforceNodeDeprecation(1, false, false);
        printf("OK\n");
    }

    printf("GetAdjustedTime... ");
    {
        int64_t t = GetAdjustedTime();
        if (t > 1700000000)
            printf("OK (%lld)\n", (long long)t);
        else {
            printf("FAIL: %lld\n", (long long)t);
            failures++;
        }
    }

    printf("ConvertBits 8->5... ");
    {
        unsigned char in[] = { 0xff, 0x00 };
        unsigned char out[8];
        size_t out_len = 0;
        if (ConvertBits(8, 5, true, in, 2, out, sizeof(out), &out_len) &&
            out_len == 4 && out[0] == 0x1f && out[1] == 0x1c && out[2] == 0x00 && out[3] == 0x00)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("net_addr IPv4... ");
    {
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip4[] = {192, 168, 1, 1};
        net_addr_set_ipv4(&a, ip4);
        char str[64];
        net_addr_to_string(&a, str, sizeof(str));
        if (net_addr_is_ipv4(&a) && strcmp(str, "192.168.1.1") == 0)
            printf("OK (%s)\n", str);
        else {
            printf("FAIL: %s\n", str);
            failures++;
        }
    }

    printf("net_service to_string... ");
    {
        struct net_service s;
        net_service_init(&s);
        unsigned char ip4[] = {10, 0, 0, 1};
        net_addr_set_ipv4(&s.addr, ip4);
        s.port = 8233;
        char str[64];
        net_service_to_string(&s, str, sizeof(str));
        if (strcmp(str, "10.0.0.1:8233") == 0)
            printf("OK (%s)\n", str);
        else {
            printf("FAIL: %s\n", str);
            failures++;
        }
    }

    printf("msg_header... ");
    {
        unsigned char start[4] = {0x24, 0xe9, 0x27, 0x64};
        struct msg_header h;
        msg_header_init_full(&h, start, "version", 100);
        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(&h, cmd, sizeof(cmd));
        if (strcmp(cmd, "version") == 0 && h.nMessageSize == 100 &&
            msg_header_is_valid(&h, start))
            printf("OK (%s)\n", cmd);
        else {
            printf("FAIL: %s\n", cmd);
            failures++;
        }
    }

    printf("inv_item... ");
    {
        struct uint256 hash;
        memset(hash.data, 0xab, 32);
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hash);
        char str[128];
        inv_item_to_string(&inv, str, sizeof(str));
        if (inv_item_is_known_type(&inv) &&
            strcmp(inv_item_get_command(&inv), "tx") == 0)
            printf("OK (%s)\n", inv_item_get_command(&inv));
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("CheckProofOfWork... ");
    {
        struct consensus_params cp;
        memset(&cp, 0, sizeof(cp));
        /* Set powLimit to all 0xff (easiest possible) */
        memset(cp.powLimit.data, 0xff, 32);

        /* A hash of all zeros should always pass the easiest target */
        struct uint256 hash;
        uint256_set_null(&hash);
        uint32_t nBits = 0x2100ffff; /* very high target */
        struct arith_uint256 target;
        arith_uint256_set_compact(&target, nBits, NULL, NULL);
        uint32_t easy_bits = arith_uint256_get_compact(&target, false);

        if (CheckProofOfWork(hash, easy_bits, &cp))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("GetBlockProof... ");
    {
        struct block_index bi;
        block_index_init(&bi);
        bi.nBits = 0x1d00ffff; /* standard Bitcoin difficulty 1 */
        struct arith_uint256 proof = GetBlockProof(&bi);
        if (!arith_uint256_is_zero(&proof))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("checkpoints... ");
    {
        struct checkpoint_entry entries[] = {
            {0, {{0}}},
            {100000, {{0}}},
        };
        struct checkpoint_data cd = {
            .entries = entries,
            .nEntries = 2,
            .nTimeLastCheckpoint = 1500000000,
            .nTransactionsLastCheckpoint = 200000,
            .fTransactionsPerDay = 1000.0,
        };
        int est = checkpoints_get_total_blocks_estimate(&cd);
        struct block_index bi;
        block_index_init(&bi);
        bi.nChainTx = 100000;
        bi.nTime = 1500000000;
        double prog = checkpoints_guess_verification_progress(&cd, &bi, true);
        if (est == 100000 && prog > 0.0 && prog < 1.0)
            printf("OK (blocks=%d, progress=%.2f)\n", est, prog);
        else {
            printf("FAIL (blocks=%d, progress=%.2f)\n", est, prog);
            failures++;
        }
    }

    ecc_start();
    printf("pubkey init/validate... ");
    {
        ecc_verify_init();
        struct pubkey pk;
        pubkey_init(&pk);
        if (!pubkey_is_valid(&pk))
            printf("OK (empty key is invalid)\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("pubkey_get_id... ");
    {
        /* Compressed pubkey: 02 + 32 bytes */
        unsigned char data[33];
        memset(data, 0, 33);
        data[0] = 0x02;
        data[1] = 0x79; data[2] = 0xBE; data[3] = 0x66; data[4] = 0x7E;
        struct pubkey pk;
        pubkey_set(&pk, data, 33);
        struct key_id kid = pubkey_get_id(&pk);
        if (!uint160_is_null(&kid.id))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("ext_pubkey encode/decode... ");
    {
        struct ext_pubkey epk;
        memset(&epk, 0, sizeof(epk));
        epk.nDepth = 3;
        epk.nChild = 42;
        unsigned char data[33];
        memset(data, 0, 33);
        data[0] = 0x02;
        data[1] = 0x01;
        pubkey_set(&epk.pubkey, data, 33);

        unsigned char code[BIP32_EXTKEY_SIZE];
        ext_pubkey_encode(&epk, code);

        struct ext_pubkey decoded;
        ext_pubkey_decode(&decoded, code);

        if (decoded.nDepth == 3 && decoded.nChild == 42 &&
            decoded.pubkey.size == 33)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        ecc_verify_destroy();
    }

    printf("key generate + sign + verify... ");
    {
        ecc_verify_init();
        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);

        struct uint256 hash;
        memset(hash.data, 0x42, 32);

        unsigned char sig[SIGNATURE_SIZE];
        size_t siglen = SIGNATURE_SIZE;
        bool signed_ok = privkey_sign(&k, &hash, sig, &siglen);
        bool verified = pubkey_verify(&pk, &hash, sig, siglen);

        if (signed_ok && verified)
            printf("OK\n");
        else {
            printf("FAIL (signed=%d, verified=%d)\n", signed_ok, verified);
            failures++;
        }
    }

    printf("key sign_compact + recover... ");
    {
        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);

        struct uint256 hash;
        memset(hash.data, 0xAB, 32);

        unsigned char csig[COMPACT_SIGNATURE_SIZE];
        bool signed_ok = privkey_sign_compact(&k, &hash, csig);

        struct pubkey recovered;
        bool recovered_ok = pubkey_recover_compact(&recovered, &hash, csig);

        if (signed_ok && recovered_ok &&
            recovered.size == pk.size &&
            memcmp(recovered.vch, pk.vch, pk.size) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("script opcodes... ");
    {
        if (strcmp(script_get_op_name(OP_DUP), "OP_DUP") == 0 &&
            strcmp(script_get_op_name(OP_CHECKSIG), "OP_CHECKSIG") == 0 &&
            strcmp(script_get_op_name(OP_HASH160), "OP_HASH160") == 0) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("script P2PKH... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        unsigned char hash[20];
        memset(hash, 0xab, 20);
        script_push_data(&s, hash, 20);
        script_push_op(&s, OP_EQUALVERIFY);
        script_push_op(&s, OP_CHECKSIG);
        if (script_is_p2pkh(&s) && s.size == 25)
            printf("OK (size=%zu)\n", s.size);
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("compress_amount roundtrip... ");
    {
        uint64_t values[] = {0, 1, 100000000, 50000000, 2100000000000000ULL};
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            uint64_t c = compress_amount(values[i]);
            uint64_t d = decompress_amount(c);
            if (d != values[i]) { ok = false; break; }
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_compress P2PKH... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        unsigned char hash20[20];
        memset(hash20, 0xAB, 20);
        script_push_data(&s, hash20, 20);
        script_push_op(&s, OP_EQUALVERIFY);
        script_push_op(&s, OP_CHECKSIG);

        unsigned char out[33];
        size_t out_len = 0;
        if (script_compress(&s, out, &out_len) && out_len == 21 &&
            out[0] == 0x00 && memcmp(out + 1, hash20, 20) == 0) {
            struct script decoded;
            script_decompress(&decoded, 0x00, out + 1, 20);
            if (decoded.size == 25 && script_is_p2pkh(&decoded))
                printf("OK\n");
            else { printf("FAIL (decompress)\n"); failures++; }
        } else { printf("FAIL (compress)\n"); failures++; }
    }

    printf("block_index_get_ancestor... ");
    {
        struct block_index blocks[5];
        for (int i = 0; i < 5; i++) {
            block_index_init(&blocks[i]);
            blocks[i].nHeight = i;
            blocks[i].pprev = i > 0 ? &blocks[i - 1] : NULL;
        }
        for (int i = 0; i < 5; i++)
            block_index_build_skip(&blocks[i]);

        struct block_index *anc = block_index_get_ancestor(&blocks[4], 1);
        if (anc == &blocks[1])
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("script_solver P2PKH... ");
    {
        struct key_id kid;
        uint160_set_null(&kid.id);
        unsigned char kbytes[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
        memcpy(kid.id.data, kbytes, 20);
        struct script s;
        script_for_p2pkh(&s, &kid);
        enum txnouttype type;
        unsigned char solutions[20][65];
        size_t solution_sizes[20];
        size_t num_solutions;
        if (script_solver(&s, &type, solutions, solution_sizes, &num_solutions) &&
            type == TX_PUBKEYHASH && num_solutions == 1 && solution_sizes[0] == 20 &&
            solutions[0][0] == 1 && solutions[0][19] == 20)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_solver P2SH... ");
    {
        struct script_id sid;
        unsigned char sbytes[20] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0x11};
        memcpy(sid.hash.data, sbytes, 20);
        struct script s;
        script_for_p2sh(&s, &sid);
        enum txnouttype type;
        unsigned char solutions[20][65];
        size_t solution_sizes[20];
        size_t num_solutions;
        if (script_solver(&s, &type, solutions, solution_sizes, &num_solutions) &&
            type == TX_SCRIPTHASH && num_solutions == 1 && solution_sizes[0] == 20 &&
            solutions[0][0] == 0xaa && solutions[0][19] == 0x11)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_extract_destination P2PKH... ");
    {
        struct key_id kid;
        unsigned char kbytes[20] = {10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160,170,180,190,200};
        memcpy(kid.id.data, kbytes, 20);
        struct script s;
        script_for_p2pkh(&s, &kid);
        struct tx_destination dest;
        if (script_extract_destination(&s, &dest) && dest.type == DEST_KEY_ID &&
            memcmp(dest.id.key.id.data, kid.id.data, 20) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_for_destination roundtrip... ");
    {
        struct key_id kid;
        unsigned char kbytes[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
        memcpy(kid.id.data, kbytes, 20);
        struct tx_destination dest = { .type = DEST_KEY_ID };
        memcpy(dest.id.key.id.data, kid.id.data, 20);
        struct script s;
        script_for_destination(&s, &dest);
        struct tx_destination dest2;
        if (script_extract_destination(&s, &dest2) && dest2.type == DEST_KEY_ID &&
            memcmp(dest2.id.key.id.data, kid.id.data, 20) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("get_txn_output_type... ");
    {
        if (strcmp(get_txn_output_type(TX_PUBKEYHASH), "pubkeyhash") == 0 &&
            strcmp(get_txn_output_type(TX_SCRIPTHASH), "scripthash") == 0 &&
            strcmp(get_txn_output_type(TX_NULL_DATA), "nulldata") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_id_from_script... ");
    {
        struct script s;
        struct key_id kid;
        memset(&kid, 0, sizeof(kid));
        script_for_p2pkh(&s, &kid);
        struct script_id sid;
        script_id_from_script(&sid, &s);
        bool non_zero = false;
        for (int i = 0; i < 20; i++) {
            if (sid.hash.data[i] != 0) { non_zero = true; break; }
        }
        if (non_zero)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ecc_init_sanity_check... ");
    {
        if (ecc_init_sanity_check())
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        ecc_verify_destroy();
        ecc_stop();
    }

    printf("\n%s (%d failures)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
