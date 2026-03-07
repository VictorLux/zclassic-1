/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Standalone test for pure C crypto primitives. */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/time.h>
#include "crypto/sha256.h"
#include "crypto/sha512.h"
#include "crypto/sha1.h"
#include "crypto/ripemd160.h"
#include "crypto/hmac_sha256.h"
#include "crypto/hmac_sha512.h"
#include "crypto/sha3.h"
#include "crypto/blake2b.h"
#include "core/uint256.h"
#include "core/hash.h"
#include "encoding/base58.h"
#include "encoding/bech32.h"
#include "core/arith_uint256.h"
#include "core/random.h"
#include "core/utiltime.h"
#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "encoding/utilmoneystr.h"
#include "encoding/utilstrencodings.h"
#include "util/clientversion.h"
#include "chain/chainparamsbase.h"
#include "util/util.h"
#include "util/ui_interface.h"
#include "util/noui.h"
#include "util/deprecation.h"
#include "util/timedata.h"
#include "net/netaddr.h"
#include "net/protocol.h"
#include "chain/pow.h"
#include "chain/checkpoints.h"
#include "keys/pubkey.h"
#include "keys/key.h"
#include "script/script.h"
#include "coins/compressor.h"
#include "script/standard.h"
#include "primitives/transaction.h"
#include "bloom/bloom.h"
#include "bloom/merkle.h"
#include "script/sighashtype.h"
#include "coins/coins.h"
#include "core/serialize.h"
#include "primitives/block.h"
#include "script/sigencoding.h"
#include "support/pagelocker.h"
#include "script/interpreter.h"
#include "script/sigcache.h"
#include "consensus/validation.h"
#include "keys/key_io.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "validation/sighash.h"
#include "validation/check_transaction.h"
#include "validation/tx_verifier.h"
#include "validation/sigops.h"
#include "validation/contextual_check_tx.h"
#include "coins/undo.h"
#include "net/p2p_message.h"
#include "net/netbase.h"
#include "bloom/merkleblock.h"
#include "script/zcashconsensus.h"
#include "validation/validationinterface.h"
#include "net/addrman.h"
#include "net/net.h"
#include "validation/txmempool.h"
#include "policy/fees.h"
#include "json/json.h"

static int test_tip_count = 0;
static int test_tip_height = 0;

static void test_updated_block_tip(void *ctx, int height)
{
    (void)ctx;
    test_tip_count++;
    test_tip_height = height;
}

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

    printf("outpoint init/null... ");
    {
        struct outpoint op;
        outpoint_set_null(&op);
        if (outpoint_is_null(&op))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("tx_out init/null... ");
    {
        struct tx_out out;
        tx_out_set_null(&out);
        if (tx_out_is_null(&out) && out.value == -1)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("transaction alloc/free... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        if (transaction_alloc(&tx, 2, 3) && tx.num_vin == 2 && tx.num_vout == 3 &&
            outpoint_is_null(&tx.vin[0].prevout) && tx_out_is_null(&tx.vout[0])) {
            transaction_free(&tx);
            if (tx.vin == NULL && tx.vout == NULL)
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        } else { printf("FAIL\n"); failures++; }
    }

    printf("transaction_get_value_out... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 0, 2);
        tx.vout[0].value = 50 * COIN;
        tx.vout[1].value = 25 * COIN;
        int64_t total = transaction_get_value_out(&tx);
        if (total == 75 * COIN)
            printf("OK (%" PRId64 ")\n", total);
        else { printf("FAIL (%" PRId64 ")\n", total); failures++; }
        transaction_free(&tx);
    }

    printf("transaction_is_coinbase... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        /* vin[0] prevout is null by default → coinbase */
        tx.vout[0].value = 10 * COIN;
        if (transaction_is_coinbase(&tx))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("bloom_filter insert/contains... ");
    {
        struct bloom_filter bf;
        bloom_filter_init(&bf, 10, 0.000001, 2147483649U, BLOOM_UPDATE_ALL);
        unsigned char data1[] = {0x99, 0x10, 0x8a, 0xd8};
        unsigned char data2[] = {0x19, 0x10, 0x8a, 0xd8};
        unsigned char data3[] = {0xab, 0xcd, 0xef, 0x01};
        bloom_filter_insert(&bf, data1, 4);
        bloom_filter_insert(&bf, data2, 4);
        if (bloom_filter_contains(&bf, data1, 4) &&
            bloom_filter_contains(&bf, data2, 4) &&
            !bloom_filter_contains(&bf, data3, 4))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        bloom_filter_free(&bf);
    }

    printf("bloom_filter uint256... ");
    {
        struct bloom_filter bf;
        bloom_filter_init(&bf, 10, 0.000001, 0, BLOOM_UPDATE_NONE);
        struct uint256 h;
        uint256_set_null(&h);
        h.data[0] = 0xDE;
        h.data[31] = 0xAD;
        bloom_filter_insert_uint256(&bf, &h);
        if (bloom_filter_contains_uint256(&bf, &h)) {
            struct uint256 h2;
            uint256_set_null(&h2);
            h2.data[0] = 0xFF;
            if (!bloom_filter_contains_uint256(&bf, &h2))
                printf("OK\n");
            else { printf("FAIL (false positive)\n"); failures++; }
        } else { printf("FAIL\n"); failures++; }
        bloom_filter_free(&bf);
    }

    printf("rolling_bloom insert/contains... ");
    {
        struct rolling_bloom_filter rbf;
        rolling_bloom_init(&rbf, 10, 0.000001);
        unsigned char data1[] = {1, 2, 3, 4};
        unsigned char data2[] = {5, 6, 7, 8};
        rolling_bloom_insert(&rbf, data1, 4);
        if (rolling_bloom_contains(&rbf, data1, 4) &&
            !rolling_bloom_contains(&rbf, data2, 4))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        rolling_bloom_free(&rbf);
    }

    printf("compute_merkle_root 1 tx... ");
    {
        struct uint256 tx;
        memset(tx.data, 0xAA, 32);
        struct uint256 root = compute_merkle_root(&tx, 1);
        if (uint256_cmp(&root, &tx) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("compute_merkle_root 2 txs... ");
    {
        struct uint256 txids[2];
        memset(txids[0].data, 0x11, 32);
        memset(txids[1].data, 0x22, 32);
        struct uint256 root = compute_merkle_root(txids, 2);
        struct uint256 expected;
        merkle_hash_pair(&txids[0], &txids[1], &expected);
        if (uint256_cmp(&root, &expected) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("partial_merkle_tree build/extract... ");
    {
        struct uint256 txids[4];
        for (int i = 0; i < 4; i++)
            memset(txids[i].data, (unsigned char)(i + 1), 32);
        bool match[4] = {false, true, false, false};

        struct partial_merkle_tree t;
        merkle_tree_init(&t);
        if (merkle_tree_build(&t, txids, 4, match, 4)) {
            struct uint256 matched[4];
            size_t num_matched;
            struct uint256 root;
            if (merkle_tree_extract(&t, matched, &num_matched, &root) &&
                num_matched == 1 &&
                uint256_cmp(&matched[0], &txids[1]) == 0) {
                struct uint256 full_root = compute_merkle_root(txids, 4);
                if (uint256_cmp(&root, &full_root) == 0)
                    printf("OK\n");
                else { printf("FAIL (root mismatch)\n"); failures++; }
            } else { printf("FAIL (extract)\n"); failures++; }
        } else { printf("FAIL (build)\n"); failures++; }
        merkle_tree_free(&t);
    }

    printf("sighash_type... ");
    {
        struct sighash_type s = sighash_type_default();
        if (s.raw == SIGHASH_ALL &&
            sighash_get_base_type(s) == BASE_SIGHASH_ALL &&
            sighash_is_defined(s) &&
            !sighash_has_anyone_can_pay(s)) {
            struct sighash_type s2 = sighash_with_anyone_can_pay(s, true);
            if (sighash_has_anyone_can_pay(s2) && s2.raw == (SIGHASH_ALL | SIGHASH_ANYONECANPAY))
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        } else { printf("FAIL\n"); failures++; }
    }

    printf("coins init/alloc/spend... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 3);
        c.vout[0].value = 50 * COIN;
        c.vout[1].value = 25 * COIN;
        c.vout[2].value = 10 * COIN;
        c.is_coinbase = true;
        c.height = 100;
        if (coins_is_available(&c, 0) && coins_is_available(&c, 1) &&
            !coins_is_pruned(&c)) {
            coins_spend(&c, 1);
            if (!coins_is_available(&c, 1) && coins_is_available(&c, 0)) {
                coins_spend(&c, 0);
                coins_spend(&c, 2);
                if (coins_is_pruned(&c))
                    printf("OK\n");
                else { printf("FAIL (not pruned)\n"); failures++; }
            } else { printf("FAIL (spend)\n"); failures++; }
        } else { printf("FAIL (init)\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_from_transaction... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 2);
        tx.vout[0].value = 100 * COIN;
        tx.vout[1].value = 50 * COIN;
        tx.version = 1;

        struct coins c;
        coins_init(&c);
        coins_from_transaction(&c, &tx, 500);
        if (c.height == 500 && c.version == 1 &&
            c.is_coinbase && c.num_vout == 2 &&
            c.vout[0].value == 100 * COIN)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
        transaction_free(&tx);
    }

    printf("stream write/read u32... ");
    {
        struct byte_stream s;
        stream_init(&s, 64);
        stream_write_u32_le(&s, 0xDEADBEEF);
        stream_write_u32_le(&s, 0x12345678);
        s.read_pos = 0;
        uint32_t v1, v2;
        stream_read_u32_le(&s, &v1);
        stream_read_u32_le(&s, &v2);
        if (v1 == 0xDEADBEEF && v2 == 0x12345678 && s.size == 8)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("stream compact_size roundtrip... ");
    {
        struct byte_stream s;
        stream_init(&s, 64);
        stream_write_compact_size(&s, 0);
        stream_write_compact_size(&s, 252);
        stream_write_compact_size(&s, 253);
        stream_write_compact_size(&s, 0x10000);
        stream_write_compact_size(&s, 0x100000000ULL);
        s.read_pos = 0;
        uint64_t v;
        bool ok = true;
        stream_read_compact_size(&s, &v); ok &= (v == 0);
        stream_read_compact_size(&s, &v); ok &= (v == 252);
        stream_read_compact_size(&s, &v); ok &= (v == 253);
        stream_read_compact_size(&s, &v); ok &= (v == 0x10000);
        stream_read_compact_size(&s, &v); ok &= (v == 0x100000000ULL);
        if (ok && !s.error)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("stream varint roundtrip... ");
    {
        struct byte_stream s;
        stream_init(&s, 64);
        stream_write_varint(&s, 0);
        stream_write_varint(&s, 127);
        stream_write_varint(&s, 128);
        stream_write_varint(&s, 0xFFFF);
        stream_write_varint(&s, 0xFFFFFFFFULL);
        s.read_pos = 0;
        uint64_t v;
        bool ok = true;
        stream_read_varint(&s, &v); ok &= (v == 0);
        stream_read_varint(&s, &v); ok &= (v == 127);
        stream_read_varint(&s, &v); ok &= (v == 128);
        stream_read_varint(&s, &v); ok &= (v == 0xFFFF);
        stream_read_varint(&s, &v); ok &= (v == 0xFFFFFFFFULL);
        if (ok && !s.error)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("stream from_data read-only... ");
    {
        unsigned char data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        struct byte_stream s;
        stream_init_from_data(&s, data, sizeof(data));
        uint32_t v1, v2;
        stream_read_u32_le(&s, &v1);
        stream_read_u32_le(&s, &v2);
        if (v1 == 0x04030201 && v2 == 0x08070605 && stream_remaining(&s) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("block_header serialize/deserialize roundtrip... ");
    {
        struct block_header h;
        block_header_init(&h);
        h.nVersion = 4;
        h.nTime = 1234567890;
        h.nBits = 0x1d00ffff;
        memset(h.hashPrevBlock.data, 0xAA, 32);
        memset(h.hashMerkleRoot.data, 0xBB, 32);

        struct byte_stream s;
        stream_init(&s, 256);
        block_header_serialize(&h, &s);

        struct block_header h2;
        block_header_init(&h2);
        s.read_pos = 0;
        block_header_deserialize(&h2, &s);

        if (h2.nVersion == 4 && h2.nTime == 1234567890 &&
            h2.nBits == 0x1d00ffff &&
            uint256_cmp(&h2.hashPrevBlock, &h.hashPrevBlock) == 0 &&
            uint256_cmp(&h2.hashMerkleRoot, &h.hashMerkleRoot) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("block_header_get_hash... ");
    {
        struct block_header h;
        block_header_init(&h);
        h.nTime = 1000;
        h.nBits = 0x207fffff;
        struct uint256 hash;
        block_header_get_hash(&h, &hash);
        /* Just verify it produces a non-zero hash */
        if (!uint256_is_null(&hash))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("outpoint serialize/deserialize... ");
    {
        struct outpoint op = { .n = 42 };
        memset(op.hash.data, 0xAB, 32);
        struct byte_stream s;
        stream_init(&s, 64);
        outpoint_serialize(&op, &s);
        s.read_pos = 0;
        struct outpoint op2;
        outpoint_deserialize(&op2, &s);
        if (op2.n == 42 && memcmp(op2.hash.data, op.hash.data, 32) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("tx_in serialize/deserialize... ");
    {
        struct tx_in in;
        tx_in_init(&in);
        memset(in.prevout.hash.data, 0xCC, 32);
        in.prevout.n = 7;
        in.sequence = 0xFFFFFFFE;
        in.script_sig.data[0] = 0x01;
        in.script_sig.data[1] = 0x02;
        in.script_sig.size = 2;
        struct byte_stream s;
        stream_init(&s, 128);
        tx_in_serialize(&in, &s);
        s.read_pos = 0;
        struct tx_in in2;
        tx_in_init(&in2);
        tx_in_deserialize(&in2, &s);
        if (in2.prevout.n == 7 && in2.sequence == 0xFFFFFFFE &&
            in2.script_sig.size == 2 &&
            in2.script_sig.data[0] == 0x01 && in2.script_sig.data[1] == 0x02)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("tx_out serialize/deserialize... ");
    {
        struct tx_out out;
        out.value = 100000000;
        out.script_pub_key.size = 3;
        out.script_pub_key.data[0] = OP_DUP;
        out.script_pub_key.data[1] = OP_HASH160;
        out.script_pub_key.data[2] = OP_CHECKSIG;
        struct byte_stream s;
        stream_init(&s, 128);
        tx_out_serialize(&out, &s);
        s.read_pos = 0;
        struct tx_out out2;
        tx_out_set_null(&out2);
        tx_out_deserialize(&out2, &s);
        if (out2.value == 100000000 && out2.script_pub_key.size == 3 &&
            out2.script_pub_key.data[0] == OP_DUP)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("transaction_compute_hash... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        tx.vout[0].value = 50 * COIN;
        transaction_compute_hash(&tx);
        if (!uint256_is_null(&tx.hash))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("script_num roundtrip... ");
    {
        int64_t values[] = {0, 1, -1, 127, -128, 255, -255, 32767, -32768,
                            2147483647LL, -2147483647LL};
        bool ok = true;
        for (int i = 0; i < 11; i++) {
            struct script_num sn = script_num_from_int(values[i]);
            unsigned char buf[8];
            size_t len = script_num_serialize(&sn, buf, sizeof(buf));
            struct script_num sn2;
            if (!script_num_from_bytes(&sn2, buf, len, true, 8) ||
                sn2.value != values[i]) {
                ok = false; break;
            }
        }
        if (ok)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_get_op... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        unsigned char payload[] = {0xAA, 0xBB};
        script_push_data(&s, payload, 2);
        script_push_op(&s, OP_CHECKSIG);

        size_t pc = 0;
        enum opcodetype op;
        unsigned char data[520];
        size_t datalen;
        bool ok = true;
        ok &= script_get_op(&s, &pc, &op, data, &datalen);
        ok &= (op == OP_DUP && datalen == 0);
        ok &= script_get_op(&s, &pc, &op, data, &datalen);
        ok &= (datalen == 2 && data[0] == 0xAA && data[1] == 0xBB);
        ok &= script_get_op(&s, &pc, &op, data, &datalen);
        ok &= (op == OP_CHECKSIG);
        ok &= (pc == s.size);
        if (ok)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_is_push_only... ");
    {
        struct script s;
        script_init(&s);
        unsigned char data[] = {1, 2, 3};
        script_push_data(&s, data, 3);
        if (script_is_push_only(&s)) {
            script_push_op(&s, OP_CHECKSIG);
            if (!script_is_push_only(&s))
                printf("OK\n");
            else { printf("FAIL (non-push passed)\n"); failures++; }
        } else { printf("FAIL (push-only failed)\n"); failures++; }
    }

    printf("sigencoding valid DER... ");
    {
        unsigned char sig[70];
        sig[0] = 0x30; sig[1] = 68;
        sig[2] = 0x02; sig[3] = 32;
        memset(&sig[4], 0x01, 32);
        sig[36] = 0x02; sig[37] = 32;
        memset(&sig[38], 0x01, 32);
        ScriptError err = SCRIPT_ERR_OK;
        bool ok = check_data_signature_encoding(sig, 70, 0, &err);
        if (ok && err == SCRIPT_ERR_OK)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("sigencoding invalid DER... ");
    {
        unsigned char sig[] = {0x30, 0x01, 0x00};
        ScriptError err = SCRIPT_ERR_OK;
        bool ok = check_data_signature_encoding(sig, 3, 0, &err);
        if (!ok && err == SCRIPT_ERR_SIG_DER)
            printf("OK\n");
        else { printf("FAIL (ok=%d, err=%d)\n", ok, err); failures++; }
    }

    printf("sigencoding empty sig... ");
    {
        ScriptError err = SCRIPT_ERR_OK;
        if (check_data_signature_encoding(NULL, 0, 0, &err) &&
            check_transaction_signature_encoding(NULL, 0, 0, &err))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_pubkey_encoding... ");
    {
        unsigned char compressed[33] = {0x02};
        unsigned char uncompressed[65] = {0x04};
        unsigned char bad[10] = {0x05};
        ScriptError err;
        if (check_pubkey_encoding(compressed, 33, SCRIPT_VERIFY_STRICTENC, &err) &&
            check_pubkey_encoding(uncompressed, 65, SCRIPT_VERIFY_STRICTENC, &err) &&
            !check_pubkey_encoding(bad, 10, SCRIPT_VERIFY_STRICTENC, &err))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("eval_script OP_TRUE... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_TRUE);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1 && cast_to_bool(stack_top(&stk, -1)))
            printf("OK\n");
        else { printf("FAIL (ok=%d, count=%zu)\n", ok, stk.count); failures++; }
    }

    printf("eval_script OP_ADD... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_2);
        script_push_op(&s, OP_3);
        script_push_op(&s, OP_ADD);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1) {
            struct script_num sn;
            script_num_from_bytes(&sn, stack_top(&stk, -1)->data,
                                  stack_top(&stk, -1)->size, false, 4);
            if (sn.value == 5)
                printf("OK\n");
            else { printf("FAIL (value=%" PRId64 ")\n", sn.value); failures++; }
        } else { printf("FAIL\n"); failures++; }
    }

    printf("eval_script OP_EQUAL... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_5);
        script_push_op(&s, OP_5);
        script_push_op(&s, OP_EQUAL);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1 && cast_to_bool(stack_top(&stk, -1)))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("eval_script OP_DUP OP_HASH160... ");
    {
        struct script s;
        script_init(&s);
        unsigned char data[] = {0x01, 0x02, 0x03};
        script_push_data(&s, data, 3);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 2 && stack_top(&stk, -1)->size == 20)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("verify_script P2PKH (no checker)... ");
    {
        struct key_id kid;
        memset(&kid, 0xAB, sizeof(kid));
        struct script spk;
        script_for_p2pkh(&spk, &kid);
        struct script ss;
        script_init(&ss);
        ScriptError err;
        bool ok = verify_script(&ss, &spk, 0, NULL, 0, &err);
        if (!ok)
            printf("OK (correctly fails without sig)\n");
        else { printf("FAIL (should have failed)\n"); failures++; }
    }

    printf("eval_script OP_IF/OP_ELSE/OP_ENDIF... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_1);
        script_push_op(&s, OP_IF);
        script_push_op(&s, OP_2);
        script_push_op(&s, OP_ELSE);
        script_push_op(&s, OP_3);
        script_push_op(&s, OP_ENDIF);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1) {
            struct script_num sn;
            script_num_from_bytes(&sn, stack_top(&stk, -1)->data,
                                  stack_top(&stk, -1)->size, false, 4);
            if (sn.value == 2)
                printf("OK\n");
            else { printf("FAIL (value=%" PRId64 ")\n", sn.value); failures++; }
        } else { printf("FAIL\n"); failures++; }
    }

    printf("validation_state... ");
    {
        struct validation_state vs;
        validation_state_init(&vs);
        if (validation_state_is_valid(&vs)) {
            validation_state_dos(&vs, 10, false, REJECT_INVALID,
                                 "bad-txns", false, NULL);
            int dos = 0;
            if (validation_state_is_invalid(&vs) &&
                validation_state_get_dos(&vs, &dos) && dos == 10 &&
                strcmp(vs.reject_reason, "bad-txns") == 0)
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        } else { printf("FAIL (init)\n"); failures++; }
    }

    printf("sigcache set/get/erase... ");
    {
        struct sig_cache cache;
        sig_cache_init(&cache);
        struct uint256 hash;
        memset(hash.data, 0x42, 32);
        unsigned char sig[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01};
        unsigned char pk[] = {0x02, 0x01};
        struct uint256 entry;
        sig_cache_compute_entry(&cache, &entry, &hash, sig, 8, pk, 2);
        if (!sig_cache_get(&cache, &entry)) {
            sig_cache_set(&cache, &entry);
            if (sig_cache_get(&cache, &entry)) {
                sig_cache_erase(&cache, &entry);
                if (!sig_cache_get(&cache, &entry))
                    printf("OK\n");
                else { printf("FAIL (erase)\n"); failures++; }
            } else { printf("FAIL (get after set)\n"); failures++; }
        } else { printf("FAIL (false positive)\n"); failures++; }
        sig_cache_destroy(&cache);
    }

    printf("pagelocker lock/unlock... ");
    {
        struct locked_page_manager m;
        locked_page_manager_init(&m);
        unsigned char buf[64];
        locked_page_manager_lock_range(&m, buf, sizeof(buf));
        int count = locked_page_manager_get_count(&m);
        locked_page_manager_unlock_range(&m, buf, sizeof(buf));
        int count2 = locked_page_manager_get_count(&m);
        if (count >= 1 && count2 == 0)
            printf("OK (locked=%d, unlocked=%d)\n", count, count2);
        else { printf("FAIL (locked=%d, unlocked=%d)\n", count, count2); failures++; }
        locked_page_manager_destroy(&m);
    }

    printf("lock_object/unlock_object... ");
    {
        unsigned char secret[32];
        memset(secret, 0xAA, 32);
        lock_object(secret, sizeof(secret));
        unlock_object(secret, sizeof(secret));
        bool zeroed = true;
        for (int i = 0; i < 32; i++) {
            if (secret[i] != 0) { zeroed = false; break; }
        }
        if (zeroed)
            printf("OK (memory cleansed)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("encode_destination pubkey... ");
    {
        const unsigned char pubkey_pfx[] = {0x1C, 0xB8};
        const unsigned char script_pfx[] = {0x1C, 0xBD};
        struct tx_destination dest;
        dest.type = DEST_KEY_ID;
        memset(dest.id.key.id.data, 0x01, 20);
        char addr[64];
        if (encode_destination(&dest, pubkey_pfx, 2, script_pfx, 2, addr, sizeof(addr)) &&
            strlen(addr) > 20) {
            struct tx_destination decoded;
            if (decode_destination(addr, pubkey_pfx, 2, script_pfx, 2, &decoded) &&
                decoded.type == DEST_KEY_ID &&
                memcmp(decoded.id.key.id.data, dest.id.key.id.data, 20) == 0)
                printf("OK\n");
            else { printf("FAIL (decode mismatch)\n"); failures++; }
        } else { printf("FAIL (encode)\n"); failures++; }
    }

    printf("encode_destination script... ");
    {
        const unsigned char pubkey_pfx[] = {0x1C, 0xB8};
        const unsigned char script_pfx[] = {0x1C, 0xBD};
        struct tx_destination dest;
        dest.type = DEST_SCRIPT_ID;
        memset(dest.id.script.hash.data, 0xAB, 20);
        char addr[64];
        if (encode_destination(&dest, pubkey_pfx, 2, script_pfx, 2, addr, sizeof(addr))) {
            struct tx_destination decoded;
            if (decode_destination(addr, pubkey_pfx, 2, script_pfx, 2, &decoded) &&
                decoded.type == DEST_SCRIPT_ID &&
                memcmp(decoded.id.script.hash.data, dest.id.script.hash.data, 20) == 0)
                printf("OK\n");
            else { printf("FAIL (decode mismatch)\n"); failures++; }
        } else { printf("FAIL (encode)\n"); failures++; }
    }

    printf("encode/decode_secret roundtrip... ");
    {
        const unsigned char secret_pfx[] = {0x80};
        struct privkey key;
        privkey_init(&key);
        memset(key.vch, 0x42, 32);
        key.fValid = true;
        key.fCompressed = true;
        char wif[64];
        if (encode_secret(&key, secret_pfx, 1, wif, sizeof(wif))) {
            struct privkey decoded;
            if (decode_secret(wif, secret_pfx, 1, &decoded) &&
                decoded.fCompressed == true &&
                memcmp(decoded.vch, key.vch, 32) == 0)
                printf("OK\n");
            else { printf("FAIL (decode mismatch)\n"); failures++; }
        } else { printf("FAIL (encode)\n"); failures++; }
    }

    printf("decode_destination invalid... ");
    {
        const unsigned char pubkey_pfx[] = {0x1C, 0xB8};
        const unsigned char script_pfx[] = {0x1C, 0xBD};
        struct tx_destination dest;
        dest.type = DEST_KEY_ID;
        if (!decode_destination("1invalidaddress", pubkey_pfx, 2, script_pfx, 2, &dest))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("chainparams mainnet... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *p = chain_params_get();
        size_t pfx_len;
        const unsigned char *pfx = chain_params_base58_prefix(p, B58_PUBKEY_ADDRESS, &pfx_len);
        if (pfx_len == 2 && pfx[0] == 0x1C && pfx[1] == 0xB8 &&
            p->nDefaultPort == 8033 &&
            p->consensus.vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight == 707000 &&
            strcmp(p->strCurrencyUnits, "ZCL") == 0 &&
            p->nFoundersRewardAddresses == 48)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("chainparams testnet... ");
    {
        chain_params_select(CHAIN_TESTNET);
        const struct chain_params *p = chain_params_get();
        size_t pfx_len;
        const unsigned char *pfx = chain_params_base58_prefix(p, B58_PUBKEY_ADDRESS, &pfx_len);
        if (pfx_len == 2 && pfx[0] == 0x1D && pfx[1] == 0x25 &&
            p->nDefaultPort == 18033 &&
            strcmp(p->strCurrencyUnits, "ZCT") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("chainparams regtest... ");
    {
        chain_params_select(CHAIN_REGTEST);
        const struct chain_params *p = chain_params_get();
        if (p->nEquihashN == 48 && p->nEquihashK == 5 &&
            p->fMineBlocksOnDemand == true &&
            p->fMiningRequiresPeers == false &&
            strcmp(p->strNetworkID, "regtest") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("chainparams address roundtrip via params... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *p = chain_params_get();
        size_t pk_len, sc_len;
        const unsigned char *pk_pfx = chain_params_base58_prefix(p, B58_PUBKEY_ADDRESS, &pk_len);
        const unsigned char *sc_pfx = chain_params_base58_prefix(p, B58_SCRIPT_ADDRESS, &sc_len);
        struct tx_destination dest;
        dest.type = DEST_KEY_ID;
        memset(dest.id.key.id.data, 0x42, 20);
        char addr[64];
        if (encode_destination(&dest, pk_pfx, pk_len, sc_pfx, sc_len, addr, sizeof(addr))) {
            struct tx_destination decoded;
            if (decode_destination(addr, pk_pfx, pk_len, sc_pfx, sc_len, &decoded) &&
                decoded.type == DEST_KEY_ID &&
                memcmp(decoded.id.key.id.data, dest.id.key.id.data, 20) == 0)
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        } else { printf("FAIL (encode)\n"); failures++; }
    }

    printf("get_block_subsidy slow start... ");
    {
        chain_params_select(CHAIN_MAIN);
        const struct chain_params *p = chain_params_get();
        int64_t s0 = get_block_subsidy(0, &p->consensus);
        int64_t s1 = get_block_subsidy(1, &p->consensus);
        if (s0 == 0 && s1 == 1250000000)
            printf("OK\n");
        else { printf("FAIL (s0=%" PRId64 " s1=%" PRId64 ")\n", s0, s1); failures++; }
    }

    printf("get_block_subsidy full reward... ");
    {
        const struct chain_params *p = chain_params_get();
        int64_t s = get_block_subsidy(10, &p->consensus);
        if (s == 1250000000)
            printf("OK\n");
        else { printf("FAIL (%" PRId64 ")\n", s); failures++; }
    }

    printf("get_block_subsidy pre-buttercup... ");
    {
        const struct chain_params *p = chain_params_get();
        int64_t s = get_block_subsidy(706999, &p->consensus);
        /* halvings = (706999 - 1) / 840000 = 0, subsidy = 12.5 ZCL */
        if (s == 1250000000)
            printf("OK\n");
        else { printf("FAIL (%" PRId64 ")\n", s); failures++; }
    }

    printf("get_block_subsidy buttercup... ");
    {
        const struct chain_params *p = chain_params_get();
        /* At buttercup: halvings = 0 + 3 = 3, subsidy/2 >> 3 = 78125000 */
        int64_t s = get_block_subsidy(707001, &p->consensus);
        if (s == 78125000)
            printf("OK\n");
        else { printf("FAIL (%" PRId64 ")\n", s); failures++; }
    }

    printf("signature_hash sprout... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        tx.overwintered = false;
        tx.vin[0].sequence = 0xffffffff;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vout[0].value = 100000000;
        tx.vout[0].script_pub_key.data[0] = OP_DUP;
        tx.vout[0].script_pub_key.size = 1;
        tx.lock_time = 0;

        struct script sc;
        sc.data[0] = OP_DUP;
        sc.size = 1;

        struct sighash_type ht = sighash_type_default();
        struct uint256 result;
        bool ok = signature_hash(&sc, &tx, 0, ht, 0, 0, NULL, &result);
        if (ok && !uint256_is_null(&result))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("signature_hash sapling... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = SAPLING_TX_VERSION;
        tx.overwintered = true;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.vin[0].sequence = 0xffffffff;
        memset(tx.vin[0].prevout.hash.data, 0x22, 32);
        tx.vin[0].prevout.n = 0;
        tx.vout[0].value = 50000000;
        tx.vout[0].script_pub_key.data[0] = OP_DUP;
        tx.vout[0].script_pub_key.size = 1;
        tx.lock_time = 0;
        tx.expiry_height = 500000;
        tx.value_balance = 0;

        struct script sc;
        sc.data[0] = OP_DUP;
        sc.size = 1;

        struct sighash_type ht = sighash_type_default();
        struct uint256 result;
        uint32_t branch_id = 0x76b809bb; /* Sapling branch ID */
        bool ok = signature_hash(&sc, &tx, 0, ht, 50000000, branch_id, NULL, &result);
        if (ok && !uint256_is_null(&result))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("precomputed_tx_data... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 2, 1);
        tx.version = SAPLING_TX_VERSION;
        tx.overwintered = true;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        for (int i = 0; i < 2; i++) {
            tx.vin[i].sequence = 0xffffffff;
            memset(tx.vin[i].prevout.hash.data, (unsigned char)(0x10 + i), 32);
            tx.vin[i].prevout.n = (uint32_t)i;
        }
        tx.vout[0].value = 100000000;
        tx.vout[0].script_pub_key.size = 0;

        struct precomputed_tx_data cache;
        precompute_tx_data(&tx, &cache);

        struct script sc;
        sc.size = 0;
        struct sighash_type ht = sighash_type_default();
        uint32_t branch_id = 0x76b809bb;

        struct uint256 r1, r2;
        signature_hash(&sc, &tx, 0, ht, 100000000, branch_id, NULL, &r1);
        signature_hash(&sc, &tx, 0, ht, 100000000, branch_id, &cache, &r2);

        if (memcmp(r1.data, r2.data, 32) == 0 && !uint256_is_null(&r1))
            printf("OK\n");
        else { printf("FAIL (cache mismatch)\n"); failures++; }
        transaction_free(&tx);
    }

    printf("check_transaction valid... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        tx.overwintered = false;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xffffffff;
        tx.vin[0].script_sig.data[0] = 0x01;
        tx.vin[0].script_sig.data[1] = 0x01;
        tx.vin[0].script_sig.size = 2;
        tx.vout[0].value = 50 * COIN;
        tx.vout[0].script_pub_key.data[0] = OP_DUP;
        tx.vout[0].script_pub_key.size = 1;

        struct validation_state state;
        validation_state_init(&state);
        if (check_transaction(&tx, &state))
            printf("OK\n");
        else { printf("FAIL (%s)\n", state.reject_reason); failures++; }
        transaction_free(&tx);
    }

    printf("check_transaction negative output... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 2;
        tx.vout[0].value = -1;

        struct validation_state state;
        validation_state_init(&state);
        if (!check_transaction(&tx, &state) &&
            strcmp(state.reject_reason, "bad-txns-vout-negative") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("check_transaction empty vin... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 0, 1);
        tx.version = 1;
        tx.vout[0].value = COIN;

        struct validation_state state;
        validation_state_init(&state);
        if (!check_transaction(&tx, &state) &&
            strcmp(state.reject_reason, "bad-txns-vin-empty") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("check_transaction duplicate inputs... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 2, 1);
        tx.version = 1;
        memset(tx.vin[0].prevout.hash.data, 0x22, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 2;
        memcpy(&tx.vin[1].prevout, &tx.vin[0].prevout, sizeof(struct outpoint));
        tx.vin[1].script_sig.size = 2;
        tx.vout[0].value = COIN;

        struct validation_state state;
        validation_state_init(&state);
        if (!check_transaction(&tx, &state) &&
            strcmp(state.reject_reason, "bad-txns-inputs-duplicate") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("check_transaction overwinter version... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = SAPLING_TX_VERSION;
        tx.overwintered = true;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = 1000;
        memset(tx.vin[0].prevout.hash.data, 0x33, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 2;
        tx.vout[0].value = COIN;

        struct validation_state state;
        validation_state_init(&state);
        if (check_transaction(&tx, &state))
            printf("OK\n");
        else { printf("FAIL (%s)\n", state.reject_reason); failures++; }
        transaction_free(&tx);
    }

    printf("tx_sig_checker create... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = SAPLING_TX_VERSION;
        tx.overwintered = true;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.vin[0].sequence = 0xffffffff;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vout[0].value = 50 * COIN;
        tx.vout[0].script_pub_key.size = 0;

        struct tx_sig_checker tsc;
        tx_sig_checker_init(&tsc, &tx, 0, 50 * COIN, 0x76b809bb, NULL);
        struct sig_checker checker = tx_make_sig_checker(&tsc);

        if (checker.check_sig != NULL &&
            checker.check_lock_time != NULL &&
            checker.verify_signature != NULL &&
            checker.ctx == &tsc)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("tx_sig_checker bad sig... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = SAPLING_TX_VERSION;
        tx.overwintered = true;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.vin[0].sequence = 0xffffffff;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vout[0].value = COIN;
        tx.vout[0].script_pub_key.size = 0;

        struct tx_sig_checker tsc;
        tx_sig_checker_init(&tsc, &tx, 0, COIN, 0x76b809bb, NULL);

        struct script sc;
        sc.size = 0;
        unsigned char fake_sig[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01};
        unsigned char fake_pk[] = {0x04};
        /* Should fail: invalid pubkey */
        if (!tx_sig_checker_check_sig(&tsc, fake_sig, sizeof(fake_sig),
                                       fake_pk, 1, &sc))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        transaction_free(&tx);
    }

    printf("script_get_sig_op_count... ");
    {
        struct script s;
        s.data[0] = OP_CHECKSIG;
        s.data[1] = OP_CHECKSIG;
        s.data[2] = OP_CHECKMULTISIG;
        s.size = 3;
        uint32_t n = script_get_sig_op_count(&s, 0, false);
        /* 2 CHECKSIG + 20 CHECKMULTISIG (inaccurate) = 22 */
        if (n == 22)
            printf("OK\n");
        else { printf("FAIL (%u)\n", n); failures++; }
    }

    printf("script_get_sig_op_count accurate... ");
    {
        struct script s;
        s.data[0] = OP_2;
        s.data[1] = OP_CHECKMULTISIG;
        s.size = 2;
        uint32_t n = script_get_sig_op_count(&s, 0, true);
        /* OP_2 then CHECKMULTISIG → 2 keys */
        if (n == 2)
            printf("OK\n");
        else { printf("FAIL (%u)\n", n); failures++; }
    }

    printf("get_legacy_sig_op_count... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = COIN;
        tx.vout[0].script_pub_key.data[0] = OP_CHECKSIG;
        tx.vout[0].script_pub_key.size = 1;
        uint64_t ops = get_legacy_sig_op_count(&tx, 0);
        if (ops == 1)
            printf("OK\n");
        else { printf("FAIL (%" PRIu64 ")\n", ops); failures++; }
        transaction_free(&tx);
    }

    printf("script_is_pay_to_script_hash... ");
    {
        struct script s;
        s.data[0] = OP_HASH160;
        s.data[1] = 0x14;
        memset(s.data + 2, 0xAA, 20);
        s.data[22] = OP_EQUAL;
        s.size = 23;
        if (script_is_pay_to_script_hash(&s))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("contextual_check_tx sprout rejects overwinter... ");
    {
        const struct consensus_params *p = &chain_params_get()->consensus;
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.overwintered = true;
        tx.version = OVERWINTER_TX_VERSION;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = COIN;
        tx.vout[0].script_pub_key.size = 0;
        struct validation_state state;
        validation_state_init(&state);
        /* height 1 is Sprout on mainnet */
        bool ok = contextual_check_transaction(&tx, &state, p, 1, 100);
        if (!ok && strcmp(state.reject_reason, "tx-overwinter-not-active") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d reason=%s)\n", ok, state.reject_reason); failures++; }
        transaction_free(&tx);
    }

    printf("contextual_check_tx sapling valid... ");
    {
        const struct consensus_params *p = &chain_params_get()->consensus;
        int sapHeight = p->vUpgrades[UPGRADE_SAPLING].nActivationHeight;
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.overwintered = true;
        tx.version = SAPLING_TX_VERSION;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = (uint32_t)(sapHeight + 100);
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = COIN;
        tx.vout[0].script_pub_key.size = 0;
        struct validation_state state;
        validation_state_init(&state);
        bool ok = contextual_check_transaction(&tx, &state, p, sapHeight, 100);
        if (ok)
            printf("OK\n");
        else { printf("FAIL (reason=%s)\n", state.reject_reason); failures++; }
        transaction_free(&tx);
    }

    printf("contextual_check_tx expired... ");
    {
        const struct consensus_params *p = &chain_params_get()->consensus;
        int sapHeight = p->vUpgrades[UPGRADE_SAPLING].nActivationHeight;
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.overwintered = true;
        tx.version = SAPLING_TX_VERSION;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = (uint32_t)sapHeight;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = COIN;
        tx.vout[0].script_pub_key.size = 0;
        struct validation_state state;
        validation_state_init(&state);
        bool ok = contextual_check_transaction(&tx, &state, p, sapHeight, 100);
        if (!ok && strcmp(state.reject_reason, "tx-overwinter-expired") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d reason=%s)\n", ok, state.reject_reason); failures++; }
        transaction_free(&tx);
    }

    printf("is_expired_tx... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        tx.overwintered = true;
        tx.expiry_height = 500;
        if (is_expired_tx(&tx, 500) && !is_expired_tx(&tx, 499) && !is_expired_tx(&tx, 0))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("tx_in_undo serialize/deserialize roundtrip... ");
    {
        struct tx_in_undo u;
        tx_in_undo_init(&u);
        u.coinbase = true;
        u.height = 12345;
        u.version = 2;
        u.txout.value = 50 * COIN;
        /* P2PKH script */
        u.txout.script_pub_key.data[0] = OP_DUP;
        u.txout.script_pub_key.data[1] = OP_HASH160;
        u.txout.script_pub_key.data[2] = 20;
        memset(u.txout.script_pub_key.data + 3, 0xAB, 20);
        u.txout.script_pub_key.data[23] = OP_EQUALVERIFY;
        u.txout.script_pub_key.data[24] = OP_CHECKSIG;
        u.txout.script_pub_key.size = 25;

        struct byte_stream s;
        stream_init(&s, 256);
        tx_in_undo_serialize(&u, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct tx_in_undo u2;
        tx_in_undo_init(&u2);
        tx_in_undo_deserialize(&u2, &r);

        if (u2.coinbase == true && u2.height == 12345 && u2.version == 2 &&
            u2.txout.value == 50 * COIN &&
            u2.txout.script_pub_key.size == 25 &&
            u2.txout.script_pub_key.data[0] == OP_DUP &&
            memcmp(u2.txout.script_pub_key.data + 3, u.txout.script_pub_key.data + 3, 20) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("block_undo serialize/deserialize roundtrip... ");
    {
        struct block_undo bu;
        block_undo_init(&bu);
        block_undo_alloc(&bu, 1);
        tx_undo_alloc(&bu.vtxundo[0], 2);
        bu.vtxundo[0].vprevout[0].coinbase = false;
        bu.vtxundo[0].vprevout[0].height = 100;
        bu.vtxundo[0].vprevout[0].version = 1;
        bu.vtxundo[0].vprevout[0].txout.value = COIN;
        bu.vtxundo[0].vprevout[0].txout.script_pub_key.data[0] = OP_RETURN;
        bu.vtxundo[0].vprevout[0].txout.script_pub_key.size = 1;
        bu.vtxundo[0].vprevout[1].coinbase = true;
        bu.vtxundo[0].vprevout[1].height = 50;
        bu.vtxundo[0].vprevout[1].version = 1;
        bu.vtxundo[0].vprevout[1].txout.value = 2 * COIN;
        bu.vtxundo[0].vprevout[1].txout.script_pub_key.data[0] = OP_TRUE;
        bu.vtxundo[0].vprevout[1].txout.script_pub_key.size = 1;
        memset(bu.old_sprout_tree_root.data, 0xCC, 32);

        struct byte_stream s;
        stream_init(&s, 512);
        block_undo_serialize(&bu, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct block_undo bu2;
        block_undo_init(&bu2);
        block_undo_deserialize(&bu2, &r);

        if (bu2.num_txundo == 1 &&
            bu2.vtxundo[0].num_prevout == 2 &&
            bu2.vtxundo[0].vprevout[0].height == 100 &&
            bu2.vtxundo[0].vprevout[1].coinbase == true &&
            bu2.vtxundo[0].vprevout[1].txout.value == 2 * COIN &&
            bu2.old_sprout_tree_root.data[0] == 0xCC)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        block_undo_free(&bu);
        block_undo_free(&bu2);
        stream_free(&s);
    }

    printf("net_address serialize/deserialize roundtrip... ");
    {
        struct net_address a;
        net_address_init(&a);
        a.nServices = NODE_NETWORK | NODE_BLOOM;
        a.nTime = 1700000000;
        a.svc.addr.ip[12] = 192; a.svc.addr.ip[13] = 168;
        a.svc.addr.ip[14] = 1; a.svc.addr.ip[15] = 1;
        a.svc.port = 8233;
        struct byte_stream s;
        stream_init(&s, 64);
        net_address_serialize(&a, &s, true);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct net_address a2;
        net_address_init(&a2);
        net_address_deserialize(&a2, &r, true);
        if (a2.nTime == 1700000000 &&
            a2.nServices == (NODE_NETWORK | NODE_BLOOM) &&
            a2.svc.addr.ip[12] == 192 && a2.svc.port == 8233)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("inv_item serialize/deserialize roundtrip... ");
    {
        struct inv_item inv;
        struct uint256 h;
        memset(h.data, 0xAA, 32);
        inv_item_init_typed(&inv, MSG_TX, &h);
        struct byte_stream s;
        stream_init(&s, 64);
        inv_item_serialize(&inv, &s);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct inv_item inv2;
        inv_item_deserialize(&inv2, &r);
        if (inv2.type == MSG_TX && inv2.hash.data[0] == 0xAA)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("block_locator serialize/deserialize roundtrip... ");
    {
        struct block_locator loc;
        block_locator_init(&loc);
        loc.num_hashes = 3;
        loc.vhave = calloc(3, sizeof(struct uint256));
        memset(loc.vhave[0].data, 0x11, 32);
        memset(loc.vhave[1].data, 0x22, 32);
        memset(loc.vhave[2].data, 0x33, 32);
        struct byte_stream s;
        stream_init(&s, 128);
        block_locator_serialize(&loc, &s);
        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct block_locator loc2;
        block_locator_init(&loc2);
        block_locator_deserialize(&loc2, &r);
        if (loc2.num_hashes == 3 &&
            loc2.vhave[0].data[0] == 0x11 &&
            loc2.vhave[2].data[0] == 0x33)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        block_locator_free(&loc);
        block_locator_free(&loc2);
        stream_free(&s);
    }

    printf("version_message serialize/deserialize roundtrip... ");
    {
        struct version_message v;
        version_message_init(&v);
        v.protocol_version = 170009;
        v.services = NODE_NETWORK;
        v.timestamp = 1700000000;
        v.addr_recv.nServices = NODE_NETWORK;
        v.addr_recv.svc.port = 8233;
        v.addr_from.nServices = NODE_NETWORK;
        v.addr_from.svc.port = 8233;
        v.nonce = 0xDEADBEEFCAFEBABEULL;
        snprintf(v.sub_version, MAX_SUBVER_LENGTH, "/ZClassic:2.1.1-3/");
        v.start_height = 500000;
        v.relay = true;

        struct byte_stream s;
        stream_init(&s, 256);
        version_message_serialize(&v, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct version_message v2;
        version_message_init(&v2);
        version_message_deserialize(&v2, &r);

        if (v2.protocol_version == 170009 &&
            v2.services == NODE_NETWORK &&
            v2.timestamp == 1700000000 &&
            v2.nonce == 0xDEADBEEFCAFEBABEULL &&
            strcmp(v2.sub_version, "/ZClassic:2.1.1-3/") == 0 &&
            v2.start_height == 500000 &&
            v2.relay == true &&
            v2.addr_recv.svc.port == 8233)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&s);
    }

    printf("split_host_port... ");
    {
        char host[128];
        int port = 8233;
        split_host_port("192.168.1.1:9033", host, sizeof(host), &port);
        if (strcmp(host, "192.168.1.1") == 0 && port == 9033)
            printf("OK\n");
        else { printf("FAIL (host=%s port=%d)\n", host, port); failures++; }
    }

    printf("split_host_port ipv6... ");
    {
        char host[128];
        int port = 8233;
        split_host_port("[::1]:9033", host, sizeof(host), &port);
        if (strcmp(host, "::1") == 0 && port == 9033)
            printf("OK\n");
        else { printf("FAIL (host=%s port=%d)\n", host, port); failures++; }
    }

    printf("lookup_host numeric ipv4... ");
    {
        struct net_addr addrs[4];
        size_t n = 0;
        bool ok = lookup_host("127.0.0.1", addrs, 4, &n, false);
        if (ok && n == 1 && addrs[0].ip[12] == 127 && addrs[0].ip[15] == 1)
            printf("OK\n");
        else { printf("FAIL (ok=%d n=%zu)\n", ok, n); failures++; }
    }

    printf("lookup_numeric... ");
    {
        struct net_service svc;
        bool ok = lookup_numeric("10.0.0.1:8233", &svc, 0);
        if (ok && svc.addr.ip[12] == 10 && svc.port == 8233)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("millis_to_timeval... ");
    {
        struct timeval tv = millis_to_timeval(5500);
        if (tv.tv_sec == 5 && tv.tv_usec == 500000)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("net_addr RFC classification... ");
    {
        struct net_addr a;
        net_addr_init(&a);

        unsigned char priv10[] = {10, 0, 0, 1};
        net_addr_set_ipv4(&a, priv10);
        bool ok = net_addr_is_rfc1918(&a);

        unsigned char pub8[] = {8, 8, 8, 8};
        net_addr_set_ipv4(&a, pub8);
        ok = ok && !net_addr_is_rfc1918(&a);
        ok = ok && net_addr_is_routable(&a);

        unsigned char local127[] = {127, 0, 0, 1};
        net_addr_set_ipv4(&a, local127);
        ok = ok && net_addr_is_local(&a);
        ok = ok && !net_addr_is_routable(&a);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("net_addr_get_group ipv4... ");
    {
        struct net_addr a;
        net_addr_init(&a);
        unsigned char ip[] = {1, 2, 3, 4};
        net_addr_set_ipv4(&a, ip);
        unsigned char group[NET_ADDR_GROUP_MAX];
        size_t glen = net_addr_get_group(&a, group, sizeof(group));
        bool ok = glen == 3 && group[0] == NET_IPV4 &&
                  group[1] == 1 && group[2] == 2;
        if (ok) printf("OK\n");
        else { printf("FAIL (len=%zu g0=%d g1=%d g2=%d)\n", glen, group[0], group[1], group[2]); failures++; }
    }

    printf("net_service_get_key... ");
    {
        struct net_service s;
        net_service_init(&s);
        unsigned char ip[] = {192, 168, 1, 1};
        net_addr_set_ipv4(&s.addr, ip);
        s.port = 8233;
        unsigned char key[18];
        net_service_get_key(&s, key);
        bool ok = key[16] == (8233 >> 8) && key[17] == (8233 & 0xFF);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("merkle_tree serialize/deserialize roundtrip... ");
    {
        struct uint256 txids[4];
        bool match[4] = {false, true, false, true};
        for (int i = 0; i < 4; i++)
            memset(txids[i].data, i + 1, 32);

        struct partial_merkle_tree tree;
        merkle_tree_init(&tree);
        merkle_tree_build(&tree, txids, 4, match, 4);

        struct byte_stream ws;
        stream_init(&ws, 256);
        bool ok = merkle_tree_serialize(&tree, &ws);

        struct partial_merkle_tree tree2;
        merkle_tree_init(&tree2);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        ok = ok && merkle_tree_deserialize(&tree2, &rs);

        ok = ok && tree2.num_transactions == tree.num_transactions;
        ok = ok && tree2.num_hashes == tree.num_hashes;
        for (size_t i = 0; i < tree.num_hashes && ok; i++)
            ok = ok && uint256_cmp(&tree.hashes[i], &tree2.hashes[i]) == 0;

        struct uint256 matched1[4], matched2[4], root1, root2;
        size_t nm1 = 0, nm2 = 0;
        merkle_tree_extract(&tree, matched1, &nm1, &root1);
        merkle_tree_extract(&tree2, matched2, &nm2, &root2);
        ok = ok && nm1 == nm2 && uint256_cmp(&root1, &root2) == 0;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }

        merkle_tree_free(&tree);
        merkle_tree_free(&tree2);
        stream_free(&ws);
        stream_free(&rs);
    }

    printf("merkle_block serialize/deserialize roundtrip... ");
    {
        struct merkle_block mb;
        merkle_block_init(&mb);
        mb.header.nVersion = 4;
        mb.header.nBits = 0x1d00ffff;
        mb.header.nTime = 1231006505;
        memset(mb.header.hashPrevBlock.data, 0xAA, 32);

        struct uint256 txids[2];
        bool match[2] = {true, false};
        memset(txids[0].data, 0x11, 32);
        memset(txids[1].data, 0x22, 32);
        merkle_tree_build(&mb.txn, txids, 2, match, 2);

        struct byte_stream ws;
        stream_init(&ws, 2048);
        bool ok = merkle_block_serialize(&mb, &ws);

        struct merkle_block mb2;
        merkle_block_init(&mb2);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        ok = ok && merkle_block_deserialize(&mb2, &rs);

        ok = ok && mb2.header.nVersion == 4;
        ok = ok && mb2.header.nBits == 0x1d00ffff;
        ok = ok && mb2.txn.num_transactions == 2;
        ok = ok && mb2.txn.num_hashes == mb.txn.num_hashes;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }

        merkle_block_free(&mb);
        merkle_block_free(&mb2);
        stream_free(&ws);
        stream_free(&rs);
    }

    printf("transaction serialize/deserialize roundtrip... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0xAB, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 50 * 100000000LL;
        tx.lock_time = 0;
        transaction_compute_hash(&tx);
        struct uint256 orig_hash = tx.hash;

        struct byte_stream ws;
        stream_init(&ws, 512);
        bool ok = transaction_serialize(&tx, &ws);

        struct transaction tx2;
        transaction_init(&tx2);
        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        ok = ok && transaction_deserialize(&tx2, &rs);

        ok = ok && tx2.num_vin == 1 && tx2.num_vout == 1;
        ok = ok && tx2.vout[0].value == 50 * 100000000LL;
        ok = ok && uint256_cmp(&tx2.hash, &orig_hash) == 0;

        size_t sz = transaction_serialize_size(&tx);
        ok = ok && sz == ws.size;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }

        transaction_free(&tx);
        transaction_free(&tx2);
        stream_free(&ws);
        stream_free(&rs);
    }

    printf("zcl_consensus_version... ");
    {
        if (zcl_consensus_version() == ZCASHCONSENSUS_API_VER)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("validation_signals register/dispatch... ");
    {
        test_tip_count = 0;
        test_tip_height = 0;

        struct validation_signals vs;
        validation_signals_init(&vs);

        struct validation_callbacks cb;
        memset(&cb, 0, sizeof(cb));
        cb.ctx = NULL;
        cb.updated_block_tip = test_updated_block_tip;

        validation_register(&vs, &cb);
        signal_updated_block_tip(&vs, 42);

        bool ok = (test_tip_count == 1 && test_tip_height == 42);

        signal_updated_block_tip(&vs, 100);
        ok = ok && test_tip_count == 2 && test_tip_height == 100;

        validation_unregister(&vs, NULL);
        signal_updated_block_tip(&vs, 200);
        ok = ok && test_tip_count == 2;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("validation_signals unregister_all... ");
    {
        struct validation_signals vs;
        validation_signals_init(&vs);

        struct validation_callbacks cb1, cb2;
        memset(&cb1, 0, sizeof(cb1));
        memset(&cb2, 0, sizeof(cb2));
        int ctx1 = 0, ctx2 = 0;
        cb1.ctx = &ctx1;
        cb2.ctx = &ctx2;

        validation_register(&vs, &cb1);
        validation_register(&vs, &cb2);
        bool ok = (vs.num_listeners == 2);

        validation_unregister_all(&vs);
        ok = ok && (vs.num_listeners == 0);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("addrman init/add/size... ");
    {
        struct addr_man am;
        addrman_init(&am);
        bool ok = addrman_size(&am) == 0;

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip1[] = {8, 8, 8, 8};
        net_addr_set_ipv4(&addr.svc.addr, ip1);
        addr.svc.port = 8233;
        addr.nTime = (uint32_t)GetTime();

        struct net_addr source;
        net_addr_init(&source);
        unsigned char src_ip[] = {1, 2, 3, 4};
        net_addr_set_ipv4(&source, src_ip);

        bool added = addrman_add(&am, &addr, &source, 0);
        ok = ok && added;
        ok = ok && addrman_size(&am) == 1;

        if (ok) printf("OK\n");
        else { printf("FAIL (added=%d size=%zu)\n", added, addrman_size(&am)); failures++; }
        addrman_free(&am);
    }

    printf("addrman select... ");
    {
        struct addr_man am;
        addrman_init(&am);

        for (int i = 0; i < 10; i++) {
            struct net_address addr;
            net_address_init(&addr);
            unsigned char ip[] = {50 + (unsigned char)i, 100, 0, 1};
            net_addr_set_ipv4(&addr.svc.addr, ip);
            addr.svc.port = 8233;
            addr.nTime = (uint32_t)GetTime();

            struct net_addr source;
            net_addr_init(&source);
            unsigned char src_ip[] = {60, 2, 3, (unsigned char)(i + 1)};
            net_addr_set_ipv4(&source, src_ip);

            addrman_add(&am, &addr, &source, 0);
        }

        bool ok = addrman_size(&am) == 10;
        struct addr_info result;
        bool selected = addrman_select(&am, true, &result);
        ok = ok && selected;
        ok = ok && result.addr.svc.port == 8233;

        if (ok) printf("OK\n");
        else { printf("FAIL (size=%zu sel=%d)\n", addrman_size(&am), selected); failures++; }
        addrman_free(&am);
    }

    printf("addr_info bucket computation... ");
    {
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        unsigned char ip[] = {192, 168, 1, 1};
        net_addr_set_ipv4(&info.addr.svc.addr, ip);
        info.addr.svc.port = 8233;

        struct uint256 key;
        memset(key.data, 0x42, 32);

        int tried_bucket = addr_info_get_tried_bucket(&info, &key);
        int new_bucket = addr_info_get_new_bucket(&info, &key, &info.addr.svc.addr);
        int pos = addr_info_get_bucket_position(&info, &key, true, new_bucket);

        bool ok = tried_bucket >= 0 && tried_bucket < ADDRMAN_TRIED_BUCKET_COUNT;
        ok = ok && new_bucket >= 0 && new_bucket < ADDRMAN_NEW_BUCKET_COUNT;
        ok = ok && pos >= 0 && pos < ADDRMAN_BUCKET_SIZE;

        if (ok) printf("OK\n");
        else { printf("FAIL (tb=%d nb=%d pos=%d)\n", tried_bucket, new_bucket, pos); failures++; }
    }

    printf("addr_info_is_terrible... ");
    {
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        info.addr.nTime = 0;
        bool ok = addr_info_is_terrible(&info, GetTime());

        info.addr.nTime = (uint32_t)GetTime();
        info.last_try = GetTime();
        ok = ok && !addr_info_is_terrible(&info, GetTime());

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("net_manager init/free... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);
        nm.default_port = 8233;
        bool ok = (nm.max_connections == DEFAULT_MAX_PEER_CONNECTIONS);
        ok = ok && (nm.discover == true);
        ok = ok && (nm.listen == true);
        ok = ok && (nm.num_nodes == 0);
        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("net_message framing... ");
    {
        unsigned char msgstart[4] = {0xfa, 0x1a, 0xf9, 0xbf};
        struct net_message msg;
        net_message_init(&msg, msgstart);

        struct msg_header hdr;
        msg_header_init_full(&hdr, msgstart, "ping", 8);
        unsigned char payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

        uint8_t wire[MSG_HEADER_SIZE + 8];
        memcpy(wire, &hdr, MSG_HEADER_SIZE);
        memcpy(wire + MSG_HEADER_SIZE, payload, 8);

        int r1 = net_message_read_header(&msg, (const char *)wire, MSG_HEADER_SIZE);
        bool ok = (r1 == MSG_HEADER_SIZE);
        ok = ok && msg.in_data;
        ok = ok && (msg.hdr.nMessageSize == 8);

        int r2 = net_message_read_data(&msg, (const char *)payload, 8);
        ok = ok && (r2 == 8);
        ok = ok && net_message_complete(&msg);
        ok = ok && (memcmp(msg.recv_data, payload, 8) == 0);

        net_message_free(&msg);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("p2p_node create/free... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8233;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                 &addr, "test-peer", false);
        bool ok = (node != NULL);
        ok = ok && (node->id == 0);
        ok = ok && (node->inbound == false);
        ok = ok && (node->disconnect == false);
        ok = ok && (node->version == 0);
        ok = ok && (strcmp(node->addr_name, "test-peer") == 0);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("ban management... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);

        struct net_addr addr;
        net_addr_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr, ip4);

        bool ok = !is_banned(&nm, &addr);
        ban_addr(&nm, &addr, 3600, false);
        ok = ok && is_banned(&nm, &addr);
        ok = ok && unban_addr(&nm, &addr);
        ok = ok && !is_banned(&nm, &addr);

        ban_addr(&nm, &addr, 3600, false);
        clear_banned(&nm);
        ok = ok && !is_banned(&nm, &addr);

        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("local address management... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);

        struct net_service svc;
        net_service_init(&svc);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&svc.addr, ip4);
        svc.port = 8233;

        bool ok = !is_local(&nm, &svc);
        ok = ok && add_local(&nm, &svc, LOCAL_BIND);
        ok = ok && is_local(&nm, &svc);
        ok = ok && remove_local(&nm, &svc);
        ok = ok && !is_local(&nm, &svc);

        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("set_limited/is_reachable... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);

        bool ok = is_reachable_net(&nm, NET_IPV4);
        set_limited(&nm, NET_IPV4, true);
        ok = ok && !is_reachable_net(&nm, NET_IPV4);
        set_limited(&nm, NET_IPV4, false);
        ok = ok && is_reachable_net(&nm, NET_IPV4);

        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("node_stats copy... ");
    {
        struct net_manager nm;
        net_manager_init(&nm);
        memcpy(nm.message_start, "\xfa\x1a\xf9\xbf", 4);

        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {50, 0, 0, 1};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8233;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                 &addr, "stats-test", true);
        node->version = 170002;
        snprintf(node->clean_sub_ver, sizeof(node->clean_sub_ver),
                 "/ZClassic:1.0.0/");

        struct node_stats stats;
        p2p_node_copy_stats(node, &stats);

        bool ok = (stats.nodeid == 0);
        ok = ok && (stats.version == 170002);
        ok = ok && (stats.inbound == true);
        ok = ok && (strcmp(stats.clean_sub_ver, "/ZClassic:1.0.0/") == 0);

        p2p_node_free(node);
        net_manager_free(&nm);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool init/free... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);
        bool ok = tx_mempool_size(&pool) == 0;
        ok = ok && tx_mempool_total_size(&pool) == 0;
        ok = ok && tx_mempool_txs_updated(&pool) == 0;
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool add/exists/lookup... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0xAB, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 50 * COIN_VALUE;
        tx.lock_time = 0;
        transaction_compute_hash(&tx);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 10000, 1700000000, 1e9, 100,
                           true, false, 0);

        bool ok = tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
        ok = ok && tx_mempool_size(&pool) == 1;
        ok = ok && tx_mempool_exists(&pool, &tx.hash);
        ok = ok && tx_mempool_total_size(&pool) > 0;

        struct transaction found;
        transaction_init(&found);
        ok = ok && tx_mempool_lookup(&pool, &tx.hash, &found);
        ok = ok && uint256_eq(&found.hash, &tx.hash);
        ok = ok && found.vout[0].value == 50 * COIN_VALUE;

        transaction_free(&found);
        mempool_entry_free(&entry);
        transaction_free(&tx);
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool remove... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0xCD, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 25 * COIN_VALUE;
        transaction_compute_hash(&tx);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 5000, 1700000000, 1e8, 200,
                           true, false, 0);
        tx_mempool_add_unchecked(&pool, &tx.hash, &entry);

        bool ok = tx_mempool_size(&pool) == 1;
        tx_mempool_remove(&pool, &tx.hash);
        ok = ok && tx_mempool_size(&pool) == 0;
        ok = ok && !tx_mempool_exists(&pool, &tx.hash);

        mempool_entry_free(&entry);
        transaction_free(&tx);
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool clear... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        for (int i = 0; i < 5; i++) {
            struct transaction tx;
            transaction_init(&tx);
            transaction_alloc(&tx, 1, 1);
            memset(tx.vin[0].prevout.hash.data, (unsigned char)(i + 1), 32);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].sequence = 0xFFFFFFFF;
            tx.vout[0].value = (int64_t)(i + 1) * COIN_VALUE;
            transaction_compute_hash(&tx);

            struct mempool_entry entry;
            mempool_entry_init(&entry, &tx, 1000, 1700000000, 1e6, 100,
                               true, false, 0);
            tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
            mempool_entry_free(&entry);
            transaction_free(&tx);
        }

        bool ok = tx_mempool_size(&pool) == 5;
        tx_mempool_clear(&pool);
        ok = ok && tx_mempool_size(&pool) == 0;
        ok = ok && tx_mempool_total_size(&pool) == 0;

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool prioritise/apply_deltas... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct uint256 hash;
        memset(hash.data, 0xEE, 32);

        tx_mempool_prioritise(&pool, &hash, 100.0, 5000);

        double pd = 0.0;
        int64_t fd = 0;
        tx_mempool_apply_deltas(&pool, &hash, &pd, &fd);
        bool ok = (pd == 100.0 && fd == 5000);

        tx_mempool_prioritise(&pool, &hash, 50.0, 2000);
        pd = 0.0; fd = 0;
        tx_mempool_apply_deltas(&pool, &hash, &pd, &fd);
        ok = ok && (pd == 150.0 && fd == 7000);

        tx_mempool_clear_prioritisation(&pool, &hash);
        pd = 0.0; fd = 0;
        tx_mempool_apply_deltas(&pool, &hash, &pd, &fd);
        ok = ok && (pd == 0.0 && fd == 0);

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool query_hashes... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        for (int i = 0; i < 3; i++) {
            struct transaction tx;
            transaction_init(&tx);
            transaction_alloc(&tx, 1, 1);
            memset(tx.vin[0].prevout.hash.data, (unsigned char)(0x10 + i), 32);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].sequence = 0xFFFFFFFF;
            tx.vout[0].value = COIN_VALUE;
            transaction_compute_hash(&tx);

            struct mempool_entry entry;
            mempool_entry_init(&entry, &tx, 1000, 1700000000, 1e6, 100,
                               true, false, 0);
            tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
            mempool_entry_free(&entry);
            transaction_free(&tx);
        }

        struct uint256 out[10];
        size_t num_out = 0;
        tx_mempool_query_hashes(&pool, out, 10, &num_out);
        bool ok = (num_out == 3);

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool remove_without_branch_id... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        for (int i = 0; i < 4; i++) {
            struct transaction tx;
            transaction_init(&tx);
            transaction_alloc(&tx, 1, 1);
            memset(tx.vin[0].prevout.hash.data, (unsigned char)(0x20 + i), 32);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].sequence = 0xFFFFFFFF;
            tx.vout[0].value = COIN_VALUE;
            transaction_compute_hash(&tx);

            struct mempool_entry entry;
            mempool_entry_init(&entry, &tx, 1000, 1700000000, 1e6, 100,
                               true, false, (i < 2) ? 0x76b809bbU : 0x892f2085U);
            tx_mempool_add_unchecked(&pool, &tx.hash, &entry);
            mempool_entry_free(&entry);
            transaction_free(&tx);
        }

        bool ok = tx_mempool_size(&pool) == 4;
        tx_mempool_remove_without_branch_id(&pool, 0x892f2085U);
        ok = ok && tx_mempool_size(&pool) == 2;

        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("txmempool has_no_inputs_of... ");
    {
        struct tx_mempool pool;
        tx_mempool_init(&pool, 1000);

        struct transaction tx1;
        transaction_init(&tx1);
        transaction_alloc(&tx1, 1, 1);
        memset(tx1.vin[0].prevout.hash.data, 0x30, 32);
        tx1.vin[0].prevout.n = 0;
        tx1.vin[0].sequence = 0xFFFFFFFF;
        tx1.vout[0].value = COIN_VALUE;
        transaction_compute_hash(&tx1);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx1, 1000, 1700000000, 1e6, 100,
                           true, false, 0);
        tx_mempool_add_unchecked(&pool, &tx1.hash, &entry);

        struct transaction tx2;
        transaction_init(&tx2);
        transaction_alloc(&tx2, 1, 1);
        tx2.vin[0].prevout.hash = tx1.hash;
        tx2.vin[0].prevout.n = 0;
        tx2.vin[0].sequence = 0xFFFFFFFF;
        tx2.vout[0].value = COIN_VALUE;
        transaction_compute_hash(&tx2);

        bool ok = !tx_mempool_has_no_inputs_of(&pool, &tx2);

        struct transaction tx3;
        transaction_init(&tx3);
        transaction_alloc(&tx3, 1, 1);
        memset(tx3.vin[0].prevout.hash.data, 0xFF, 32);
        tx3.vin[0].prevout.n = 0;
        tx3.vin[0].sequence = 0xFFFFFFFF;
        tx3.vout[0].value = COIN_VALUE;
        transaction_compute_hash(&tx3);

        ok = ok && tx_mempool_has_no_inputs_of(&pool, &tx3);

        mempool_entry_free(&entry);
        transaction_free(&tx1);
        transaction_free(&tx2);
        transaction_free(&tx3);
        tx_mempool_free(&pool);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("mempool_entry get_priority... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        memset(tx.vin[0].prevout.hash.data, 0x40, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].sequence = 0xFFFFFFFF;
        tx.vout[0].value = 10 * COIN_VALUE;
        transaction_compute_hash(&tx);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 50000, 1700000000, 1000.0, 100,
                           true, false, 0);

        double p = mempool_entry_get_priority(&entry, 200);
        bool ok = (p > 1000.0);

        mempool_entry_free(&entry);
        transaction_free(&tx);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("tx_confirm_stats init/setup/find_bucket... ");
    {
        struct tx_confirm_stats s;
        tx_confirm_stats_init(&s);
        double bkts[] = {10.0, 100.0, 1000.0, 10000.0};
        tx_confirm_stats_setup(&s, bkts, 4, 10, 0.998);

        bool ok = s.num_buckets == 5;
        ok = ok && s.max_confirms == 10;

        ok = ok && tx_confirm_stats_find_bucket(&s, 5.0) == 0;
        ok = ok && tx_confirm_stats_find_bucket(&s, 10.0) == 0;
        ok = ok && tx_confirm_stats_find_bucket(&s, 50.0) == 1;
        ok = ok && tx_confirm_stats_find_bucket(&s, 5000.0) == 3;
        ok = ok && tx_confirm_stats_find_bucket(&s, 99999.0) == 4;

        tx_confirm_stats_free(&s);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("tx_confirm_stats record/update... ");
    {
        struct tx_confirm_stats s;
        tx_confirm_stats_init(&s);
        double bkts[] = {100.0, 1000.0, 10000.0};
        tx_confirm_stats_setup(&s, bkts, 3, 5, 0.998);

        tx_confirm_stats_clear_current(&s, 1);
        tx_confirm_stats_record(&s, 1, 500.0);
        tx_confirm_stats_record(&s, 2, 500.0);
        tx_confirm_stats_update_averages(&s);

        bool ok = s.tx_ct_avg[1] > 0;
        ok = ok && s.avg[1] > 0;

        tx_confirm_stats_free(&s);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("block_policy_estimator init/free... ");
    {
        struct fee_rate min_fee;
        min_fee.satoshis_per_k = 1000;
        struct block_policy_estimator est;
        block_policy_estimator_init(&est, &min_fee);

        bool ok = est.best_seen_height == 0;
        ok = ok && est.fee_stats.num_buckets > 0;
        ok = ok && est.pri_stats.num_buckets > 0;
        ok = ok && est.min_tracked_fee.satoshis_per_k >= 10;

        block_policy_estimator_free(&est);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("block_policy_estimator estimate_fee empty... ");
    {
        struct fee_rate min_fee;
        min_fee.satoshis_per_k = 1000;
        struct block_policy_estimator est;
        block_policy_estimator_init(&est, &min_fee);

        struct fee_rate r = policy_estimate_fee(&est, 2);
        bool ok = r.satoshis_per_k == 0;
        double p = policy_estimate_priority(&est, 2);
        ok = ok && p == -1;

        block_policy_estimator_free(&est);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("policy is_fee/pri_data_point... ");
    {
        struct fee_rate min_fee;
        min_fee.satoshis_per_k = 1000;
        struct block_policy_estimator est;
        block_policy_estimator_init(&est, &min_fee);

        struct fee_rate high_fee;
        high_fee.satoshis_per_k = 50000;
        bool ok = policy_is_fee_data_point(&est, &high_fee, 0.0);

        struct fee_rate zero_fee;
        zero_fee.satoshis_per_k = 0;
        ok = ok && policy_is_pri_data_point(&est, &zero_fee, 1e12);

        block_policy_estimator_free(&est);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json null/bool/int/str... ");
    {
        struct json_value v;
        json_init(&v);
        bool ok = json_is_null(&v);

        json_set_bool(&v, true);
        ok = ok && json_get_bool(&v);

        json_set_int(&v, 42);
        ok = ok && json_get_int(&v) == 42;

        json_set_str(&v, "hello");
        ok = ok && strcmp(json_get_str(&v), "hello") == 0;

        json_set_real(&v, 3.14);
        ok = ok && json_get_real(&v) > 3.13 && json_get_real(&v) < 3.15;

        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json object write... ");
    {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "method", "getinfo");
        json_push_kv_int(&obj, "id", 1);

        char buf[256];
        json_write(&obj, buf, sizeof(buf));
        bool ok = strstr(buf, "\"method\":\"getinfo\"") != NULL;
        ok = ok && strstr(buf, "\"id\":1") != NULL;

        json_free(&obj);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json array write... ");
    {
        struct json_value arr;
        json_init(&arr);
        json_set_array(&arr);
        struct json_value v;
        json_init(&v);
        json_set_int(&v, 10);
        json_push_back(&arr, &v);
        json_set_int(&v, 20);
        json_push_back(&arr, &v);
        json_free(&v);

        char buf[64];
        json_write(&arr, buf, sizeof(buf));
        bool ok = strcmp(buf, "[10,20]") == 0;

        json_free(&arr);
        if (ok) printf("OK\n"); else { printf("FAIL (got: %s)\n", buf); failures++; }
    }

    printf("json read object... ");
    {
        const char *input = "{\"name\":\"zcl\",\"port\":8233,\"active\":true}";
        struct json_value v;
        bool ok = json_read(&v, input, strlen(input));
        ok = ok && v.type == JSON_OBJ;
        ok = ok && json_size(&v) == 3;

        const struct json_value *name = json_get(&v, "name");
        ok = ok && name && strcmp(json_get_str(name), "zcl") == 0;

        const struct json_value *port = json_get(&v, "port");
        ok = ok && port && json_get_int(port) == 8233;

        const struct json_value *active = json_get(&v, "active");
        ok = ok && active && json_get_bool(active);

        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json read array... ");
    {
        const char *input = "[1,\"two\",null,false]";
        struct json_value v;
        bool ok = json_read(&v, input, strlen(input));
        ok = ok && v.type == JSON_ARR;
        ok = ok && json_size(&v) == 4;
        ok = ok && json_get_int(json_at(&v, 0)) == 1;
        ok = ok && strcmp(json_get_str(json_at(&v, 1)), "two") == 0;
        ok = ok && json_is_null(json_at(&v, 2));
        ok = ok && !json_get_bool(json_at(&v, 3));

        json_free(&v);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("json roundtrip... ");
    {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "result", "ok");
        json_push_kv_int(&obj, "code", 200);

        char buf[256];
        size_t n = json_write(&obj, buf, sizeof(buf));

        struct json_value parsed;
        bool ok = json_read(&parsed, buf, n);
        ok = ok && parsed.type == JSON_OBJ;
        const struct json_value *r = json_get(&parsed, "result");
        ok = ok && r && strcmp(json_get_str(r), "ok") == 0;
        const struct json_value *c = json_get(&parsed, "code");
        ok = ok && c && json_get_int(c) == 200;

        json_free(&obj);
        json_free(&parsed);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
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
