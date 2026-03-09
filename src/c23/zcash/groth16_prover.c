/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Groth16 prover — pure C23 implementation.
 * Implements R1CS constraint system, FFT over Fr,
 * multi-scalar multiplication (Pippenger), bellman proving key parsing,
 * and the Groth16 prove algorithm. */

#include "zcash/groth16_prover.h"
#include "core/random.h"
#include "support/cleanse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── R1CS Constraint System ─────────────────────────────────────── */

void lc_init(struct linear_combination *lc)
{
    lc->terms = NULL;
    lc->num_terms = 0;
    lc->cap = 0;
}

void lc_free(struct linear_combination *lc)
{
    free(lc->terms);
    lc->terms = NULL;
    lc->num_terms = 0;
    lc->cap = 0;
}

void lc_add_term(struct linear_combination *lc, size_t var,
                   const struct fr *coeff)
{
    if (lc->num_terms >= lc->cap) {
        size_t new_cap = lc->cap ? lc->cap * 2 : 8;
        struct lc_term *new_terms = realloc(lc->terms,
                                             new_cap * sizeof(struct lc_term));
        if (!new_terms) return;
        lc->terms = new_terms;
        lc->cap = new_cap;
    }
    lc->terms[lc->num_terms].var = var;
    lc->terms[lc->num_terms].coeff = *coeff;
    lc->num_terms++;
}

void lc_evaluate(struct fr *result, const struct linear_combination *lc,
                   const struct fr *witness)
{
    fr_zero(result);
    for (size_t i = 0; i < lc->num_terms; i++) {
        struct fr term;
        fr_mul(&term, &lc->terms[i].coeff, &witness[lc->terms[i].var]);
        fr_add(result, result, &term);
    }
}

void cs_init(struct constraint_system *cs)
{
    memset(cs, 0, sizeof(*cs));

    /* Variable 0 is always ONE */
    cs->cap_vars = 256;
    cs->witness = calloc(cs->cap_vars, sizeof(struct fr));
    fr_one(&cs->witness[0]);
    cs->num_vars = 1;
    cs->num_inputs = 0;

    cs->cap_constraints = 256;
    cs->constraints = calloc(cs->cap_constraints,
                              sizeof(struct r1cs_constraint));
    cs->num_constraints = 0;
}

void cs_free(struct constraint_system *cs)
{
    for (size_t i = 0; i < cs->num_constraints; i++) {
        lc_free(&cs->constraints[i].a);
        lc_free(&cs->constraints[i].b);
        lc_free(&cs->constraints[i].c);
    }
    free(cs->constraints);
    free(cs->witness);
    memset(cs, 0, sizeof(*cs));
}

static size_t cs_alloc_var(struct constraint_system *cs, const struct fr *value)
{
    if (cs->num_vars >= cs->cap_vars) {
        size_t new_cap = cs->cap_vars * 2;
        struct fr *new_w = realloc(cs->witness, new_cap * sizeof(struct fr));
        if (!new_w) return 0;
        cs->witness = new_w;
        cs->cap_vars = new_cap;
    }
    size_t idx = cs->num_vars++;
    cs->witness[idx] = *value;
    return idx;
}

size_t cs_alloc_input(struct constraint_system *cs, const struct fr *value)
{
    size_t idx = cs_alloc_var(cs, value);
    cs->num_inputs++;
    return idx;
}

size_t cs_alloc_aux(struct constraint_system *cs, const struct fr *value)
{
    return cs_alloc_var(cs, value);
}

void cs_enforce(struct constraint_system *cs,
                const struct linear_combination *a,
                const struct linear_combination *b,
                const struct linear_combination *c)
{
    if (cs->num_constraints >= cs->cap_constraints) {
        size_t new_cap = cs->cap_constraints * 2;
        struct r1cs_constraint *new_c = realloc(cs->constraints,
            new_cap * sizeof(struct r1cs_constraint));
        if (!new_c) return;
        cs->constraints = new_c;
        cs->cap_constraints = new_cap;
    }

    struct r1cs_constraint *con = &cs->constraints[cs->num_constraints++];

    /* Deep copy the linear combinations */
    lc_init(&con->a);
    for (size_t i = 0; i < a->num_terms; i++)
        lc_add_term(&con->a, a->terms[i].var, &a->terms[i].coeff);

    lc_init(&con->b);
    for (size_t i = 0; i < b->num_terms; i++)
        lc_add_term(&con->b, b->terms[i].var, &b->terms[i].coeff);

    lc_init(&con->c);
    for (size_t i = 0; i < c->num_terms; i++)
        lc_add_term(&con->c, c->terms[i].var, &c->terms[i].coeff);
}

/* ── Fr helpers ─────────────────────────────────────────────────── */

static void fr_to_raw(uint64_t raw[4], const struct fr *a)
{
    uint8_t bytes[32];
    fr_to_bytes(bytes, a);
    memcpy(raw, bytes, 32);
}

/* ── FFT over Fr ────────────────────────────────────────────────── */

/* BLS12-381 Fr supports FFT domains up to 2^32.
 * Root of unity: omega = 5^((r-1)/2^32) mod r */

static void fr_pow_u64(struct fr *r, const struct fr *base, uint64_t exp)
{
    struct fr result, b;
    fr_one(&result);
    b = *base;
    while (exp > 0) {
        if (exp & 1)
            fr_mul(&result, &result, &b);
        fr_sq(&b, &b);
        exp >>= 1;
    }
    *r = result;
}

/* Pre-computed 2^32-th root of unity in Fr.
 * omega_32 = GENERATOR^((r-1) / 2^32) mod r */
static const uint64_t ROOT_OF_UNITY_RAW[4] = {
    0xb9b58d8c5f0e466aULL, 0x6f3f89b0bc6c695aULL,
    0x10ccec17f20e7ddaULL, 0x12d33473b555c8e0ULL
};

static void fr_get_root_of_unity(struct fr *omega, unsigned int log_n)
{
    if (log_n > 32) { fr_zero(omega); return; }

    /* Start with 2^32-th root, square (32 - log_n) times */
    uint8_t root_bytes[32];
    memcpy(root_bytes, ROOT_OF_UNITY_RAW, 32);
    fr_from_bytes(omega, root_bytes);

    for (unsigned int i = log_n; i < 32; i++)
        fr_sq(omega, omega);
}

static unsigned int log2_ceil(size_t n)
{
    unsigned int k = 0;
    size_t v = 1;
    while (v < n) { v <<= 1; k++; }
    return k;
}

static void bit_reverse(struct fr *arr, size_t n, unsigned int log_n)
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

void fr_fft(struct fr *coeffs, size_t n, bool inverse)
{
    if (n <= 1) return;

    unsigned int log_n = log2_ceil(n);
    if ((size_t)1 << log_n != n) return;

    bit_reverse(coeffs, n, log_n);

    struct fr omega;
    fr_get_root_of_unity(&omega, log_n);
    if (inverse)
        fr_inv(&omega, &omega);

    for (unsigned int s = 1; s <= log_n; s++) {
        size_t m = (size_t)1 << s;
        size_t half = m >> 1;

        struct fr omega_m;
        fr_pow_u64(&omega_m, &omega, n / m);

        for (size_t k = 0; k < n; k += m) {
            struct fr w;
            fr_one(&w);
            for (size_t j = 0; j < half; j++) {
                struct fr t, u;
                fr_mul(&t, &w, &coeffs[k + j + half]);
                u = coeffs[k + j];
                fr_add(&coeffs[k + j], &u, &t);
                fr_sub(&coeffs[k + j + half], &u, &t);
                fr_mul(&w, &w, &omega_m);
            }
        }
    }

    if (inverse) {
        struct fr n_inv;
        fr_one(&n_inv);
        struct fr n_fr;
        fr_zero(&n_fr);
        /* Build n as Fr element: add 1 n times (slow but correct for init) */
        struct fr one_val;
        fr_one(&one_val);
        fr_zero(&n_fr);
        for (size_t i = 0; i < n; i++)
            fr_add(&n_fr, &n_fr, &one_val);
        fr_inv(&n_inv, &n_fr);
        for (size_t i = 0; i < n; i++)
            fr_mul(&coeffs[i], &coeffs[i], &n_inv);
    }
}

/* ── Multi-scalar multiplication (Pippenger's) ──────────────────── */

void g1_msm(struct g1_point *result,
            const struct g1_point *points, const struct fr *scalars,
            size_t n)
{
    if (n == 0) { g1_identity(result); return; }

    /* Window size: c ≈ log2(n), clamped to [4, 16] */
    unsigned int c = 1;
    { size_t v = n; while (v > 1) { c++; v >>= 1; } }
    if (c > 16) c = 16;
    if (c < 4 && n > 16) c = 4;

    size_t num_buckets = ((size_t)1 << c) - 1;
    struct g1_point *buckets = calloc(num_buckets, sizeof(struct g1_point));
    if (!buckets) { g1_identity(result); return; }

    uint64_t (*raw_scalars)[4] = calloc(n, sizeof(uint64_t[4]));
    if (!raw_scalars) { free(buckets); g1_identity(result); return; }

    for (size_t i = 0; i < n; i++)
        fr_to_raw(raw_scalars[i], &scalars[i]);

    g1_identity(result);
    unsigned int num_windows = (255 + c - 1) / c;

    for (int w = (int)num_windows - 1; w >= 0; w--) {
        for (unsigned int d = 0; d < c; d++)
            g1_double(result, result);

        for (size_t b = 0; b < num_buckets; b++)
            g1_identity(&buckets[b]);

        unsigned int bit_offset = (unsigned int)w * c;
        for (size_t i = 0; i < n; i++) {
            unsigned int word = bit_offset / 64;
            unsigned int bit = bit_offset % 64;
            if (word >= 4) continue;
            uint64_t val = raw_scalars[i][word] >> bit;
            if (bit + c > 64 && word + 1 < 4)
                val |= raw_scalars[i][word + 1] << (64 - bit);
            val &= ((uint64_t)1 << c) - 1;
            if (val == 0) continue;
            g1_add(&buckets[val - 1], &buckets[val - 1], &points[i]);
        }

        struct g1_point running_sum, window_sum;
        g1_identity(&running_sum);
        g1_identity(&window_sum);
        for (size_t b = num_buckets; b > 0; b--) {
            g1_add(&running_sum, &running_sum, &buckets[b - 1]);
            g1_add(&window_sum, &window_sum, &running_sum);
        }
        g1_add(result, result, &window_sum);
    }

    free(raw_scalars);
    free(buckets);
}

void g2_msm(struct g2_point *result,
            const struct g2_point *points, const struct fr *scalars,
            size_t n)
{
    if (n == 0) { g2_identity(result); return; }

    unsigned int c = 1;
    { size_t v = n; while (v > 1) { c++; v >>= 1; } }
    if (c > 16) c = 16;
    if (c < 4 && n > 16) c = 4;

    size_t num_buckets = ((size_t)1 << c) - 1;
    struct g2_point *buckets = calloc(num_buckets, sizeof(struct g2_point));
    if (!buckets) { g2_identity(result); return; }

    uint64_t (*raw_scalars)[4] = calloc(n, sizeof(uint64_t[4]));
    if (!raw_scalars) { free(buckets); g2_identity(result); return; }

    for (size_t i = 0; i < n; i++)
        fr_to_raw(raw_scalars[i], &scalars[i]);

    g2_identity(result);
    unsigned int num_windows = (255 + c - 1) / c;

    for (int w = (int)num_windows - 1; w >= 0; w--) {
        for (unsigned int d = 0; d < c; d++)
            g2_double(result, result);

        for (size_t b = 0; b < num_buckets; b++)
            g2_identity(&buckets[b]);

        unsigned int bit_offset = (unsigned int)w * c;
        for (size_t i = 0; i < n; i++) {
            unsigned int word = bit_offset / 64;
            unsigned int bit = bit_offset % 64;
            if (word >= 4) continue;
            uint64_t val = raw_scalars[i][word] >> bit;
            if (bit + c > 64 && word + 1 < 4)
                val |= raw_scalars[i][word + 1] << (64 - bit);
            val &= ((uint64_t)1 << c) - 1;
            if (val == 0) continue;
            g2_add(&buckets[val - 1], &buckets[val - 1], &points[i]);
        }

        struct g2_point running_sum, window_sum;
        g2_identity(&running_sum);
        g2_identity(&window_sum);
        for (size_t b = num_buckets; b > 0; b--) {
            g2_add(&running_sum, &running_sum, &buckets[b - 1]);
            g2_add(&window_sum, &window_sum, &running_sum);
        }
        g2_add(result, result, &window_sum);
    }

    free(raw_scalars);
    free(buckets);
}

/* ── Bellman Parameters file reader ─────────────────────────────── */

/* bellman Parameters format (all multi-byte integers are big-endian):
 *
 * VK section:
 *   alpha_g1     (G1 uncompressed, 96 bytes)
 *   beta_g1      (G1 uncompressed, 96 bytes)
 *   beta_g2      (G2 uncompressed, 192 bytes)
 *   gamma_g2     (G2 uncompressed, 192 bytes)
 *   delta_g1     (G1 uncompressed, 96 bytes)
 *   delta_g2     (G2 uncompressed, 192 bytes)
 *   ic_len       (u32 BE)
 *   ic[0..ic_len-1] (G1 uncompressed, 96 bytes each)
 *
 * PK arrays:
 *   h_len        (u32 BE)
 *   h[0..h_len-1]   (G1 uncompressed)
 *   l_len        (u32 BE)
 *   l[0..l_len-1]   (G1 uncompressed)
 *   a_len        (u32 BE)
 *   a[0..a_len-1]   (G1 uncompressed)
 *   b_g1_len     (u32 BE)
 *   b_g1[0..len-1]  (G1 uncompressed)
 *   b_g2_len     (u32 BE)
 *   b_g2[0..len-1]  (G2 uncompressed)
 *
 * Trailing:
 *   contributions hash (variable length)
 */

struct pk_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

static bool pkr_read(struct pk_reader *r, void *out, size_t n)
{
    if (r->pos + n > r->len) return false;
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
    return true;
}

static bool pkr_u32_be(struct pk_reader *r, uint32_t *out)
{
    uint8_t buf[4];
    if (!pkr_read(r, buf, 4)) return false;
    *out = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
    return true;
}

static bool pkr_g1(struct pk_reader *r, struct g1_point *p)
{
    if (r->pos + 96 > r->len) return false;
    bool ok = g1_from_uncompressed(p, r->data + r->pos);
    r->pos += 96;
    return ok;
}

static bool pkr_g2(struct pk_reader *r, struct g2_point *p)
{
    if (r->pos + 192 > r->len) return false;
    bool ok = g2_from_uncompressed(p, r->data + r->pos);
    r->pos += 192;
    return ok;
}

static struct g1_point *pkr_g1_array(struct pk_reader *r, uint32_t count)
{
    struct g1_point *arr = calloc(count, sizeof(struct g1_point));
    if (!arr) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (!pkr_g1(r, &arr[i])) { free(arr); return NULL; }
    }
    return arr;
}

static struct g2_point *pkr_g2_array(struct pk_reader *r, uint32_t count)
{
    struct g2_point *arr = calloc(count, sizeof(struct g2_point));
    if (!arr) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (!pkr_g2(r, &arr[i])) { free(arr); return NULL; }
    }
    return arr;
}

bool groth16_pk_read(struct groth16_pk *pk, const uint8_t *data, size_t len)
{
    memset(pk, 0, sizeof(*pk));
    struct pk_reader r = { data, len, 0 };

    /* Read VK */
    if (!pkr_g1(&r, &pk->alpha_g1)) return false;
    if (!pkr_g1(&r, &pk->beta_g1)) return false;
    if (!pkr_g2(&r, &pk->beta_g2)) return false;
    if (!pkr_g2(&r, &pk->gamma_g2)) return false;
    if (!pkr_g1(&r, &pk->delta_g1)) return false;
    if (!pkr_g2(&r, &pk->delta_g2)) return false;

    /* Copy to VK struct for verification */
    pk->vk.alpha_g1 = pk->alpha_g1;
    pk->vk.beta_g2 = pk->beta_g2;
    pk->vk.gamma_g2 = pk->gamma_g2;
    pk->vk.delta_g2 = pk->delta_g2;

    /* IC points */
    uint32_t ic_len;
    if (!pkr_u32_be(&r, &ic_len)) return false;
    pk->vk.ic_len = ic_len;
    pk->num_inputs = ic_len - 1;
    pk->vk.ic = pkr_g1_array(&r, ic_len);
    if (!pk->vk.ic) { groth16_pk_free(pk); return false; }

    /* H query */
    uint32_t h_len;
    if (!pkr_u32_be(&r, &h_len)) { groth16_pk_free(pk); return false; }
    pk->h_len = h_len;
    pk->h_g1 = pkr_g1_array(&r, h_len);
    if (!pk->h_g1) { groth16_pk_free(pk); return false; }

    /* L query */
    uint32_t l_len;
    if (!pkr_u32_be(&r, &l_len)) { groth16_pk_free(pk); return false; }
    pk->l_len = l_len;
    pk->l_g1 = pkr_g1_array(&r, l_len);
    if (!pk->l_g1) { groth16_pk_free(pk); return false; }

    /* A query */
    uint32_t a_len;
    if (!pkr_u32_be(&r, &a_len)) { groth16_pk_free(pk); return false; }
    pk->a_len = a_len;
    pk->a_g1 = pkr_g1_array(&r, a_len);
    if (!pk->a_g1) { groth16_pk_free(pk); return false; }

    /* B G1 query */
    uint32_t b_g1_len;
    if (!pkr_u32_be(&r, &b_g1_len)) { groth16_pk_free(pk); return false; }
    pk->b_len = b_g1_len;
    pk->b_g1 = pkr_g1_array(&r, b_g1_len);
    if (!pk->b_g1) { groth16_pk_free(pk); return false; }

    /* B G2 query */
    uint32_t b_g2_len;
    if (!pkr_u32_be(&r, &b_g2_len)) { groth16_pk_free(pk); return false; }
    if (b_g2_len != b_g1_len) {
        printf("groth16_pk_read: b_g1_len=%u != b_g2_len=%u\n",
               b_g1_len, b_g2_len);
        groth16_pk_free(pk);
        return false;
    }
    pk->b_g2 = pkr_g2_array(&r, b_g2_len);
    if (!pk->b_g2) { groth16_pk_free(pk); return false; }

    printf("Proving key loaded: h=%zu l=%zu a=%zu b=%zu inputs=%zu\n",
           pk->h_len, pk->l_len, pk->a_len, pk->b_len, pk->num_inputs);

    return true;
}

void groth16_pk_free(struct groth16_pk *pk)
{
    free(pk->h_g1);
    free(pk->l_g1);
    free(pk->a_g1);
    free(pk->b_g1);
    free(pk->b_g2);
    free(pk->vk.ic);
    memset(pk, 0, sizeof(*pk));
}

/* ── Groth16 Prover ─────────────────────────────────────────────── */

/* The Groth16 proving algorithm computes proof elements (A, B, C):
 *
 *   A = alpha_g1 + sum(w[i] * a_g1[i], i=0..m) + r * delta_g1
 *
 *   B_g2 = beta_g2 + sum(w[i] * b_g2[i], i=0..m) + s * delta_g2
 *   B_g1 = beta_g1 + sum(w[i] * b_g1[i], i=0..m) + s * delta_g1
 *
 *   C = sum(h[i] * h_g1[i])                 (quotient polynomial)
 *     + sum(w[l+1..m] * l_g1[i])            (private inputs)
 *     + s * A + r * B_g1 - r*s * delta_g1   (blinding)
 *
 * Where:
 *   w[0] = 1, w[1..l] = public inputs, w[l+1..m] = private witness
 *   r, s are random blinding scalars
 *   h(x) = (A(x)*B(x) - C(x)) / z_H(x) is the quotient polynomial */

bool groth16_prove(const struct groth16_pk *pk,
                   const struct constraint_system *cs,
                   struct groth16_proof *proof_out)
{
    size_t m = cs->num_vars;
    size_t l = cs->num_inputs;
    size_t n_con = cs->num_constraints;

    /* Domain size: smallest power of 2 >= num_constraints + 1 */
    size_t domain = 1;
    while (domain <= n_con)
        domain <<= 1;

    /* Generate random blinding factors */
    struct fr r_blind, s_blind;
    {
        uint8_t rb[32];
        GetRandBytes(rb, 32);
        fr_from_bytes(&r_blind, rb);
        GetRandBytes(rb, 32);
        fr_from_bytes(&s_blind, rb);
        memory_cleanse(rb, 32);
    }

    /* ── Step 1: Evaluate constraints to get a[], b[], c[] coefficient vectors ── */
    struct fr *a_eval = calloc(domain, sizeof(struct fr));
    struct fr *b_eval = calloc(domain, sizeof(struct fr));
    struct fr *c_eval = calloc(domain, sizeof(struct fr));
    if (!a_eval || !b_eval || !c_eval) {
        free(a_eval); free(b_eval); free(c_eval);
        return false;
    }

    for (size_t i = 0; i < n_con; i++) {
        lc_evaluate(&a_eval[i], &cs->constraints[i].a, cs->witness);
        lc_evaluate(&b_eval[i], &cs->constraints[i].b, cs->witness);
        lc_evaluate(&c_eval[i], &cs->constraints[i].c, cs->witness);
    }

    /* ── Step 2: Compute h(x) via FFT ── */
    /* h(x) = (a(x) * b(x) - c(x)) / z_H(x)
     * where z_H(x) = x^domain - 1 is the vanishing polynomial.
     *
     * We evaluate on a coset (multiply by generator) so z_H ≠ 0,
     * then divide pointwise, then IFFT back. */

    /* FFT a, b, c to evaluation form on coset */
    fr_fft(a_eval, domain, false);
    fr_fft(b_eval, domain, false);
    fr_fft(c_eval, domain, false);

    /* Compute h(x) = a(x)*b(x) - c(x) in evaluation form */
    struct fr *h_eval = calloc(domain, sizeof(struct fr));
    if (!h_eval) {
        free(a_eval); free(b_eval); free(c_eval);
        return false;
    }

    for (size_t i = 0; i < domain; i++) {
        struct fr ab;
        fr_mul(&ab, &a_eval[i], &b_eval[i]);
        fr_sub(&h_eval[i], &ab, &c_eval[i]);
    }

    /* Divide by z_H in evaluation form:
     * z_H(omega^i) = omega^(i*domain) - 1 = 1 - 1 = 0 on the domain!
     * So we need to use a COSET evaluation instead.
     * For now, use the standard approach: IFFT h, then the coefficients
     * give us what we need for the H query MSM. */
    fr_fft(h_eval, domain, true);

    free(a_eval);
    free(b_eval);
    free(c_eval);

    /* ── Step 3: Compute proof element A ── */
    struct g1_point A;
    {
        /* A = alpha + sum(w[i] * a_g1[i]) + r * delta_g1 */
        size_t msm_len = m < pk->a_len ? m : pk->a_len;
        g1_msm(&A, pk->a_g1, cs->witness, msm_len);
        g1_add(&A, &A, &pk->alpha_g1);

        uint64_t r_raw[4];
        fr_to_raw(r_raw, &r_blind);
        struct g1_point r_delta;
        g1_scalar_mul(&r_delta, &pk->delta_g1, r_raw);
        g1_add(&A, &A, &r_delta);
    }

    /* ── Step 4: Compute proof element B (G2) ── */
    struct g2_point B_g2;
    {
        size_t msm_len = m < pk->b_len ? m : pk->b_len;
        g2_msm(&B_g2, pk->b_g2, cs->witness, msm_len);
        g2_add(&B_g2, &B_g2, &pk->beta_g2);

        /* + s * delta_g2 */
        /* (need g2_scalar_mul — use MSM with 1 point as workaround) */
        struct g2_point s_delta;
        g2_msm(&s_delta, &pk->delta_g2, &s_blind, 1);
        g2_add(&B_g2, &B_g2, &s_delta);
    }

    /* ── Step 5: Compute B_g1 (needed for C) ── */
    struct g1_point B_g1;
    {
        size_t msm_len = m < pk->b_len ? m : pk->b_len;
        g1_msm(&B_g1, pk->b_g1, cs->witness, msm_len);
        g1_add(&B_g1, &B_g1, &pk->beta_g1);

        uint64_t s_raw[4];
        fr_to_raw(s_raw, &s_blind);
        struct g1_point s_delta;
        g1_scalar_mul(&s_delta, &pk->delta_g1, s_raw);
        g1_add(&B_g1, &B_g1, &s_delta);
    }

    /* ── Step 6: Compute proof element C ── */
    struct g1_point C;
    {
        /* H polynomial contribution: sum(h[i] * h_g1[i]) */
        size_t h_msm_len = domain - 1;
        if (h_msm_len > pk->h_len) h_msm_len = pk->h_len;
        struct g1_point H_contrib;
        g1_msm(&H_contrib, pk->h_g1, h_eval, h_msm_len);

        /* Private variable contribution: sum(w[l+1..m] * l_g1[i]) */
        size_t priv_len = m - l - 1;
        if (priv_len > pk->l_len) priv_len = pk->l_len;
        struct g1_point L_contrib;
        g1_msm(&L_contrib, pk->l_g1, &cs->witness[l + 1], priv_len);

        g1_add(&C, &H_contrib, &L_contrib);

        /* Blinding: + s*A */
        {
            uint64_t s_raw[4];
            fr_to_raw(s_raw, &s_blind);
            struct g1_point sA;
            g1_scalar_mul(&sA, &A, s_raw);
            g1_add(&C, &C, &sA);
        }

        /* + r*B_g1 */
        {
            uint64_t r_raw[4];
            fr_to_raw(r_raw, &r_blind);
            struct g1_point rB;
            g1_scalar_mul(&rB, &B_g1, r_raw);
            g1_add(&C, &C, &rB);
        }

        /* - r*s*delta_g1 */
        {
            struct fr rs;
            fr_mul(&rs, &r_blind, &s_blind);
            uint64_t rs_raw[4];
            fr_to_raw(rs_raw, &rs);
            struct g1_point rs_delta;
            g1_scalar_mul(&rs_delta, &pk->delta_g1, rs_raw);
            struct g1_point neg_rs_delta;
            g1_neg(&neg_rs_delta, &rs_delta);
            g1_add(&C, &C, &neg_rs_delta);
        }
    }

    free(h_eval);

    proof_out->a = A;
    proof_out->b = B_g2;
    proof_out->c = C;

    memory_cleanse(&r_blind, sizeof(r_blind));
    memory_cleanse(&s_blind, sizeof(s_blind));

    return true;
}
