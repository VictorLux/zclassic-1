/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BLS12-381 base field Fp — pure C23 implementation.
 * 381-bit prime, 6 x 64-bit limbs, Montgomery multiplication. */

#include "zcash/bls12_381.h"
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

/* q = 0x1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab */
static const uint64_t FP_Q[6] = {
    0xb9feffffffffaaabULL, 0x1eabfffeb153ffffULL,
    0x6730d2a0f6b0f624ULL, 0x64774b84f38512bfULL,
    0x4b1ba7b6434bacd7ULL, 0x1a0111ea397fe69aULL
};

/* -q^{-1} mod 2^64 */
static const uint64_t FP_INV = 0x89f3fffcfffcfffdULL;

/* R = 2^384 mod q */
static const uint64_t FP_R[6] = {
    0x760900000002fffdULL, 0xebf4000bc40c0002ULL,
    0x5f48985753c758baULL, 0x77ce585370525745ULL,
    0x5c071a97a256ec6dULL, 0x15f65ec3fa80e493ULL
};

/* R^2 mod q */
static const uint64_t FP_R2[6] = {
    0xf4df1f341c341746ULL, 0x0a76e6a609d104f1ULL,
    0x8de5476c4c95b6d5ULL, 0x67eb88a9939d83c0ULL,
    0x9a793e85b519952dULL, 0x11988fe592cae3aaULL
};

/* --- Fp arithmetic --- */

static bool fp_gte(const uint64_t a[6], const uint64_t b[6])
{
    for (int i = 5; i >= 0; i--) {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return true;
}

static void fp_sub_noborrow(uint64_t r[6], const uint64_t a[6], const uint64_t b[6])
{
    unsigned __int128 borrow = 0;
    for (int i = 0; i < 6; i++) {
        unsigned __int128 tmp = (unsigned __int128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)tmp;
        borrow = (tmp >> 127) & 1;
    }
}

/* Montgomery multiplication: r = a * b * R^{-1} mod q */
static void fp_mont_mul(uint64_t r[6], const uint64_t a[6], const uint64_t b[6])
{
    uint64_t t[7] = {0};

    for (int i = 0; i < 6; i++) {
        unsigned __int128 carry = 0;
        for (int j = 0; j < 6; j++) {
            unsigned __int128 prod = (unsigned __int128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        t[6] = (uint64_t)carry;

        uint64_t m = t[0] * FP_INV;
        carry = 0;
        unsigned __int128 prod0 = (unsigned __int128)m * FP_Q[0] + t[0];
        carry = prod0 >> 64;

        for (int j = 1; j < 6; j++) {
            unsigned __int128 prod = (unsigned __int128)m * FP_Q[j] + t[j] + carry;
            t[j - 1] = (uint64_t)prod;
            carry = prod >> 64;
        }
        unsigned __int128 sum = (unsigned __int128)t[6] + carry;
        t[5] = (uint64_t)sum;
        t[6] = (uint64_t)(sum >> 64);
    }

    if (t[6] || fp_gte(t, FP_Q))
        fp_sub_noborrow(r, t, FP_Q);
    else
        memcpy(r, t, 48);
}

void fp_zero(struct fp *r) { memset(r->d, 0, 48); }

void fp_one(struct fp *r) { memcpy(r->d, FP_R, 48); }

bool fp_is_zero(const struct fp *a)
{
    for (int i = 0; i < 6; i++)
        if (a->d[i] != 0) return false;
    return true;
}

bool fp_eq(const struct fp *a, const struct fp *b)
{
    for (int i = 0; i < 6; i++)
        if (a->d[i] != b->d[i]) return false;
    return true;
}

void fp_add(struct fp *r, const struct fp *a, const struct fp *b)
{
    unsigned __int128 carry = 0;
    uint64_t tmp[6];
    for (int i = 0; i < 6; i++) {
        unsigned __int128 sum = (unsigned __int128)a->d[i] + b->d[i] + carry;
        tmp[i] = (uint64_t)sum;
        carry = sum >> 64;
    }
    if (carry || fp_gte(tmp, FP_Q))
        fp_sub_noborrow(r->d, tmp, FP_Q);
    else
        memcpy(r->d, tmp, 48);
}

void fp_sub(struct fp *r, const struct fp *a, const struct fp *b)
{
    if (fp_gte(a->d, b->d)) {
        fp_sub_noborrow(r->d, a->d, b->d);
    } else {
        uint64_t tmp[6];
        unsigned __int128 carry = 0;
        for (int i = 0; i < 6; i++) {
            unsigned __int128 sum = (unsigned __int128)a->d[i] + FP_Q[i] + carry;
            tmp[i] = (uint64_t)sum;
            carry = sum >> 64;
        }
        fp_sub_noborrow(r->d, tmp, b->d);
    }
}

void fp_neg(struct fp *r, const struct fp *a)
{
    if (fp_is_zero(a))
        fp_zero(r);
    else
        fp_sub_noborrow(r->d, FP_Q, a->d);
}

void fp_mul(struct fp *r, const struct fp *a, const struct fp *b)
{
    fp_mont_mul(r->d, a->d, b->d);
}

void fp_sq(struct fp *r, const struct fp *a)
{
    fp_mont_mul(r->d, a->d, a->d);
}

void fp_inv(struct fp *r, const struct fp *a)
{
    /* Fermat: a^{-1} = a^{q-2} mod q */
    struct fp result;
    fp_one(&result);
    struct fp base = *a;

    uint8_t exp[48];
    for (int i = 0; i < 48; i++)
        exp[i] = ((const uint8_t *)FP_Q)[i];
    /* Subtract 2 */
    if (exp[0] >= 2) exp[0] -= 2;
    else { exp[0] += 254; int i = 1; while (i < 48 && exp[i] == 0) { exp[i] = 0xff; i++; } if (i < 48) exp[i]--; }

    for (int i = 0; i < 384; i++) {
        if ((exp[i / 8] >> (i % 8)) & 1)
            fp_mul(&result, &result, &base);
        fp_sq(&base, &base);
    }
    *r = result;
}

bool fp_from_bytes(struct fp *r, const uint8_t s[48])
{
    /* BLS12-381 uses BIG-endian encoding */
    uint64_t raw[6];
    for (int i = 0; i < 6; i++) {
        raw[5 - i] = 0;
        for (int j = 0; j < 8; j++)
            raw[5 - i] |= (uint64_t)s[i * 8 + j] << (8 * (7 - j));
    }

    /* Check < q */
    bool ok = true;
    for (int i = 5; i >= 0; i--) {
        if (raw[i] > FP_Q[i]) { ok = false; break; }
        if (raw[i] < FP_Q[i]) break;
    }

    /* Convert to Montgomery form */
    fp_mont_mul(r->d, raw, FP_R2);
    return ok;
}

void fp_to_bytes(uint8_t s[48], const struct fp *a)
{
    /* Convert from Montgomery form */
    uint64_t one[6] = {1, 0, 0, 0, 0, 0};
    uint64_t raw[6];
    fp_mont_mul(raw, a->d, one);

    /* BIG-endian encoding */
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 8; j++)
            s[i * 8 + j] = (uint8_t)(raw[5 - i] >> (8 * (7 - j)));
}

/* --- Fp2 = Fp[u] / (u^2 + 1) --- */

void fp2_zero(struct fp2 *r) { fp_zero(&r->c0); fp_zero(&r->c1); }

void fp2_one(struct fp2 *r) { fp_one(&r->c0); fp_zero(&r->c1); }

bool fp2_is_zero(const struct fp2 *a) { return fp_is_zero(&a->c0) && fp_is_zero(&a->c1); }

bool fp2_eq(const struct fp2 *a, const struct fp2 *b)
{
    return fp_eq(&a->c0, &b->c0) && fp_eq(&a->c1, &b->c1);
}

void fp2_add(struct fp2 *r, const struct fp2 *a, const struct fp2 *b)
{
    fp_add(&r->c0, &a->c0, &b->c0);
    fp_add(&r->c1, &a->c1, &b->c1);
}

void fp2_sub(struct fp2 *r, const struct fp2 *a, const struct fp2 *b)
{
    fp_sub(&r->c0, &a->c0, &b->c0);
    fp_sub(&r->c1, &a->c1, &b->c1);
}

void fp2_neg(struct fp2 *r, const struct fp2 *a)
{
    fp_neg(&r->c0, &a->c0);
    fp_neg(&r->c1, &a->c1);
}

/* (a0 + a1*u) * (b0 + b1*u) = (a0*b0 - a1*b1) + (a0*b1 + a1*b0)*u */
void fp2_mul(struct fp2 *r, const struct fp2 *a, const struct fp2 *b)
{
    struct fp t0, t1, t2;
    fp_mul(&t0, &a->c0, &b->c0); /* a0*b0 */
    fp_mul(&t1, &a->c1, &b->c1); /* a1*b1 */

    /* c0 = a0*b0 - a1*b1 */
    fp_sub(&r->c0, &t0, &t1);

    /* c1 = (a0+a1)*(b0+b1) - a0*b0 - a1*b1 = a0*b1 + a1*b0 */
    fp_add(&t2, &a->c0, &a->c1);
    struct fp t3;
    fp_add(&t3, &b->c0, &b->c1);
    fp_mul(&r->c1, &t2, &t3);
    fp_sub(&r->c1, &r->c1, &t0);
    fp_sub(&r->c1, &r->c1, &t1);
}

/* (a0 + a1*u)^2 = (a0^2 - a1^2) + 2*a0*a1*u */
void fp2_sq(struct fp2 *r, const struct fp2 *a)
{
    struct fp t0, t1;
    fp_add(&t0, &a->c0, &a->c1);     /* a0 + a1 */
    fp_sub(&t1, &a->c0, &a->c1);     /* a0 - a1 */
    fp_mul(&r->c0, &t0, &t1);        /* a0^2 - a1^2 */
    fp_mul(&t0, &a->c0, &a->c1);     /* a0 * a1 */
    fp_add(&r->c1, &t0, &t0);        /* 2 * a0 * a1 */
}

void fp2_inv(struct fp2 *r, const struct fp2 *a)
{
    /* 1/(a0 + a1*u) = (a0 - a1*u) / (a0^2 + a1^2) */
    struct fp t0, t1, inv;
    fp_sq(&t0, &a->c0);
    fp_sq(&t1, &a->c1);
    fp_add(&t0, &t0, &t1); /* a0^2 + a1^2 (since u^2 = -1) */
    fp_inv(&inv, &t0);
    fp_mul(&r->c0, &a->c0, &inv);
    fp_neg(&r->c1, &a->c1);
    fp_mul(&r->c1, &r->c1, &inv);
}

/* Multiply by non-residue (u + 1) for Fp6 tower construction.
 * (a0 + a1*u) * (1 + u) = (a0 - a1) + (a0 + a1)*u */
void fp2_mul_by_nonresidue(struct fp2 *r, const struct fp2 *a)
{
    struct fp t0;
    fp_sub(&t0, &a->c0, &a->c1);
    fp_add(&r->c1, &a->c0, &a->c1);
    r->c0 = t0;
}

/* --- G1 point operations (Jacobian coordinates) --- */

void g1_identity(struct g1_point *p)
{
    fp_zero(&p->x);
    fp_one(&p->y);
    fp_zero(&p->z);
}

bool g1_is_identity(const struct g1_point *p)
{
    return fp_is_zero(&p->z);
}

/* TODO: g1_add, g1_double, g1_from_compressed, g2 operations, pairing */
/* These will be implemented incrementally. */

#pragma GCC diagnostic pop
