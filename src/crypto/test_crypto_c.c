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

static int check_hex(const unsigned char *data, size_t len, const char *expected)
{
    char buf[256];
    for (size_t i = 0; i < len; i++)
        snprintf(buf + i * 2, 3, "%02x", data[i]);
    if (strcmp(buf, expected) != 0) {
        printf("FAIL\n  got:      %s\n  expected: %s\n", buf, expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;
    unsigned char hash[64];

    /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    printf("SHA-256(\"\")... ");
    struct sha256_ctx sha256;
    sha256_init(&sha256);
    sha256_finalize(&sha256, hash);
    failures += check_hex(hash, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    if (!failures) printf("OK\n");

    /* SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
    printf("SHA-256(\"abc\")... ");
    sha256_init(&sha256);
    sha256_write(&sha256, (const unsigned char *)"abc", 3);
    sha256_finalize(&sha256, hash);
    failures += check_hex(hash, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    if (!failures) printf("OK\n");

    /* SHA-512("") */
    printf("SHA-512(\"\")... ");
    struct sha512_ctx sha512;
    sha512_init(&sha512);
    sha512_finalize(&sha512, hash);
    failures += check_hex(hash, 64, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    if (!failures) printf("OK\n");

    /* SHA-512("abc") */
    printf("SHA-512(\"abc\")... ");
    sha512_init(&sha512);
    sha512_write(&sha512, (const unsigned char *)"abc", 3);
    sha512_finalize(&sha512, hash);
    failures += check_hex(hash, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    if (!failures) printf("OK\n");

    /* SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d */
    printf("SHA-1(\"abc\")... ");
    struct sha1_ctx sha1;
    sha1_init(&sha1);
    sha1_write(&sha1, (const unsigned char *)"abc", 3);
    sha1_finalize(&sha1, hash);
    failures += check_hex(hash, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
    if (!failures) printf("OK\n");

    /* RIPEMD-160("abc") = 8eb208f7e05d987a9b044a8e98c6b087f15a0bfc */
    printf("RIPEMD-160(\"abc\")... ");
    struct ripemd160_ctx rmd;
    ripemd160_init(&rmd);
    ripemd160_write(&rmd, (const unsigned char *)"abc", 3);
    ripemd160_finalize(&rmd, hash);
    failures += check_hex(hash, 20, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
    if (!failures) printf("OK\n");

    /* HMAC-SHA256 with empty key and empty data */
    printf("HMAC-SHA256(\"\",\"\")... ");
    struct hmac_sha256_ctx hmac256;
    hmac_sha256_init(&hmac256, (const unsigned char *)"", 0);
    hmac_sha256_finalize(&hmac256, hash);
    failures += check_hex(hash, 32, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    if (!failures) printf("OK\n");

    printf("\n%s (%d failures)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
