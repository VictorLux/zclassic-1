/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling key operations — pure C23 implementation.
 * group_hash, key derivation, commitment, nullifier, RedJubjub, verification context. */

#include "zcash/sapling.h"
#include "zcash/pedersen_hash.h"
#include "zcash/jubjub.h"
#include "crypto/blake2s.h"
#include "crypto/blake2b.h"
#include <string.h>

/* URS: first 64 bytes of BLAKE2s input for group_hash (rigidity constant) */
static const uint8_t GH_FIRST_BLOCK[] =
    "096b36a5804bfacef1691e173c366a47ff5ba84a44f26ddd7e8d9f79d5b42df0";

bool group_hash(struct jub_point *result,
                const uint8_t *tag, size_t tag_len,
                const uint8_t personalization[8])
{
    struct blake2s_ctx ctx;
    uint8_t hash[32];

    blake2s_init_personal(&ctx, 32, personalization);
    blake2s_update(&ctx, GH_FIRST_BLOCK, 64);
    blake2s_update(&ctx, tag, tag_len);
    blake2s_final(&ctx, hash, 32);

    if (!jub_from_bytes(result, hash))
        return false;

    jub_mul_by_cofactor(result, result);

    return !jub_is_identity(result);
}

/* find_group_hash: try counter bytes until group_hash succeeds */
static bool find_group_hash(struct jub_point *result,
                             const uint8_t *m, size_t m_len,
                             const uint8_t personalization[8])
{
    uint8_t tag[256];
    if (m_len + 1 > sizeof(tag)) return false;
    memcpy(tag, m, m_len);
    tag[m_len] = 0;

    for (int i = 0; i < 256; i++) {
        tag[m_len] = (uint8_t)i;
        if (group_hash(result, tag, m_len + 1, personalization))
            return true;
    }
    return false;
}

/* Fixed generators, lazily computed */
enum {
    GEN_PROOF_GENERATION_KEY = 0,
    GEN_NOTE_COMMITMENT_RANDOMNESS = 1,
    GEN_NULLIFIER_POSITION = 2,
    GEN_VALUE_COMMITMENT_VALUE = 3,
    GEN_VALUE_COMMITMENT_RANDOMNESS = 4,
    GEN_SPENDING_KEY = 5,
    GEN_MAX = 6
};

static struct jub_point fixed_generators[GEN_MAX];
static bool fixed_generators_loaded = false;

static void ensure_fixed_generators(void)
{
    if (fixed_generators_loaded) return;

    find_group_hash(&fixed_generators[GEN_PROOF_GENERATION_KEY],
                    (const uint8_t *)"", 0,
                    (const uint8_t *)"Zcash_H_");

    find_group_hash(&fixed_generators[GEN_NOTE_COMMITMENT_RANDOMNESS],
                    (const uint8_t *)"r", 1,
                    (const uint8_t *)"Zcash_PH");

    find_group_hash(&fixed_generators[GEN_NULLIFIER_POSITION],
                    (const uint8_t *)"", 0,
                    (const uint8_t *)"Zcash_J_");

    find_group_hash(&fixed_generators[GEN_VALUE_COMMITMENT_VALUE],
                    (const uint8_t *)"v", 1,
                    (const uint8_t *)"Zcash_cv");

    find_group_hash(&fixed_generators[GEN_VALUE_COMMITMENT_RANDOMNESS],
                    (const uint8_t *)"r", 1,
                    (const uint8_t *)"Zcash_cv");

    find_group_hash(&fixed_generators[GEN_SPENDING_KEY],
                    (const uint8_t *)"", 0,
                    (const uint8_t *)"Zcash_G_");

    fixed_generators_loaded = true;
}

bool sapling_check_diversifier(const uint8_t diversifier[11])
{
    struct jub_point g_d;
    return group_hash(&g_d, diversifier, 11,
                      (const uint8_t *)"Zcash_gd");
}

bool sapling_diversifier_to_gd(struct jub_point *g_d, const uint8_t diversifier[11])
{
    return group_hash(g_d, diversifier, 11,
                      (const uint8_t *)"Zcash_gd");
}

void sapling_ask_to_ak(const uint8_t ask[32], uint8_t ak[32])
{
    ensure_fixed_generators();
    struct jub_point result;
    jub_scalar_mul(&result, &fixed_generators[GEN_SPENDING_KEY], ask);
    jub_to_bytes(ak, &result);
}

void sapling_nsk_to_nk(const uint8_t nsk[32], uint8_t nk[32])
{
    ensure_fixed_generators();
    struct jub_point result;
    jub_scalar_mul(&result, &fixed_generators[GEN_PROOF_GENERATION_KEY], nsk);
    jub_to_bytes(nk, &result);
}

void sapling_crh_ivk(const uint8_t ak[32], const uint8_t nk[32], uint8_t ivk[32])
{
    struct blake2s_ctx ctx;
    blake2s_init_personal(&ctx, 32, (const uint8_t *)"Zcashivk");
    blake2s_update(&ctx, ak, 32);
    blake2s_update(&ctx, nk, 32);
    blake2s_final(&ctx, ivk, 32);
    /* Drop top 5 bits so it can be interpreted as Fs scalar */
    ivk[31] &= 0x07;
}

bool sapling_ivk_to_pkd(const uint8_t ivk[32], const uint8_t diversifier[11],
                         uint8_t pk_d[32])
{
    struct jub_point g_d;
    if (!sapling_diversifier_to_gd(&g_d, diversifier))
        return false;

    struct jub_point result;
    jub_scalar_mul(&result, &g_d, ivk);
    jub_to_bytes(pk_d, &result);
    return true;
}

bool sapling_ka_agree(const uint8_t p[32], const uint8_t sk[32], uint8_t result[32])
{
    struct jub_point point;
    if (!jub_from_bytes(&point, p))
        return false;

    /* Multiply by cofactor 8 first */
    jub_mul_by_cofactor(&point, &point);

    /* Then multiply by sk */
    struct jub_point out;
    jub_scalar_mul(&out, &point, sk);

    jub_to_bytes(result, &out);
    return true;
}

bool sapling_ka_derivepublic(const uint8_t diversifier[11], const uint8_t esk[32],
                              uint8_t result[32])
{
    struct jub_point g_d;
    if (!sapling_diversifier_to_gd(&g_d, diversifier))
        return false;

    struct jub_point out;
    jub_scalar_mul(&out, &g_d, esk);
    jub_to_bytes(result, &out);
    return true;
}

/* Pedersen hash with NoteCommitment personalization (all 6 bits set) */
static void pedersen_note_commitment(const uint8_t *data, size_t data_len,
                                      struct jub_point *result)
{
    /* Extract all bits from data */
    size_t nbits = data_len * 8;

    /* Personalization bits: NoteCommitment = [1,1,1,1,1,1] */
    uint8_t bits[6 + 8 * 576]; /* 6 personal + up to 576 data bits */
    int pos = 0;

    for (int i = 0; i < 6; i++)
        bits[pos++] = 1;

    for (size_t i = 0; i < nbits; i++) {
        int byte_idx = (int)(i / 8);
        int bit_idx = (int)(i % 8);
        bits[pos++] = (data[byte_idx] >> bit_idx) & 1;
    }

    /* Use the same Pedersen hash engine but with NoteCommitment personalization.
     * We need a version that takes raw bits. For now, we implement the same
     * scalar accumulation + generator multiplication as pedersen_merkle_hash
     * but with the NoteCommitment personalization already encoded in bits[]. */

    /* The pedersen_hash function from Rust takes personalization bits prepended
     * to the data bits, then processes in 3-bit chunks across generators.
     * Our pedersen_merkle_hash hardcodes MerkleTree personalization.
     * We need a generic version. */

    /* Call the generic Pedersen hash with pre-assembled bits */
    extern void pedersen_hash_bits(const uint8_t *bits, int nbits,
                                    struct jub_point *result);
    pedersen_hash_bits(bits, pos, result);
}

bool sapling_compute_cm(const uint8_t diversifier[11], const uint8_t pk_d[32],
                         uint64_t value, const uint8_t rcm[32],
                         uint8_t cm[32])
{
    ensure_fixed_generators();

    /* Note contents: value(8 LE) || g_d(32) || pk_d(32) = 72 bytes */
    uint8_t note_contents[72];

    /* value as 8 bytes LE */
    for (int i = 0; i < 8; i++)
        note_contents[i] = (uint8_t)(value >> (i * 8));

    /* g_d = compressed point from diversifier */
    struct jub_point g_d;
    if (!sapling_diversifier_to_gd(&g_d, diversifier))
        return false;
    jub_to_bytes(note_contents + 8, &g_d);

    /* pk_d */
    memcpy(note_contents + 40, pk_d, 32);

    /* hash_of_contents = PedersenHash(NoteCommitment, note_contents as bits) */
    struct jub_point hash_pt;
    pedersen_note_commitment(note_contents, 72, &hash_pt);

    /* cm_full_point = hash_pt + rcm * NoteCommitmentRandomness */
    struct jub_point rcm_point;
    jub_scalar_mul(&rcm_point, &fixed_generators[GEN_NOTE_COMMITMENT_RANDOMNESS], rcm);
    jub_add(&hash_pt, &rcm_point, &hash_pt);

    /* cm = x-coordinate of cm_full_point */
    struct fr x_coord;
    jub_get_x(&x_coord, &hash_pt);
    fr_to_bytes(cm, &x_coord);
    return true;
}

bool sapling_compute_nf(const uint8_t diversifier[11], const uint8_t pk_d[32],
                         uint64_t value, const uint8_t rcm[32],
                         const uint8_t ak_bytes[32], const uint8_t nk_bytes[32],
                         uint64_t position, uint8_t nf[32])
{
    ensure_fixed_generators();
    (void)ak_bytes; /* ak used only for viewing key construction, nf needs nk */

    /* First compute cm_full_point (same as in compute_cm but keep the point) */
    uint8_t note_contents[72];
    for (int i = 0; i < 8; i++)
        note_contents[i] = (uint8_t)(value >> (i * 8));

    struct jub_point g_d;
    if (!sapling_diversifier_to_gd(&g_d, diversifier))
        return false;
    jub_to_bytes(note_contents + 8, &g_d);
    memcpy(note_contents + 40, pk_d, 32);

    struct jub_point cm_pt;
    pedersen_note_commitment(note_contents, 72, &cm_pt);

    struct jub_point rcm_point;
    jub_scalar_mul(&rcm_point, &fixed_generators[GEN_NOTE_COMMITMENT_RANDOMNESS], rcm);
    jub_add(&cm_pt, &rcm_point, &cm_pt);

    /* rho = cm_full_point + position * NullifierPosition */
    uint8_t pos_bytes[32] = {0};
    for (int i = 0; i < 8; i++)
        pos_bytes[i] = (uint8_t)(position >> (i * 8));

    struct jub_point pos_point;
    jub_scalar_mul(&pos_point, &fixed_generators[GEN_NULLIFIER_POSITION], pos_bytes);

    struct jub_point rho;
    jub_add(&rho, &cm_pt, &pos_point);

    /* nf = BLAKE2s("Zcash_nf", nk || rho_compressed) */
    uint8_t rho_bytes[32];
    jub_to_bytes(rho_bytes, &rho);

    struct blake2s_ctx ctx;
    blake2s_init_personal(&ctx, 32, (const uint8_t *)"Zcash_nf");
    blake2s_update(&ctx, nk_bytes, 32);
    blake2s_update(&ctx, rho_bytes, 32);
    blake2s_final(&ctx, nf, 32);
    return true;
}

/* --- RedJubjub signature verification --- */

/* H* = BLAKE2b-512("Zcash_RedJubjubH", a || b) → reduce to Fs via to_uniform */
static void h_star(const uint8_t *a, size_t a_len,
                    const uint8_t *b, size_t b_len,
                    uint8_t scalar[32])
{
    static const uint8_t personal[16] = {'Z','c','a','s','h','_','R','e','d','J','u','b','j','u','b','H'};
    uint8_t digest[64];
    struct blake2b_ctx ctx;
    blake2b_init_salt_personal(&ctx, 64, NULL, 0, NULL, personal);
    blake2b_update(&ctx, a, a_len);
    blake2b_update(&ctx, b, b_len);
    blake2b_final(&ctx, digest, 64);
    jubjub_to_scalar(digest, scalar);
}

/* Check if point has small order: [8]*p == identity */
static bool is_small_order(const uint8_t point_bytes[32])
{
    struct jub_point p;
    if (!jub_from_bytes(&p, point_bytes))
        return true;
    struct jub_point cofactored;
    jub_mul_by_cofactor(&cofactored, &p);
    return jub_is_identity(&cofactored);
}

bool redjubjub_verify(const uint8_t vk_bytes[32],
                       const uint8_t msg[64],
                       const uint8_t sig_rbar[32],
                       const uint8_t sig_sbar[32],
                       int generator_idx)
{
    ensure_fixed_generators();

    /* Deserialize vk */
    struct jub_point vk;
    if (!jub_from_bytes(&vk, vk_bytes))
        return false;

    /* Deserialize R */
    struct jub_point R;
    if (!jub_from_bytes(&R, sig_rbar))
        return false;

    /* c = H*(Rbar || msg) */
    uint8_t c_scalar[32];
    h_star(sig_rbar, 32, msg, 64, c_scalar);

    /* S as scalar bytes */
    /* Check S < Fs order (optional, Rust checks via from_repr) */

    /* Verify: [8] * (R + c*vk - S*G) == 0
     * Equivalently: [8] * (-S*G + R + c*vk) == 0 */

    /* c * vk */
    struct jub_point c_vk;
    jub_scalar_mul(&c_vk, &vk, c_scalar);

    /* S * G */
    struct jub_point s_g;
    jub_scalar_mul(&s_g, &fixed_generators[generator_idx], sig_sbar);

    /* -S*G */
    struct jub_point neg_s_g;
    jub_neg(&neg_s_g, &s_g);

    /* R + c*vk + (-S*G) */
    struct jub_point sum;
    jub_add(&sum, &R, &c_vk);
    jub_add(&sum, &sum, &neg_s_g);

    /* [8] * sum */
    struct jub_point result;
    jub_mul_by_cofactor(&result, &sum);

    return jub_is_identity(&result);
}

/* --- Sapling verification context --- */

void sapling_verification_ctx_init(struct sapling_verification_ctx *ctx)
{
    jub_identity(&ctx->bvk);
}

bool sapling_check_spend(struct sapling_verification_ctx *ctx,
                          const uint8_t cv[32],
                          const uint8_t anchor[32],
                          const uint8_t nullifier[32],
                          const uint8_t rk[32],
                          const uint8_t zkproof[192],
                          const uint8_t spend_auth_sig[64],
                          const uint8_t sighash[32])
{
    ensure_fixed_generators();
    (void)anchor; (void)nullifier; (void)zkproof; /* Groth16 verification TODO */

    /* Deserialize cv */
    struct jub_point cv_point;
    if (!jub_from_bytes(&cv_point, cv))
        return false;

    /* Small order check */
    if (is_small_order(cv))
        return false;

    /* Accumulate cv into bvk */
    jub_add(&ctx->bvk, &ctx->bvk, &cv_point);

    /* Small order check on rk */
    if (is_small_order(rk))
        return false;

    /* Verify spend_auth_sig: msg = rk || sighash */
    uint8_t data_to_sign[64];
    memcpy(data_to_sign, rk, 32);
    memcpy(data_to_sign + 32, sighash, 32);

    if (!redjubjub_verify(rk, data_to_sign,
                           spend_auth_sig, spend_auth_sig + 32,
                           GEN_SPENDING_KEY))
        return false;

    /* TODO: Groth16 proof verification */
    return true;
}

bool sapling_check_output(struct sapling_verification_ctx *ctx,
                           const uint8_t cv[32],
                           const uint8_t cm[32],
                           const uint8_t epk[32],
                           const uint8_t zkproof[192])
{
    (void)cm; (void)epk; (void)zkproof; /* Groth16 verification TODO */

    /* Deserialize cv */
    struct jub_point cv_point;
    if (!jub_from_bytes(&cv_point, cv))
        return false;

    /* Small order check */
    if (is_small_order(cv))
        return false;

    /* Subtract cv from bvk (outputs are negative) */
    struct jub_point neg_cv;
    jub_neg(&neg_cv, &cv_point);
    jub_add(&ctx->bvk, &ctx->bvk, &neg_cv);

    /* TODO: Groth16 proof verification */
    return true;
}

bool sapling_final_check(struct sapling_verification_ctx *ctx,
                          int64_t value_balance,
                          const uint8_t binding_sig[64],
                          const uint8_t sighash[32])
{
    ensure_fixed_generators();

    /* Compute value balance point: value_balance * ValueCommitmentValue */
    struct jub_point value_pt;
    if (value_balance == 0) {
        jub_identity(&value_pt);
    } else {
        uint64_t abs_val;
        bool negate;
        if (value_balance > 0) {
            abs_val = (uint64_t)value_balance;
            negate = false;
        } else {
            abs_val = (uint64_t)(-value_balance);
            negate = true;
        }
        uint8_t scalar[32] = {0};
        for (int i = 0; i < 8; i++)
            scalar[i] = (uint8_t)(abs_val >> (i * 8));

        jub_scalar_mul(&value_pt, &fixed_generators[GEN_VALUE_COMMITMENT_VALUE], scalar);
        if (negate)
            jub_neg(&value_pt, &value_pt);
    }

    /* bvk = bvk - value_balance */
    struct jub_point neg_value_pt;
    jub_neg(&neg_value_pt, &value_pt);
    struct jub_point final_bvk;
    jub_add(&final_bvk, &ctx->bvk, &neg_value_pt);

    /* Compute msg = bvk_compressed || sighash */
    uint8_t bvk_bytes[32];
    jub_to_bytes(bvk_bytes, &final_bvk);

    uint8_t data_to_sign[64];
    memcpy(data_to_sign, bvk_bytes, 32);
    memcpy(data_to_sign + 32, sighash, 32);

    /* Verify binding sig with ValueCommitmentRandomness generator */
    return redjubjub_verify(bvk_bytes, data_to_sign,
                             binding_sig, binding_sig + 32,
                             GEN_VALUE_COMMITMENT_RANDOMNESS);
}
