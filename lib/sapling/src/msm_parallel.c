/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Parallel multi-scalar multiplication (Pippenger's algorithm).
 * Splits bucket accumulation across pthreads for ~Nx speedup on N cores.
 *
 * Also: parallel FFT for Groth16 quotient polynomial computation. */

#include "sapling/groth16_prover.h"
#include "sapling/bls12_381.h"
#include "sapling/fr.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "util/safe_alloc.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

/* ── Helper: convert Fr from Montgomery to raw ──────────────────── */

static void msm_fr_to_raw(uint64_t raw[4], const struct fr *a)
{
    /* Same as fr_to_bytes but to uint64_t directly.
     * Multiply by 1 to undo Montgomery form. */
    uint8_t bytes[32];
    fr_to_bytes(bytes, a);
    for (int i = 0; i < 4; i++) {
        raw[i] = 0;
        for (int j = 0; j < 8; j++)
            raw[i] |= (uint64_t)bytes[i * 8 + j] << (j * 8);
    }
}

/* ── Parallel G1 MSM ────────────────────────────────────────────── */

struct g1_msm_window_args {
    const struct g1_point *points;
    const uint64_t (*raw_scalars)[4];
    size_t n;
    unsigned int c;           /* window size in bits */
    int window_index;         /* which window this thread handles */
    struct g1_point result;   /* output: window contribution */
};

static void *g1_msm_window_thread(void *arg)
{
    struct g1_msm_window_args *a = (struct g1_msm_window_args *)arg;
    int w = a->window_index;
    unsigned int c = a->c;
    size_t n = a->n;
    size_t num_buckets = ((size_t)1 << c) - 1;

    struct g1_point *buckets = zcl_calloc(num_buckets, sizeof(struct g1_point), "g1_par_buckets");
    if (!buckets) { g1_identity(&a->result); return NULL; }

    for (size_t b = 0; b < num_buckets; b++)
        g1_identity(&buckets[b]);

    unsigned int bit_offset = (unsigned int)w * c;
    for (size_t i = 0; i < n; i++) {
        unsigned int word = bit_offset / 64;
        unsigned int bit = bit_offset % 64;
        if (word >= 4) continue;
        uint64_t val = a->raw_scalars[i][word] >> bit;
        if (bit + c > 64 && word + 1 < 4)
            val |= a->raw_scalars[i][word + 1] << (64 - bit);
        val &= ((uint64_t)1 << c) - 1;
        if (val == 0) continue;
        g1_add(&buckets[val - 1], &buckets[val - 1], &a->points[i]);
    }

    /* Reduce buckets: running_sum accumulation */
    struct g1_point running_sum, window_sum;
    g1_identity(&running_sum);
    g1_identity(&window_sum);
    for (size_t b = num_buckets; b > 0; b--) {
        g1_add(&running_sum, &running_sum, &buckets[b - 1]);
        g1_add(&window_sum, &window_sum, &running_sum);
    }

    a->result = window_sum;
    free(buckets);
    return NULL;
}

void g1_msm_parallel(struct g1_point *result,
                     const struct g1_point *points, const struct fr *scalars,
                     size_t n, int num_threads)
{
    if (n == 0) { g1_identity(result); return; }
    if (num_threads <= 1 || n < 64) {
        /* Fall back to serial MSM */
        g1_msm(result, points, scalars, n);
        return;
    }

    /* Window size: c ≈ log2(n), clamped to [4, 16] */
    unsigned int c = 1;
    { size_t v = n; while (v > 1) { c++; v >>= 1; } }
    if (c > 16) c = 16;
    if (c < 4 && n > 16) c = 4;

    unsigned int num_windows = (255 + c - 1) / c;

    /* Convert scalars from Montgomery form */
    uint64_t (*raw_scalars)[4] = zcl_calloc(n, sizeof(uint64_t[4]), "g1_par_scalars");
    if (!raw_scalars) { g1_identity(result); return; }
    for (size_t i = 0; i < n; i++)
        msm_fr_to_raw(raw_scalars[i], &scalars[i]);

    /* Launch one thread per window (up to num_threads at a time) */
    struct g1_msm_window_args *args = zcl_calloc(num_windows,
                                              sizeof(struct g1_msm_window_args), "g1_par_args");
    pthread_t *threads = zcl_calloc(num_windows, sizeof(pthread_t), "g1_par_threads");
    if (!args || !threads) {
        free(raw_scalars); free(args); free(threads);
        g1_identity(result);
        return;
    }

    for (unsigned int w = 0; w < num_windows; w++) {
        args[w].points = points;
        args[w].raw_scalars = raw_scalars;
        args[w].n = n;
        args[w].c = c;
        args[w].window_index = (int)w;
    }

    /* Launch threads in batches of num_threads */
    for (unsigned int batch_start = 0; batch_start < num_windows;
         batch_start += (unsigned int)num_threads) {
        unsigned int batch_end = batch_start + (unsigned int)num_threads;
        if (batch_end > num_windows) batch_end = num_windows;

        for (unsigned int w = batch_start; w < batch_end; w++)
            pthread_create(&threads[w], NULL, g1_msm_window_thread, &args[w]);

        for (unsigned int w = batch_start; w < batch_end; w++)
            pthread_join(threads[w], NULL);
    }

    /* Combine windows: result = sum(window[w] * 2^(w*c)) */
    g1_identity(result);
    for (int w = (int)num_windows - 1; w >= 0; w--) {
        for (unsigned int d = 0; d < c; d++)
            g1_double(result, result);
        g1_add(result, result, &args[w].result);
    }

    free(raw_scalars);
    free(args);
    free(threads);
}

/* ── Parallel G2 MSM ────────────────────────────────────────────── */

struct g2_msm_window_args {
    const struct g2_point *points;
    const uint64_t (*raw_scalars)[4];
    size_t n;
    unsigned int c;
    int window_index;
    struct g2_point result;
};

static void *g2_msm_window_thread(void *arg)
{
    struct g2_msm_window_args *a = (struct g2_msm_window_args *)arg;
    int w = a->window_index;
    unsigned int c = a->c;
    size_t n = a->n;
    size_t num_buckets = ((size_t)1 << c) - 1;

    struct g2_point *buckets = zcl_calloc(num_buckets, sizeof(struct g2_point), "g2_par_buckets");
    if (!buckets) { g2_identity(&a->result); return NULL; }

    for (size_t b = 0; b < num_buckets; b++)
        g2_identity(&buckets[b]);

    unsigned int bit_offset = (unsigned int)w * c;
    for (size_t i = 0; i < n; i++) {
        unsigned int word = bit_offset / 64;
        unsigned int bit = bit_offset % 64;
        if (word >= 4) continue;
        uint64_t val = a->raw_scalars[i][word] >> bit;
        if (bit + c > 64 && word + 1 < 4)
            val |= a->raw_scalars[i][word + 1] << (64 - bit);
        val &= ((uint64_t)1 << c) - 1;
        if (val == 0) continue;
        g2_add(&buckets[val - 1], &buckets[val - 1], &a->points[i]);
    }

    struct g2_point running_sum, window_sum;
    g2_identity(&running_sum);
    g2_identity(&window_sum);
    for (size_t b = num_buckets; b > 0; b--) {
        g2_add(&running_sum, &running_sum, &buckets[b - 1]);
        g2_add(&window_sum, &window_sum, &running_sum);
    }

    a->result = window_sum;
    free(buckets);
    return NULL;
}

void g2_msm_parallel(struct g2_point *result,
                     const struct g2_point *points, const struct fr *scalars,
                     size_t n, int num_threads)
{
    if (n == 0) { g2_identity(result); return; }
    if (num_threads <= 1 || n < 64) {
        g2_msm(result, points, scalars, n);
        return;
    }

    unsigned int c = 1;
    { size_t v = n; while (v > 1) { c++; v >>= 1; } }
    if (c > 16) c = 16;
    if (c < 4 && n > 16) c = 4;

    unsigned int num_windows = (255 + c - 1) / c;

    uint64_t (*raw_scalars)[4] = zcl_calloc(n, sizeof(uint64_t[4]), "g2_par_scalars");
    if (!raw_scalars) { g2_identity(result); return; }
    for (size_t i = 0; i < n; i++)
        msm_fr_to_raw(raw_scalars[i], &scalars[i]);

    struct g2_msm_window_args *args = zcl_calloc(num_windows,
                                              sizeof(struct g2_msm_window_args), "g2_par_args");
    pthread_t *threads = zcl_calloc(num_windows, sizeof(pthread_t), "g2_par_threads");
    if (!args || !threads) {
        free(raw_scalars); free(args); free(threads);
        g2_identity(result);
        return;
    }

    for (unsigned int w = 0; w < num_windows; w++) {
        args[w].points = points;
        args[w].raw_scalars = raw_scalars;
        args[w].n = n;
        args[w].c = c;
        args[w].window_index = (int)w;
    }

    for (unsigned int batch_start = 0; batch_start < num_windows;
         batch_start += (unsigned int)num_threads) {
        unsigned int batch_end = batch_start + (unsigned int)num_threads;
        if (batch_end > num_windows) batch_end = num_windows;

        for (unsigned int w = batch_start; w < batch_end; w++)
            pthread_create(&threads[w], NULL, g2_msm_window_thread, &args[w]);

        for (unsigned int w = batch_start; w < batch_end; w++)
            pthread_join(threads[w], NULL);
    }

    g2_identity(result);
    for (int w = (int)num_windows - 1; w >= 0; w--) {
        for (unsigned int d = 0; d < c; d++)
            g2_double(result, result);
        g2_add(result, result, &args[w].result);
    }

    free(raw_scalars);
    free(args);
    free(threads);
}

/* ── Parallel FFT ───────────────────────────────────────────────── */

struct fft_butterfly_args {
    struct fr *coeffs;
    size_t n;
    size_t start_k;    /* first group index for this thread */
    size_t end_k;      /* last group index (exclusive) */
    size_t m;           /* group size (2^stage) */
    struct fr *omega_m; /* twiddle factor for this stage */
};

static void *fft_butterfly_thread(void *arg)
{
    struct fft_butterfly_args *a = (struct fft_butterfly_args *)arg;
    size_t half = a->m >> 1;
    struct fr omega_m = *a->omega_m;

    for (size_t k = a->start_k; k < a->end_k; k += a->m) {
        struct fr w;
        /* Compute w = omega_m^(k/m * half) — but actually we start from 1
         * and multiply by omega_m each iteration within the group. */
        fr_one(&w);

        /* Recompute twiddle: w = omega_m^0 at start of each group */
        for (size_t j = 0; j < half; j++) {
            struct fr t, u;
            fr_mul(&t, &w, &a->coeffs[k + j + half]);
            u = a->coeffs[k + j];
            fr_add(&a->coeffs[k + j], &u, &t);
            fr_sub(&a->coeffs[k + j + half], &u, &t);
            fr_mul(&w, &w, &omega_m);
        }
    }
    return NULL;
}

/* Bit-reverse permutation */
static void par_bit_reverse(struct fr *arr, size_t n, unsigned int log_n)
{
    for (size_t i = 0; i < n; i++) {
        size_t j = 0;
        for (unsigned int b = 0; b < log_n; b++)
            j |= ((i >> b) & 1) << (log_n - 1 - b);
        if (j > i) {
            struct fr tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }
    }
}

static unsigned int par_log2_ceil(size_t n)
{
    unsigned int r = 0;
    n--;
    while (n > 0) { r++; n >>= 1; }
    return r;
}

/* Forward declaration from groth16_prover.c */
extern bool fr_fft(struct fr *coeffs, size_t n, bool inverse);

bool fr_fft_parallel(struct fr *coeffs, size_t n, bool inverse, int num_threads)
{
    if (n <= 1) return true;
    if (num_threads <= 1 || n < 256)
        return fr_fft(coeffs, n, inverse);

    unsigned int log_n = par_log2_ceil(n);
    if ((size_t)1 << log_n != n) return false;

    par_bit_reverse(coeffs, n, log_n);

    /* Compute omega (root of unity) */
    /* 2^32-th root of unity for BLS12-381 Fr */
    static const uint8_t ROOT_BYTES[32] = {
        0x59, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xec, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    struct fr omega;
    fr_from_bytes(&omega, ROOT_BYTES);

    /* Raise to 2^(32-log_n) to get n-th root of unity */
    for (unsigned int i = log_n; i < 32; i++)
        fr_sq(&omega, &omega);

    if (inverse)
        fr_inv(&omega, &omega);

    for (unsigned int s = 1; s <= log_n; s++) {
        size_t m = (size_t)1 << s;
        size_t num_groups = n / m;

        struct fr omega_m;
        /* omega_m = omega^(n/m) */
        struct fr base = omega;
        uint64_t exp = n / m;
        fr_one(&omega_m);
        while (exp > 0) {
            if (exp & 1) fr_mul(&omega_m, &omega_m, &base);
            fr_sq(&base, &base);
            exp >>= 1;
        }

        if (num_groups < (size_t)num_threads || m <= 4) {
            /* Not enough parallelism, run serial */
            for (size_t k = 0; k < n; k += m) {
                struct fr w;
                fr_one(&w);
                size_t half = m >> 1;
                for (size_t j = 0; j < half; j++) {
                    struct fr t, u;
                    fr_mul(&t, &w, &coeffs[k + j + half]);
                    u = coeffs[k + j];
                    fr_add(&coeffs[k + j], &u, &t);
                    fr_sub(&coeffs[k + j + half], &u, &t);
                    fr_mul(&w, &w, &omega_m);
                }
            }
        } else {
            /* Parallel: split groups across threads */
            int actual_threads = num_threads;
            if ((size_t)actual_threads > num_groups)
                actual_threads = (int)num_groups;

            struct fft_butterfly_args *args = zcl_calloc((size_t)actual_threads,
                sizeof(struct fft_butterfly_args), "fft_par_args");
            pthread_t *tids = zcl_calloc((size_t)actual_threads, sizeof(pthread_t), "fft_par_threads");

            size_t groups_per_thread = num_groups / (size_t)actual_threads;
            size_t remainder = num_groups % (size_t)actual_threads;

            size_t k_offset = 0;
            for (int t = 0; t < actual_threads; t++) {
                size_t my_groups = groups_per_thread + (t < (int)remainder ? 1 : 0);
                args[t].coeffs = coeffs;
                args[t].n = n;
                args[t].start_k = k_offset;
                args[t].end_k = k_offset + my_groups * m;
                args[t].m = m;
                args[t].omega_m = &omega_m;
                k_offset = args[t].end_k;

                pthread_create(&tids[t], NULL, fft_butterfly_thread, &args[t]);
            }

            for (int t = 0; t < actual_threads; t++)
                pthread_join(tids[t], NULL);

            free(args);
            free(tids);
        }
    }

    if (inverse) {
        /* Multiply all coefficients by n^{-1} */
        struct fr n_fr, n_inv, one_val;
        fr_one(&one_val);
        fr_zero(&n_fr);
        for (size_t i = 0; i < n; i++)
            fr_add(&n_fr, &n_fr, &one_val);
        fr_inv(&n_inv, &n_fr);
        for (size_t i = 0; i < n; i++)
            fr_mul(&coeffs[i], &coeffs[i], &n_inv);
    }
    return true;
}

#pragma GCC diagnostic pop
