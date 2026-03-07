/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BLS12-381 pairing — pure C23 implementation.
 * Fp (381-bit), Fp2, Fp6, Fp12, G1, G2, optimal Ate pairing. */

#ifndef ZCL_ZCASH_BLS12_381_H
#define ZCL_ZCASH_BLS12_381_H

#include <stdint.h>
#include <stdbool.h>

/* Fp: 381-bit prime field, 6 x 64-bit limbs, Montgomery form.
 * q = 0x1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab */
struct fp {
    uint64_t d[6];
};

void fp_zero(struct fp *r);
void fp_one(struct fp *r);
bool fp_is_zero(const struct fp *a);
bool fp_eq(const struct fp *a, const struct fp *b);

void fp_add(struct fp *r, const struct fp *a, const struct fp *b);
void fp_sub(struct fp *r, const struct fp *a, const struct fp *b);
void fp_neg(struct fp *r, const struct fp *a);
void fp_mul(struct fp *r, const struct fp *a, const struct fp *b);
void fp_sq(struct fp *r, const struct fp *a);
void fp_inv(struct fp *r, const struct fp *a);
bool fp_sqrt(struct fp *r, const struct fp *a);

bool fp_from_bytes(struct fp *r, const uint8_t s[48]);
void fp_to_bytes(uint8_t s[48], const struct fp *a);

/* Fp2 = Fp[u] / (u^2 + 1) */
struct fp2 {
    struct fp c0, c1; /* c0 + c1 * u */
};

void fp2_zero(struct fp2 *r);
void fp2_one(struct fp2 *r);
bool fp2_is_zero(const struct fp2 *a);
bool fp2_eq(const struct fp2 *a, const struct fp2 *b);

void fp2_add(struct fp2 *r, const struct fp2 *a, const struct fp2 *b);
void fp2_sub(struct fp2 *r, const struct fp2 *a, const struct fp2 *b);
void fp2_neg(struct fp2 *r, const struct fp2 *a);
void fp2_mul(struct fp2 *r, const struct fp2 *a, const struct fp2 *b);
void fp2_sq(struct fp2 *r, const struct fp2 *a);
void fp2_inv(struct fp2 *r, const struct fp2 *a);
void fp2_mul_by_nonresidue(struct fp2 *r, const struct fp2 *a);

/* G1: y^2 = x^3 + 4 over Fp (Jacobian coordinates) */
struct g1_point {
    struct fp x, y, z;
};

void g1_identity(struct g1_point *p);
bool g1_is_identity(const struct g1_point *p);
void g1_neg(struct g1_point *r, const struct g1_point *p);
void g1_add(struct g1_point *r, const struct g1_point *a, const struct g1_point *b);
void g1_double(struct g1_point *r, const struct g1_point *a);
bool g1_from_compressed(struct g1_point *p, const uint8_t in[48]);

/* G2: y^2 = x^3 + 4(u+1) over Fp2 (Jacobian coordinates) */
struct g2_point {
    struct fp2 x, y, z;
};

void g2_identity(struct g2_point *p);
bool g2_is_identity(const struct g2_point *p);
void g2_neg(struct g2_point *r, const struct g2_point *p);
void g2_add(struct g2_point *r, const struct g2_point *a, const struct g2_point *b);
void g2_double(struct g2_point *r, const struct g2_point *a);
bool g2_from_compressed(struct g2_point *p, const uint8_t in[96]);

/* Fp12 for pairing result (tower: Fp2 → Fp6 → Fp12) */
struct fp6 {
    struct fp2 c0, c1, c2; /* c0 + c1*v + c2*v^2 */
};

struct fp12 {
    struct fp6 c0, c1; /* c0 + c1*w */
};

/* Optimal Ate pairing */
void bls12_381_pairing(struct fp12 *result,
                        const struct g1_point *p,
                        const struct g2_point *q);

/* Multi-pairing for Groth16 verification (more efficient) */
bool bls12_381_multi_pairing_check(
    const struct g1_point *a1, const struct g2_point *b1,
    const struct g1_point *a2, const struct g2_point *b2,
    const struct g1_point *a3, const struct g2_point *b3);

/* Groth16 proof verification */
struct groth16_proof {
    struct g1_point a;
    struct g2_point b;
    struct g1_point c;
};

bool groth16_proof_read(struct groth16_proof *proof, const uint8_t data[192]);

#endif
