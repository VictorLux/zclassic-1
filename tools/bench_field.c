/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Benchmark: field arithmetic acceleration measurement.
 * Compares portable vs BMI2+ADX vs AVX-512 IFMA paths. */

#include "sapling/fr.h"
#include "sapling/bls12_381.h"
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

/* From fr_avx512.c */
extern const char *fr_accel_implementation(void);
extern bool fr_accel_has_bmi2_adx(void);
extern bool fr_accel_has_avx512ifma(void);

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    printf("=== ZClassic23 Field Arithmetic Benchmark ===\n");
    printf("Implementation: %s\n", fr_accel_implementation());
    printf("BMI2+ADX: %s\n", fr_accel_has_bmi2_adx() ? "yes" : "no");
    printf("AVX-512 IFMA: %s\n", fr_accel_has_avx512ifma() ? "yes" : "no");
    printf("\n");

    /* Initialize test values */
    uint8_t a_bytes[32] = {0};
    uint8_t b_bytes[32] = {0};
    a_bytes[0] = 42; a_bytes[7] = 0x13; a_bytes[15] = 0x37;
    b_bytes[0] = 99; b_bytes[3] = 0x55; b_bytes[11] = 0xAA;

    struct fr a, b, r;
    fr_from_bytes(&a, a_bytes);
    fr_from_bytes(&b, b_bytes);

    /* Warm up */
    for (int i = 0; i < 10000; i++)
        fr_mul(&r, &a, &b);

    /* --- Fr multiply benchmark --- */
    {
        int iters = 10000000;
        uint64_t t0 = now_ns();
        struct fr acc = a;
        for (int i = 0; i < iters; i++)
            fr_mul(&acc, &acc, &b);
        uint64_t elapsed = now_ns() - t0;
        double ns_per = (double)elapsed / iters;
        double muls_per_sec = 1e9 / ns_per;
        printf("Fr multiply:    %.1f ns/op  (%.1f M ops/sec)\n",
               ns_per, muls_per_sec / 1e6);
        /* Prevent optimizer from removing the loop */
        if (fr_is_zero(&acc)) printf("(zero)\n");
    }

    /* --- Fr add benchmark --- */
    {
        int iters = 10000000;
        uint64_t t0 = now_ns();
        struct fr acc = a;
        for (int i = 0; i < iters; i++)
            fr_add(&acc, &acc, &b);
        uint64_t elapsed = now_ns() - t0;
        double ns_per = (double)elapsed / iters;
        printf("Fr add:         %.1f ns/op  (%.1f M ops/sec)\n",
               ns_per, 1e9 / ns_per / 1e6);
        if (fr_is_zero(&acc)) printf("(zero)\n");
    }

    /* --- Fr square benchmark --- */
    {
        int iters = 10000000;
        uint64_t t0 = now_ns();
        struct fr acc = a;
        for (int i = 0; i < iters; i++)
            fr_sq(&acc, &acc);
        uint64_t elapsed = now_ns() - t0;
        double ns_per = (double)elapsed / iters;
        printf("Fr square:      %.1f ns/op  (%.1f M ops/sec)\n",
               ns_per, 1e9 / ns_per / 1e6);
        if (fr_is_zero(&acc)) printf("(zero)\n");
    }

    /* --- Fp multiply benchmark --- */
    {
        uint8_t fa_bytes[48] = {0}, fb_bytes[48] = {0};
        fa_bytes[0] = 42; fa_bytes[10] = 0x37;
        fb_bytes[0] = 99; fb_bytes[20] = 0xAA;
        struct fp fa, fb, fr_fp;
        fp_from_bytes(&fa, fa_bytes);
        fp_from_bytes(&fb, fb_bytes);

        int iters = 10000000;
        uint64_t t0 = now_ns();
        struct fp acc = fa;
        for (int i = 0; i < iters; i++)
            fp_mul(&acc, &acc, &fb);
        uint64_t elapsed = now_ns() - t0;
        double ns_per = (double)elapsed / iters;
        printf("Fp multiply:    %.1f ns/op  (%.1f M ops/sec)\n",
               ns_per, 1e9 / ns_per / 1e6);
        if (fp_is_zero(&acc)) printf("(zero)\n");
    }

    /* --- Jubjub point add benchmark --- */
    {
        /* Use generator-like point */
        struct jub_point p, q, result;
        uint8_t gen_bytes[32] = {
            0x8d, 0x51, 0xcc, 0xce, 0x85, 0x2c, 0xea, 0x65,
            0xa2, 0x80, 0xaf, 0x6e, 0x19, 0x51, 0x3e, 0x9a,
            0x56, 0x77, 0x33, 0x02, 0x7b, 0x0e, 0x0a, 0x3b,
            0x37, 0x55, 0xb3, 0xb6, 0x35, 0x2c, 0x52, 0x19
        };
        if (!jub_from_bytes(&p, gen_bytes)) {
            printf("Jubjub: using identity\n");
            jub_identity(&p);
        }
        q = p;
        jub_double(&q, &p);

        int iters = 1000000;
        uint64_t t0 = now_ns();
        result = p;
        for (int i = 0; i < iters; i++)
            jub_add(&result, &result, &q);
        uint64_t elapsed = now_ns() - t0;
        double us_per = (double)elapsed / iters / 1000.0;
        printf("Jubjub add:     %.2f us/op (%.0f K ops/sec)\n",
               us_per, 1e6 / (us_per * 1000.0) * 1000.0);
        if (jub_is_identity(&result)) printf("(identity)\n");
    }

    /* --- Jubjub scalar mul benchmark --- */
    {
        struct jub_point p, result;
        jub_identity(&p);
        /* Create a non-identity point */
        struct fr one_val;
        fr_one(&one_val);
        p.x = one_val;
        fr_one(&p.y);
        fr_one(&p.z);
        fr_mul(&p.t, &p.x, &p.y);

        uint8_t scalar[32] = {0};
        scalar[0] = 0x42; scalar[15] = 0x13; scalar[31] = 0x07;

        int iters = 1000;
        uint64_t t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            scalar[0] = (uint8_t)(i & 0xFF);
            jub_scalar_mul(&result, &p, scalar);
        }
        uint64_t elapsed = now_ns() - t0;
        double ms_per = (double)elapsed / iters / 1e6;
        printf("Jubjub scalmul: %.2f ms/op\n", ms_per);
    }

    /* --- G1 point add benchmark --- */
    {
        struct g1_point p, q, result;
        g1_identity(&p);
        g1_identity(&q);
        /* Use generator (1, 2) */
        fp_one(&p.x);
        uint8_t two[48] = {0}; two[0] = 2;
        fp_from_bytes(&p.y, two);
        fp_one(&p.z);
        q = p;
        g1_double(&q, &p);

        int iters = 100000;
        uint64_t t0 = now_ns();
        result = p;
        for (int i = 0; i < iters; i++)
            g1_add(&result, &result, &q);
        uint64_t elapsed = now_ns() - t0;
        double us_per = (double)elapsed / iters / 1000.0;
        printf("G1 add:         %.1f us/op  (%.0f K ops/sec)\n",
               us_per, 1e6 / (us_per * 1000.0) * 1000.0);
    }

    printf("\n=== Shielded Transaction Impact ===\n");
    printf("Groth16 proving is dominated by:\n");
    printf("  1. Multi-scalar multiplication (MSM) — ~60%% of time\n");
    printf("  2. FFT for quotient polynomial — ~20%% of time\n");
    printf("  3. Field arithmetic (Fr/Fp mul) — inner loop of both\n");
    printf("\n");
    printf("With BMI2+ADX: MULX+ADCX+ADOX carry chains\n");
    printf("With AVX-512 IFMA: 8 Fr multiplies per clock (batch FFT)\n");
    printf("With parallel MSM: N threads on Pippenger windows\n");

    return 0;
}
