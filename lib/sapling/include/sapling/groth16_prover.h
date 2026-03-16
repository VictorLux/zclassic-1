/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Groth16 zero-knowledge proof prover — pure C23 implementation.
 * Generates proofs for Sapling spend and output circuits. */

#ifndef ZCL_SAPLING_GROTH16_PROVER_H
#define ZCL_SAPLING_GROTH16_PROVER_H

#include "sapling/bls12_381.h"
#include "sapling/fr.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── R1CS Constraint System ─────────────────────────────────────── */

/* A linear combination: sum of (variable_index, coefficient) pairs */
struct lc_term {
    size_t var;
    struct fr coeff;
};

struct linear_combination {
    struct lc_term *terms;
    size_t num_terms;
    size_t cap;
};

/* R1CS constraint: A * B = C (each is a linear combination) */
struct r1cs_constraint {
    struct linear_combination a, b, c;
};

/* Constraint system with witness assignment */
struct constraint_system {
    struct r1cs_constraint *constraints;
    size_t num_constraints;
    size_t cap_constraints;

    struct fr *witness;       /* full variable assignment */
    size_t num_vars;          /* total variables (including ONE at index 0) */
    size_t cap_vars;
    size_t num_inputs;        /* public inputs (indices 1..num_inputs) */
};

void cs_init(struct constraint_system *cs);
void cs_free(struct constraint_system *cs);

/* Allocate a new variable, return its index */
size_t cs_alloc_input(struct constraint_system *cs, const struct fr *value);
size_t cs_alloc_aux(struct constraint_system *cs, const struct fr *value);

/* Add linear combination term */
void lc_init(struct linear_combination *lc);
void lc_free(struct linear_combination *lc);
void lc_add_term(struct linear_combination *lc, size_t var,
                   const struct fr *coeff);

/* Add constraint: A * B = C */
void cs_enforce(struct constraint_system *cs,
                const struct linear_combination *a,
                const struct linear_combination *b,
                const struct linear_combination *c);

/* Evaluate a linear combination with the current witness */
void lc_evaluate(struct fr *result, const struct linear_combination *lc,
                   const struct fr *witness);

/* ── Proving Key ────────────────────────────────────────────────── */

/* Proving key for Groth16.
 * Parsed from bellman Parameters format. */
struct groth16_pk {
    size_t num_inputs;    /* l: public inputs */
    size_t h_len;         /* H query length */
    size_t l_len;         /* L query length */
    size_t a_len;         /* A query length */
    size_t b_len;         /* B query length (same for G1 and G2) */

    /* Points from CRS */
    struct g1_point alpha_g1;
    struct g1_point beta_g1;
    struct g1_point delta_g1;
    struct g2_point beta_g2;
    struct g2_point gamma_g2;
    struct g2_point delta_g2;

    /* CRS query arrays */
    struct g1_point *h_g1;    /* H polynomial evaluation points */
    struct g1_point *l_g1;    /* Private variable commitments */
    struct g1_point *a_g1;    /* A query */
    struct g1_point *b_g1;    /* B query (G1) */
    struct g2_point *b_g2;    /* B query (G2) */

    /* Verification key */
    struct groth16_vk vk;
};

/* Read a proving key from bellman Parameters file.
 * Format: VK | h[] | l[] | a[] | b_g1[] | b_g2[] | hash
 * All lengths are u32 big-endian. Points are uncompressed.
 * Caller must call groth16_pk_free() when done. */
bool groth16_pk_read(struct groth16_pk *pk, const uint8_t *data, size_t len);
void groth16_pk_free(struct groth16_pk *pk);

/* ── FFT ────────────────────────────────────────────────────────── */

void fr_fft(struct fr *coeffs, size_t n, bool inverse);

/* ── Multi-scalar multiplication ────────────────────────────────── */

void g1_msm(struct g1_point *result,
            const struct g1_point *points, const struct fr *scalars,
            size_t n);

void g2_msm(struct g2_point *result,
            const struct g2_point *points, const struct fr *scalars,
            size_t n);

/* ── Groth16 Prover ─────────────────────────────────────────────── */

/* Generate a Groth16 proof from a satisfied constraint system and proving key.
 *
 * The constraint system must already have its witness fully assigned.
 * The prover:
 *   1. Evaluates constraint polynomials via the witness
 *   2. Computes h(x) quotient polynomial via FFT
 *   3. Computes proof elements (A, B, C) via MSM with CRS
 *   4. Adds random blinding (r, s) for zero-knowledge */
bool groth16_prove(const struct groth16_pk *pk,
                   const struct constraint_system *cs,
                   struct groth16_proof *proof_out);

#endif
