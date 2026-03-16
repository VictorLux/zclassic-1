/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Fr/Fs field arithmetic, Jubjub, BLS12-381, PRF, FF1 tests. */

#include "test/test_helpers.h"

int test_zcash_crypto(void)
{
    int failures = 0;

    printf("Fr zero/one identity... ");
    {
        struct fr z, o;
        fr_zero(&z);
        fr_one(&o);
        bool ok = fr_is_zero(&z) && !fr_is_zero(&o);
        /* 0 + 1 = 1 */
        struct fr sum;
        fr_add(&sum, &z, &o);
        ok = ok && fr_eq(&sum, &o);
        /* 1 - 1 = 0 */
        struct fr diff;
        fr_sub(&diff, &o, &o);
        ok = ok && fr_is_zero(&diff);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fr add/sub/neg... ");
    {
        struct fr a, b, c, neg_a, sum;
        fr_one(&a);
        fr_add(&b, &a, &a); /* b = 2 */
        fr_add(&c, &b, &a); /* c = 3 */
        fr_sub(&sum, &c, &b); /* 3 - 2 = 1 */
        bool ok = fr_eq(&sum, &a);
        fr_neg(&neg_a, &a);
        fr_add(&sum, &a, &neg_a); /* 1 + (-1) = 0 */
        ok = ok && fr_is_zero(&sum);
        /* Double negation */
        struct fr neg_neg;
        fr_neg(&neg_neg, &neg_a);
        ok = ok && fr_eq(&neg_neg, &a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fr mul/sq/inv... ");
    {
        struct fr a, two, four, sq_two, inv_a, prod;
        fr_one(&a);
        fr_add(&two, &a, &a);
        fr_mul(&four, &two, &two);
        fr_sq(&sq_two, &two);
        bool ok = fr_eq(&four, &sq_two); /* 2*2 == 2^2 */
        /* inv(2) * 2 = 1 */
        fr_inv(&inv_a, &two);
        fr_mul(&prod, &inv_a, &two);
        ok = ok && fr_eq(&prod, &a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fr from_bytes/to_bytes roundtrip... ");
    {
        uint8_t bytes[32] = {0};
        bytes[0] = 42; /* small value */
        struct fr a;
        bool ok = fr_from_bytes(&a, bytes);
        uint8_t out[32];
        fr_to_bytes(out, &a);
        ok = ok && (memcmp(bytes, out, 32) == 0);
        /* Zero roundtrip */
        memset(bytes, 0, 32);
        ok = ok && fr_from_bytes(&a, bytes);
        fr_to_bytes(out, &a);
        ok = ok && (memcmp(bytes, out, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fs zero/one/add/neg... ");
    {
        struct fs z, o, sum, neg_o;
        fs_zero(&z);
        fs_one(&o);
        bool ok = fs_is_zero(&z) && !fs_is_zero(&o);
        fs_add(&sum, &o, &o); /* 1 + 1 = 2 */
        ok = ok && !fs_is_zero(&sum);
        fs_neg(&neg_o, &o);
        fs_add(&sum, &o, &neg_o); /* 1 + (-1) = 0 */
        ok = ok && fs_is_zero(&sum);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fs from_bytes/to_bytes roundtrip... ");
    {
        uint8_t bytes[32] = {0};
        bytes[0] = 7;
        struct fs a;
        bool ok = fs_from_bytes(&a, bytes);
        uint8_t out[32];
        fs_to_bytes(out, &a);
        ok = ok && (memcmp(bytes, out, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fs mul... ");
    {
        struct fs a, b, c;
        fs_one(&a);
        fs_mul(&b, &a, &a); /* 1 * 1 = 1 */
        bool ok = !fs_is_zero(&b);
        uint8_t out_a[32], out_b[32];
        fs_to_bytes(out_a, &a);
        fs_to_bytes(out_b, &b);
        ok = ok && (memcmp(out_a, out_b, 32) == 0);
        /* 0 * anything = 0 */
        struct fs z;
        fs_zero(&z);
        fs_mul(&c, &z, &a);
        ok = ok && fs_is_zero(&c);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fs to_uniform (64-byte reduction)... ");
    {
        uint8_t digest[64] = {0};
        digest[0] = 0xff;
        digest[63] = 0xff;
        struct fs r;
        fs_to_uniform(&r, digest);
        /* Should produce a non-zero scalar less than the group order */
        bool ok = !fs_is_zero(&r);
        /* Same input should produce same output */
        struct fs r2;
        fs_to_uniform(&r2, digest);
        uint8_t b1[32], b2[32];
        fs_to_bytes(b1, &r);
        fs_to_bytes(b2, &r2);
        ok = ok && (memcmp(b1, b2, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================ */
    /* Jubjub point operation tests                                     */
    /* ================================================================ */

    printf("Jubjub identity... ");
    {
        struct jub_point id;
        jub_identity(&id);
        bool ok = jub_is_identity(&id);
        /* Identity + identity = identity */
        struct jub_point sum;
        jub_add(&sum, &id, &id);
        ok = ok && jub_is_identity(&sum);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub compress/decompress roundtrip... ");
    {
        /* Generate a known point via ask_to_ak */
        uint8_t ask[32] = {1};
        uint8_t ak[32];
        sapling_ask_to_ak(ask, ak);
        /* ak is a compressed point. Decompress, recompress. */
        struct jub_point p;
        bool ok = jub_from_bytes(&p, ak);
        uint8_t recomp[32];
        jub_to_bytes(recomp, &p);
        ok = ok && (memcmp(ak, recomp, 32) == 0);
        /* Identity roundtrip */
        struct jub_point id;
        jub_identity(&id);
        uint8_t id_bytes[32];
        jub_to_bytes(id_bytes, &id);
        struct jub_point id2;
        ok = ok && jub_from_bytes(&id2, id_bytes);
        ok = ok && jub_is_identity(&id2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub add associativity... ");
    {
        /* P = ak(1), Q = ak(2), R = ak(3) */
        uint8_t s1[32] = {1}, s2[32] = {2}, s3[32] = {3};
        uint8_t b1[32], b2[32], b3[32];
        sapling_ask_to_ak(s1, b1);
        sapling_ask_to_ak(s2, b2);
        sapling_ask_to_ak(s3, b3);
        struct jub_point P, Q, R;
        jub_from_bytes(&P, b1);
        jub_from_bytes(&Q, b2);
        jub_from_bytes(&R, b3);
        /* (P+Q)+R */
        struct jub_point pq, pqr;
        jub_add(&pq, &P, &Q);
        jub_add(&pqr, &pq, &R);
        /* P+(Q+R) */
        struct jub_point qr, p_qr;
        jub_add(&qr, &Q, &R);
        jub_add(&p_qr, &P, &qr);
        uint8_t out1[32], out2[32];
        jub_to_bytes(out1, &pqr);
        jub_to_bytes(out2, &p_qr);
        bool ok = (memcmp(out1, out2, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub double = add to self... ");
    {
        uint8_t s[32] = {5};
        uint8_t b[32];
        sapling_ask_to_ak(s, b);
        struct jub_point P, dbl, add_self;
        jub_from_bytes(&P, b);
        jub_double(&dbl, &P);
        jub_add(&add_self, &P, &P);
        uint8_t out1[32], out2[32];
        jub_to_bytes(out1, &dbl);
        jub_to_bytes(out2, &add_self);
        bool ok = (memcmp(out1, out2, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub negate... ");
    {
        uint8_t s[32] = {7};
        uint8_t b[32];
        sapling_ask_to_ak(s, b);
        struct jub_point P, neg_P, sum;
        jub_from_bytes(&P, b);
        jub_neg(&neg_P, &P);
        jub_add(&sum, &P, &neg_P);
        bool ok = jub_is_identity(&sum);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub scalar mul by 1... ");
    {
        uint8_t s[32] = {3};
        uint8_t b[32];
        sapling_ask_to_ak(s, b);
        struct jub_point P, result;
        jub_from_bytes(&P, b);
        uint8_t one_scalar[32] = {1};
        jub_scalar_mul(&result, &P, one_scalar);
        uint8_t out1[32], out2[32];
        jub_to_bytes(out1, &P);
        jub_to_bytes(out2, &result);
        bool ok = (memcmp(out1, out2, 32) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub cofactor multiplication... ");
    {
        uint8_t s[32] = {11};
        uint8_t b[32];
        sapling_ask_to_ak(s, b);
        struct jub_point P, cof;
        jub_from_bytes(&P, b);
        jub_mul_by_cofactor(&cof, &P);
        /* [8]P should be a valid non-identity point (P is not small order) */
        bool ok = !jub_is_identity(&cof);
        /* [8]P compressed should decompress back */
        uint8_t cof_bytes[32];
        jub_to_bytes(cof_bytes, &cof);
        struct jub_point cof2;
        ok = ok && jub_from_bytes(&cof2, cof_bytes);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Jubjub get_x/get_y... ");
    {
        uint8_t s[32] = {13};
        uint8_t b[32];
        sapling_ask_to_ak(s, b);
        struct jub_point P;
        jub_from_bytes(&P, b);
        struct fr x, y;
        jub_get_x(&x, &P);
        jub_get_y(&y, &P);
        /* x and y should be non-zero for a non-identity point */
        bool ok = !fr_is_zero(&x) && !fr_is_zero(&y);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================ */
    /* BLS12-381 field arithmetic tests                                 */
    /* ================================================================ */

    printf("Fp zero/one identity... ");
    {
        struct fp z, o;
        fp_zero(&z);
        fp_one(&o);
        bool ok = fp_is_zero(&z) && !fp_is_zero(&o);
        struct fp sum;
        fp_add(&sum, &z, &o);
        ok = ok && fp_eq(&sum, &o);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fp add/sub/neg... ");
    {
        struct fp a, b, sum, diff, neg_a;
        fp_one(&a);
        fp_add(&b, &a, &a);
        fp_sub(&diff, &b, &a);
        bool ok = fp_eq(&diff, &a);
        fp_neg(&neg_a, &a);
        fp_add(&sum, &a, &neg_a);
        ok = ok && fp_is_zero(&sum);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fp mul/sq/inv... ");
    {
        struct fp a, two, four, sq_two, inv_two, prod;
        fp_one(&a);
        fp_add(&two, &a, &a);
        fp_mul(&four, &two, &two);
        fp_sq(&sq_two, &two);
        bool ok = fp_eq(&four, &sq_two);
        fp_inv(&inv_two, &two);
        fp_mul(&prod, &inv_two, &two);
        ok = ok && fp_eq(&prod, &a);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fp from_bytes/to_bytes roundtrip... ");
    {
        /* Use the canonical generator x-coordinate */
        uint8_t bytes[48] = {0};
        bytes[47] = 42; /* small value in big-endian */
        struct fp a;
        bool ok = fp_from_bytes(&a, bytes);
        uint8_t out[48];
        fp_to_bytes(out, &a);
        ok = ok && (memcmp(bytes, out, 48) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fp sqrt... ");
    {
        struct fp a, sq, root;
        fp_one(&a);
        fp_add(&a, &a, &a); /* a = 2 */
        fp_sq(&sq, &a);     /* sq = 4 */
        bool ok = fp_sqrt(&root, &sq);
        /* root^2 should equal sq */
        struct fp check;
        fp_sq(&check, &root);
        ok = ok && fp_eq(&check, &sq);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fp2 arithmetic... ");
    {
        struct fp2 z, o, sum, prod;
        fp2_zero(&z);
        fp2_one(&o);
        bool ok = fp2_is_zero(&z) && !fp2_is_zero(&o);
        fp2_add(&sum, &z, &o);
        ok = ok && fp2_eq(&sum, &o);
        fp2_mul(&prod, &o, &o);
        ok = ok && fp2_eq(&prod, &o); /* 1 * 1 = 1 */
        struct fp2 inv_o;
        fp2_inv(&inv_o, &o);
        fp2_mul(&prod, &o, &inv_o);
        struct fp2 one2;
        fp2_one(&one2);
        ok = ok && fp2_eq(&prod, &one2);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("Fp12 arithmetic... ");
    {
        struct fp12 z, o, prod;
        fp12_zero(&z);
        fp12_one(&o);
        bool ok = fp12_is_zero(&z) && !fp12_is_zero(&o);
        fp12_mul(&prod, &o, &o);
        struct fp12 one12;
        fp12_one(&one12);
        /* Compare Fp12 by checking c0.c0.c0 limbs (1*1 should still be 1) */
        ok = ok && fp_eq(&prod.c0.c0.c0, &one12.c0.c0.c0);
        struct fp12 inv_o;
        fp12_inv(&inv_o, &o);
        fp12_mul(&prod, &o, &inv_o);
        ok = ok && fp_eq(&prod.c0.c0.c0, &one12.c0.c0.c0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("G1 identity... ");
    {
        struct g1_point id, dbl;
        g1_identity(&id);
        bool ok = g1_is_identity(&id);
        g1_double(&dbl, &id);
        ok = ok && g1_is_identity(&dbl);
        struct g1_point sum;
        g1_add(&sum, &id, &id);
        ok = ok && g1_is_identity(&sum);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("G1 generator compressed roundtrip... ");
    {
        /* BLS12-381 G1 generator compressed (48 bytes, big-endian) */
        const uint8_t g1_gen_compressed[48] = {
            0x97,0xf1,0xd3,0xa7,0x31,0x97,0xd7,0x94,
            0x26,0x95,0x63,0x8c,0x4f,0xa9,0xac,0x0f,
            0xc3,0x68,0x8c,0x4f,0x97,0x74,0xb9,0x05,
            0xa1,0x4e,0x3a,0x3f,0x17,0x1b,0xac,0x58,
            0x6c,0x55,0xe8,0x3f,0xf9,0x7a,0x1a,0xef,
            0xfb,0x3a,0xf0,0x0a,0xdb,0x22,0xc6,0xbb
        };
        struct g1_point gen;
        bool ok = g1_from_compressed(&gen, g1_gen_compressed);
        ok = ok && !g1_is_identity(&gen);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("G2 identity... ");
    {
        struct g2_point id, dbl;
        g2_identity(&id);
        bool ok = g2_is_identity(&id);
        g2_double(&dbl, &id);
        ok = ok && g2_is_identity(&dbl);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("BLS12-381 pairing: e(G1, G2) is not identity... ");
    {
        const uint8_t g1_gen[48] = {
            0x97,0xf1,0xd3,0xa7,0x31,0x97,0xd7,0x94,
            0x26,0x95,0x63,0x8c,0x4f,0xa9,0xac,0x0f,
            0xc3,0x68,0x8c,0x4f,0x97,0x74,0xb9,0x05,
            0xa1,0x4e,0x3a,0x3f,0x17,0x1b,0xac,0x58,
            0x6c,0x55,0xe8,0x3f,0xf9,0x7a,0x1a,0xef,
            0xfb,0x3a,0xf0,0x0a,0xdb,0x22,0xc6,0xbb
        };
        const uint8_t g2_gen[96] = {
            0x93,0xe0,0x2b,0x60,0x52,0x71,0x9f,0x60,
            0x7d,0xac,0xd3,0xa0,0x88,0x27,0x4f,0x65,
            0x59,0x6b,0xd0,0xd0,0x99,0x20,0xb6,0x1a,
            0xb5,0xda,0x61,0xbb,0xdc,0x7f,0x50,0x49,
            0x33,0x4c,0xf1,0x12,0x13,0x94,0x5d,0x57,
            0xe5,0xac,0x7d,0x05,0x5d,0x04,0x2b,0x7e,
            0x02,0x4a,0xa2,0xb2,0xf0,0x8f,0x0a,0x91,
            0x26,0x08,0x05,0x27,0x2d,0xc5,0x10,0x51,
            0xc6,0xe4,0x7a,0xd4,0xfa,0x40,0x3b,0x02,
            0xb4,0x51,0x0b,0x64,0x7a,0xe3,0xd1,0x77,
            0x0b,0xac,0x03,0x26,0xa8,0x05,0xbb,0xef,
            0xd4,0x80,0x56,0xc8,0xc1,0x21,0xbd,0xb8
        };
        struct g1_point p;
        struct g2_point q;
        bool ok = g1_from_compressed(&p, g1_gen);
        ok = ok && g2_from_compressed(&q, g2_gen);
        if (ok) {
            struct fp12 result;
            bls12_381_pairing(&result, &p, &q);
            struct fp12 one12;
            fp12_one(&one12);
            /* e(G1,G2) != 1: check that c1 component is nonzero */
            ok = !fp_is_zero(&result.c1.c0.c0);
            ok = ok && !fp12_is_zero(&result);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("BLS12-381 pairing bilinearity: e(aP, Q) = e(P, aQ)... ");
    {
        const uint8_t g1_gen[48] = {
            0x97,0xf1,0xd3,0xa7,0x31,0x97,0xd7,0x94,
            0x26,0x95,0x63,0x8c,0x4f,0xa9,0xac,0x0f,
            0xc3,0x68,0x8c,0x4f,0x97,0x74,0xb9,0x05,
            0xa1,0x4e,0x3a,0x3f,0x17,0x1b,0xac,0x58,
            0x6c,0x55,0xe8,0x3f,0xf9,0x7a,0x1a,0xef,
            0xfb,0x3a,0xf0,0x0a,0xdb,0x22,0xc6,0xbb
        };
        const uint8_t g2_gen[96] = {
            0x93,0xe0,0x2b,0x60,0x52,0x71,0x9f,0x60,
            0x7d,0xac,0xd3,0xa0,0x88,0x27,0x4f,0x65,
            0x59,0x6b,0xd0,0xd0,0x99,0x20,0xb6,0x1a,
            0xb5,0xda,0x61,0xbb,0xdc,0x7f,0x50,0x49,
            0x33,0x4c,0xf1,0x12,0x13,0x94,0x5d,0x57,
            0xe5,0xac,0x7d,0x05,0x5d,0x04,0x2b,0x7e,
            0x02,0x4a,0xa2,0xb2,0xf0,0x8f,0x0a,0x91,
            0x26,0x08,0x05,0x27,0x2d,0xc5,0x10,0x51,
            0xc6,0xe4,0x7a,0xd4,0xfa,0x40,0x3b,0x02,
            0xb4,0x51,0x0b,0x64,0x7a,0xe3,0xd1,0x77,
            0x0b,0xac,0x03,0x26,0xa8,0x05,0xbb,0xef,
            0xd4,0x80,0x56,0xc8,0xc1,0x21,0xbd,0xb8
        };
        struct g1_point P;
        struct g2_point Q;
        g1_from_compressed(&P, g1_gen);
        g2_from_compressed(&Q, g2_gen);

        uint64_t scalar[4] = {7, 0, 0, 0}; /* a = 7 */

        /* aP */
        struct g1_point aP;
        g1_scalar_mul(&aP, &P, scalar);

        /* aQ = Q + Q + ... (7 times) */
        struct g2_point aQ;
        g2_identity(&aQ);
        for (int i = 0; i < 7; i++) {
            struct g2_point tmp;
            g2_add(&tmp, &aQ, &Q);
            aQ = tmp;
        }

        /* e(aP, Q) */
        struct fp12 lhs;
        bls12_381_pairing(&lhs, &aP, &Q);

        /* e(P, aQ) */
        struct fp12 rhs;
        bls12_381_pairing(&rhs, &P, &aQ);

        /* Compare all Fp12 components via memcmp */
        bool ok = (memcmp(&lhs, &rhs, sizeof(struct fp12)) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("multipack_bytes_to_fr... ");
    {
        uint8_t bytes[32];
        memset(bytes, 0, 32);
        bytes[0] = 0x42;
        uint64_t out[4][4];
        size_t n_out = 0;
        multipack_bytes_to_fr(out, &n_out, bytes, 32);
        bool ok = (n_out >= 1);
        /* First scalar should be nonzero */
        ok = ok && (out[0][0] != 0 || out[0][1] != 0 || out[0][2] != 0 || out[0][3] != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================ */
    /* PRF standalone tests                                             */
    /* ================================================================ */

    printf("PRF ask/nsk/ovk deterministic... ");
    {
        uint8_t sk[32] = {0};
        struct uint256 sk_u;
        memcpy(sk_u.data, sk, 32);
        struct uint256 ask1, ask2, nsk, ovk;
        prf_ask(&sk_u, &ask1);
        prf_ask(&sk_u, &ask2);
        prf_nsk(&sk_u, &nsk);
        prf_ovk(&sk_u, &ovk);
        bool ok = (memcmp(ask1.data, ask2.data, 32) == 0); /* deterministic */
        /* ask != nsk != ovk */
        ok = ok && (memcmp(ask1.data, nsk.data, 32) != 0);
        ok = ok && (memcmp(ask1.data, ovk.data, 32) != 0);
        ok = ok && (memcmp(nsk.data, ovk.data, 32) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("PRF expand... ");
    {
        uint8_t sk[32] = {0};
        struct uint256 sk_u;
        memcpy(sk_u.data, sk, 32);
        uint8_t out0[64], out1[64];
        prf_expand(&sk_u, 0, out0);
        prf_expand(&sk_u, 1, out1);
        /* Different t → different output */
        bool ok = (memcmp(out0, out1, 64) != 0);
        /* Same t → same output (deterministic) */
        uint8_t out0b[64];
        prf_expand(&sk_u, 0, out0b);
        ok = ok && (memcmp(out0, out0b, 64) == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("PRF addr a_pk/sk_enc (Sprout)... ");
    {
        uint8_t a_sk[32] = {0};
        a_sk[0] = 0x42;
        struct uint256 a_pk, sk_enc;
        prf_addr_a_pk(a_sk, &a_pk);
        prf_addr_sk_enc(a_sk, &sk_enc);
        /* Should produce non-zero outputs */
        uint8_t zeros[32] = {0};
        bool ok = (memcmp(a_pk.data, zeros, 32) != 0);
        ok = ok && (memcmp(sk_enc.data, zeros, 32) != 0);
        /* a_pk != sk_enc */
        ok = ok && (memcmp(a_pk.data, sk_enc.data, 32) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================ */
    /* Sprout h_sig test                                                */
    /* ================================================================ */

    printf("Sprout h_sig deterministic... ");
    {
        uint8_t random_seed[32], nf0[32], nf1[32], pk[32];
        memset(random_seed, 0x01, 32);
        memset(nf0, 0x02, 32);
        memset(nf1, 0x03, 32);
        memset(pk, 0x04, 32);
        uint8_t h1[32], h2[32];
        sprout_h_sig(random_seed, nf0, nf1, pk, h1);
        sprout_h_sig(random_seed, nf0, nf1, pk, h2);
        bool ok = (memcmp(h1, h2, 32) == 0); /* deterministic */
        /* Change input → different output */
        uint8_t h3[32];
        nf0[0] = 0xff;
        sprout_h_sig(random_seed, nf0, nf1, pk, h3);
        ok = ok && (memcmp(h1, h3, 32) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ================================================================ */
    /* FF1 format-preserving encryption test                            */
    /* ================================================================ */

    printf("FF1 encrypt deterministic... ");
    {
        uint8_t key[32];
        memset(key, 0x42, 32);
        uint8_t tweak[4] = {0x01, 0x02, 0x03, 0x04};
        uint8_t data1[11] = {0};
        uint8_t data2[11] = {0};
        ff1_aes256_encrypt(key, tweak, 4, data1, 88);
        ff1_aes256_encrypt(key, tweak, 4, data2, 88);
        bool ok = (memcmp(data1, data2, 11) == 0); /* deterministic */
        /* Output should differ from input (all zeros) */
        uint8_t zeros[11] = {0};
        ok = ok && (memcmp(data1, zeros, 11) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("FF1 different keys produce different output... ");
    {
        uint8_t key1[32], key2[32];
        memset(key1, 0x11, 32);
        memset(key2, 0x22, 32);
        uint8_t data1[11] = {0}, data2[11] = {0};
        ff1_aes256_encrypt(key1, NULL, 0, data1, 88);
        ff1_aes256_encrypt(key2, NULL, 0, data2, 88);
        bool ok = (memcmp(data1, data2, 11) != 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
