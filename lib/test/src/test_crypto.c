/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Crypto hash tests: SHA-256, SHA-512, SHA-1, RIPEMD-160, HMAC-SHA256,
 * BLAKE2b, Hash256, Hash160. */

#include "test/test_helpers.h"

int test_crypto(void)
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

    printf("SHA-256 self-test (%s)... ", sha256_implementation());
    if (sha256_selftest()) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    /* Stress test: SHA-256 1MB of data — verify both paths agree */
    printf("SHA-256 1MB stress test... ");
    {
        unsigned char *big = malloc(1024 * 1024);
        for (int i = 0; i < 1024 * 1024; i++)
            big[i] = (unsigned char)(i * 137 + 73);

        unsigned char h1[32], h2[32];

        struct sha256_ctx c1;
        sha256_init(&c1);
        sha256_write(&c1, big, 1024 * 1024);
        sha256_finalize(&c1, h1);

        /* Second pass — must match */
        struct sha256_ctx c2;
        sha256_init(&c2);
        sha256_write(&c2, big, 1024 * 1024);
        sha256_finalize(&c2, h2);

        free(big);

        if (memcmp(h1, h2, 32) == 0) printf("OK\n");
        else { printf("FAIL (non-deterministic)\n"); failures++; }
    }

    /* Benchmark: SHA-256 throughput */
    printf("SHA-256 benchmark (%s)... ", sha256_implementation());
    {
        unsigned char block[64];
        memset(block, 0x42, 64);
        uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        struct timeval t1, t2;
        gettimeofday(&t1, NULL);
        int iters = 1000000;
        struct sha256_ctx bench;
        for (int i = 0; i < iters; i++) {
            sha256_init(&bench);
            sha256_write(&bench, block, 64);
            sha256_finalize(&bench, (unsigned char *)state);
        }
        gettimeofday(&t2, NULL);
        double elapsed = (double)(t2.tv_sec - t1.tv_sec) +
                          (double)(t2.tv_usec - t1.tv_usec) / 1e6;
        double mbs = (double)iters * 64.0 / elapsed / 1e6;
        printf("OK (%.0f MB/s, %d hashes in %.3fs)\n", mbs, iters, elapsed);
    }

    return failures;
}
