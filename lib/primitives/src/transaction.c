/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "primitives/transaction.h"
#include "core/hash.h"
#include "core/serialize.h"
#include <stdlib.h>
#include <stdio.h>

void transaction_init(struct transaction *tx)
{
    tx->overwintered = false;
    tx->version = 1;
    tx->version_group_id = 0;
    tx->vin = NULL;
    tx->num_vin = 0;
    tx->vout = NULL;
    tx->num_vout = 0;
    tx->lock_time = 0;
    tx->expiry_height = 0;
    tx->value_balance = 0;
    tx->v_shielded_spend = NULL;
    tx->num_shielded_spend = 0;
    tx->v_shielded_output = NULL;
    tx->num_shielded_output = 0;
    tx->v_joinsplit = NULL;
    tx->num_joinsplit = 0;
    uint256_set_null(&tx->joinsplit_pubkey);
    memset(tx->joinsplit_sig, 0, 64);
    memset(tx->binding_sig, 0, 64);
    uint256_set_null(&tx->hash);
}

void transaction_free(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
    free(tx->v_shielded_spend);
    free(tx->v_shielded_output);
    free(tx->v_joinsplit);
    tx->vin = NULL;
    tx->vout = NULL;
    tx->v_shielded_spend = NULL;
    tx->v_shielded_output = NULL;
    tx->v_joinsplit = NULL;
    tx->num_vin = 0;
    tx->num_vout = 0;
    tx->num_shielded_spend = 0;
    tx->num_shielded_output = 0;
    tx->num_joinsplit = 0;
}

bool transaction_alloc(struct transaction *tx, size_t num_vin, size_t num_vout)
{
    if (num_vin > MAX_TX_INPUTS || num_vout > MAX_TX_OUTPUTS)
        return false;

    /* Zero-size calls must not allocate. glibc's calloc(0, n) returns a
     * unique 1-byte pointer that must be freed; callers that later replace
     * tx->vin/tx->vout with a fresh allocation (e.g. transaction_deserialize
     * partial paths) would silently leak that stub. Treat zero as "no
     * array" and leave the pointer NULL so transaction_free is a no-op. */
    tx->vin  = num_vin  ? calloc(num_vin,  sizeof(struct tx_in))  : NULL;
    tx->vout = num_vout ? calloc(num_vout, sizeof(struct tx_out)) : NULL;
    if ((num_vin && !tx->vin) || (num_vout && !tx->vout)) {
        fprintf(stderr, "transaction_alloc FAILED: vin=%zu (need %zu MB) vout=%zu\n",
                num_vin, num_vin * sizeof(struct tx_in) / (1024*1024), num_vout);
        free(tx->vin);
        free(tx->vout);
        tx->vin = NULL;
        tx->vout = NULL;
        return false;
    }
    tx->num_vin = num_vin;
    tx->num_vout = num_vout;

    for (size_t i = 0; i < num_vin; i++)
        tx_in_init(&tx->vin[i]);
    for (size_t i = 0; i < num_vout; i++)
        tx_out_set_null(&tx->vout[i]);

    return true;
}

bool transaction_copy(struct transaction *dst, const struct transaction *src)
{
    transaction_init(dst);
    dst->overwintered = src->overwintered;
    dst->version = src->version;
    dst->version_group_id = src->version_group_id;
    dst->lock_time = src->lock_time;
    dst->expiry_height = src->expiry_height;
    dst->value_balance = src->value_balance;
    dst->hash = src->hash;

    if (!transaction_alloc(dst, src->num_vin, src->num_vout)) {
        transaction_free(dst);
        return false;
    }

    for (size_t i = 0; i < src->num_vin; i++) {
        dst->vin[i].prevout = src->vin[i].prevout;
        dst->vin[i].sequence = src->vin[i].sequence;
        memcpy(dst->vin[i].script_sig.data, src->vin[i].script_sig.data,
               src->vin[i].script_sig.size);
        dst->vin[i].script_sig.size = src->vin[i].script_sig.size;
    }

    for (size_t i = 0; i < src->num_vout; i++) {
        dst->vout[i].value = src->vout[i].value;
        memcpy(dst->vout[i].script_pub_key.data,
               src->vout[i].script_pub_key.data,
               src->vout[i].script_pub_key.size);
        dst->vout[i].script_pub_key.size = src->vout[i].script_pub_key.size;
    }

    if (src->num_shielded_spend > 0) {
        dst->v_shielded_spend = calloc(src->num_shielded_spend,
                                        sizeof(struct spend_description));
        if (!dst->v_shielded_spend) { transaction_free(dst); return false; }
        dst->num_shielded_spend = src->num_shielded_spend;
        memcpy(dst->v_shielded_spend, src->v_shielded_spend,
               src->num_shielded_spend * sizeof(struct spend_description));
    }

    if (src->num_shielded_output > 0) {
        dst->v_shielded_output = calloc(src->num_shielded_output,
                                         sizeof(struct output_description));
        if (!dst->v_shielded_output) { transaction_free(dst); return false; }
        dst->num_shielded_output = src->num_shielded_output;
        memcpy(dst->v_shielded_output, src->v_shielded_output,
               src->num_shielded_output * sizeof(struct output_description));
    }

    if (src->num_joinsplit > 0) {
        dst->v_joinsplit = calloc(src->num_joinsplit,
                                   sizeof(struct js_description));
        if (!dst->v_joinsplit) { transaction_free(dst); return false; }
        dst->num_joinsplit = src->num_joinsplit;
        memcpy(dst->v_joinsplit, src->v_joinsplit,
               src->num_joinsplit * sizeof(struct js_description));
    }

    dst->joinsplit_pubkey = src->joinsplit_pubkey;
    memcpy(dst->joinsplit_sig, src->joinsplit_sig, 64);
    memcpy(dst->binding_sig, src->binding_sig, 64);

    return true;
}

int64_t transaction_get_value_out(const struct transaction *tx)
{
    int64_t total = 0;
    for (size_t i = 0; i < tx->num_vout; i++) {
        total += tx->vout[i].value;
        if (!MoneyRange(tx->vout[i].value) || !MoneyRange(total))
            return -1;
    }

    if (tx->value_balance <= 0) {
        int64_t neg = -tx->value_balance;
        total += neg;
        if (!MoneyRange(neg) || !MoneyRange(total))
            return -1;
    }

    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        total += tx->v_joinsplit[i].vpub_old;
        if (!MoneyRange(tx->v_joinsplit[i].vpub_old) || !MoneyRange(total))
            return -1;
    }
    return total;
}

int64_t transaction_get_shielded_value_in(const struct transaction *tx)
{
    int64_t total = 0;
    if (tx->value_balance >= 0) {
        total += tx->value_balance;
        if (!MoneyRange(tx->value_balance) || !MoneyRange(total))
            return -1;
    }
    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        total += tx->v_joinsplit[i].vpub_new;
        if (!MoneyRange(tx->v_joinsplit[i].vpub_new) || !MoneyRange(total))
            return -1;
    }
    return total;
}

void outpoint_to_string(const struct outpoint *op, char *buf, size_t buflen)
{
    char hex[65];
    uint256_get_hex(&op->hash, hex);
    snprintf(buf, buflen, "%s:%u", hex, op->n);
}

bool outpoint_serialize(const struct outpoint *op, struct byte_stream *s)
{
    return stream_write_bytes(s, op->hash.data, 32) &&
           stream_write_u32_le(s, op->n);
}

bool outpoint_deserialize(struct outpoint *op, struct byte_stream *s)
{
    return stream_read_bytes(s, op->hash.data, 32) &&
           stream_read_u32_le(s, &op->n);
}

bool tx_in_serialize(const struct tx_in *in, struct byte_stream *s)
{
    if (!outpoint_serialize(&in->prevout, s)) return false;
    if (!stream_write_compact_size(s, in->script_sig.size)) return false;
    if (in->script_sig.size > 0 &&
        !stream_write_bytes(s, in->script_sig.data, in->script_sig.size))
        return false;
    return stream_write_u32_le(s, in->sequence);
}

bool tx_in_deserialize(struct tx_in *in, struct byte_stream *s)
{
    if (!outpoint_deserialize(&in->prevout, s)) return false;
    uint64_t script_len;
    if (!stream_read_compact_size(s, &script_len)) return false;
    if (script_len > MAX_SCRIPT_SIZE) return false;
    in->script_sig.size = (size_t)script_len;
    if (in->script_sig.size > 0 &&
        !stream_read_bytes(s, in->script_sig.data, in->script_sig.size))
        return false;
    return stream_read_u32_le(s, &in->sequence);
}

bool tx_out_serialize(const struct tx_out *out, struct byte_stream *s)
{
    if (!stream_write_i64_le(s, out->value)) return false;
    if (!stream_write_compact_size(s, out->script_pub_key.size)) return false;
    if (out->script_pub_key.size > 0)
        return stream_write_bytes(s, out->script_pub_key.data,
                                  out->script_pub_key.size);
    return true;
}

bool tx_out_deserialize(struct tx_out *out, struct byte_stream *s)
{
    if (!stream_read_i64_le(s, &out->value)) return false;
    uint64_t script_len;
    if (!stream_read_compact_size(s, &script_len)) return false;
    if (script_len > MAX_SCRIPT_SIZE) return false;
    out->script_pub_key.size = (size_t)script_len;
    if (out->script_pub_key.size > 0)
        return stream_read_bytes(s, out->script_pub_key.data,
                                 out->script_pub_key.size);
    return true;
}

bool spend_description_serialize(const struct spend_description *sd,
                                  struct byte_stream *s)
{
    return stream_write_bytes(s, sd->cv.data, 32) &&
           stream_write_bytes(s, sd->anchor.data, 32) &&
           stream_write_bytes(s, sd->nullifier.data, 32) &&
           stream_write_bytes(s, sd->rk.data, 32) &&
           stream_write_bytes(s, sd->zkproof, GROTH_PROOF_SIZE) &&
           stream_write_bytes(s, sd->spend_auth_sig, 64);
}

bool spend_description_deserialize(struct spend_description *sd,
                                    struct byte_stream *s)
{
    return stream_read_bytes(s, sd->cv.data, 32) &&
           stream_read_bytes(s, sd->anchor.data, 32) &&
           stream_read_bytes(s, sd->nullifier.data, 32) &&
           stream_read_bytes(s, sd->rk.data, 32) &&
           stream_read_bytes(s, sd->zkproof, GROTH_PROOF_SIZE) &&
           stream_read_bytes(s, sd->spend_auth_sig, 64);
}

bool output_description_serialize(const struct output_description *od,
                                   struct byte_stream *s)
{
    return stream_write_bytes(s, od->cv.data, 32) &&
           stream_write_bytes(s, od->cm.data, 32) &&
           stream_write_bytes(s, od->ephemeral_key.data, 32) &&
           stream_write_bytes(s, od->enc_ciphertext, ZC_SAPLING_ENCCIPHERTEXT_SIZE) &&
           stream_write_bytes(s, od->out_ciphertext, ZC_SAPLING_OUTCIPHERTEXT_SIZE) &&
           stream_write_bytes(s, od->zkproof, GROTH_PROOF_SIZE);
}

bool output_description_deserialize(struct output_description *od,
                                     struct byte_stream *s)
{
    return stream_read_bytes(s, od->cv.data, 32) &&
           stream_read_bytes(s, od->cm.data, 32) &&
           stream_read_bytes(s, od->ephemeral_key.data, 32) &&
           stream_read_bytes(s, od->enc_ciphertext, ZC_SAPLING_ENCCIPHERTEXT_SIZE) &&
           stream_read_bytes(s, od->out_ciphertext, ZC_SAPLING_OUTCIPHERTEXT_SIZE) &&
           stream_read_bytes(s, od->zkproof, GROTH_PROOF_SIZE);
}

bool js_description_serialize(const struct js_description *jsd,
                               struct byte_stream *s)
{
    if (!stream_write_i64_le(s, jsd->vpub_old)) return false;
    if (!stream_write_i64_le(s, jsd->vpub_new)) return false;
    if (!stream_write_bytes(s, jsd->anchor.data, 32)) return false;
    for (int i = 0; i < ZC_NUM_JS_INPUTS; i++)
        if (!stream_write_bytes(s, jsd->nullifiers[i].data, 32)) return false;
    for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++)
        if (!stream_write_bytes(s, jsd->commitments[i].data, 32)) return false;
    if (!stream_write_bytes(s, jsd->ephemeral_key.data, 32)) return false;
    if (!stream_write_bytes(s, jsd->random_seed.data, 32)) return false;
    for (int i = 0; i < ZC_NUM_JS_INPUTS; i++)
        if (!stream_write_bytes(s, jsd->macs[i].data, 32)) return false;

    size_t proof_size = jsd->use_groth ? GROTH_PROOF_SIZE : PHGR_PROOF_SIZE;
    if (!stream_write_bytes(s, jsd->proof, proof_size)) return false;

    for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++)
        if (!stream_write_bytes(s, jsd->ciphertexts[i], ZC_SPROUT_CIPHERTEXT_SIZE))
            return false;
    return true;
}

bool js_description_deserialize(struct js_description *jsd, bool use_groth,
                                 struct byte_stream *s)
{
    jsd->use_groth = use_groth;
    if (!stream_read_i64_le(s, &jsd->vpub_old)) return false;
    if (!stream_read_i64_le(s, &jsd->vpub_new)) return false;
    if (!stream_read_bytes(s, jsd->anchor.data, 32)) return false;
    for (int i = 0; i < ZC_NUM_JS_INPUTS; i++)
        if (!stream_read_bytes(s, jsd->nullifiers[i].data, 32)) return false;
    for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++)
        if (!stream_read_bytes(s, jsd->commitments[i].data, 32)) return false;
    if (!stream_read_bytes(s, jsd->ephemeral_key.data, 32)) return false;
    if (!stream_read_bytes(s, jsd->random_seed.data, 32)) return false;
    for (int i = 0; i < ZC_NUM_JS_INPUTS; i++)
        if (!stream_read_bytes(s, jsd->macs[i].data, 32)) return false;

    size_t proof_size = use_groth ? GROTH_PROOF_SIZE : PHGR_PROOF_SIZE;
    memset(jsd->proof, 0, PHGR_PROOF_SIZE);
    if (!stream_read_bytes(s, jsd->proof, proof_size)) return false;

    for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++)
        if (!stream_read_bytes(s, jsd->ciphertexts[i], ZC_SPROUT_CIPHERTEXT_SIZE))
            return false;
    return true;
}

void transaction_compute_hash(struct transaction *tx)
{
    struct byte_stream s;
    stream_init(&s, 4096);
    transaction_serialize(tx, &s);
    hash256(s.data, s.size, tx->hash.data);
    stream_free(&s);
}

bool transaction_serialize(const struct transaction *tx, struct byte_stream *s)
{
    uint32_t header = (uint32_t)tx->version;
    if (tx->overwintered) header |= (1u << 31);
    if (!stream_write_u32_le(s, header)) return false;

    if (tx->overwintered)
        if (!stream_write_u32_le(s, tx->version_group_id)) return false;

    bool is_overwinter_v3 = tx->overwintered &&
        tx->version_group_id == OVERWINTER_VERSION_GROUP_ID &&
        tx->version == OVERWINTER_TX_VERSION;
    bool is_sapling_v4 = tx->overwintered &&
        tx->version_group_id == SAPLING_VERSION_GROUP_ID &&
        tx->version == SAPLING_TX_VERSION;

    if (!stream_write_compact_size(s, tx->num_vin)) return false;
    for (size_t i = 0; i < tx->num_vin; i++)
        if (!tx_in_serialize(&tx->vin[i], s)) return false;

    if (!stream_write_compact_size(s, tx->num_vout)) return false;
    for (size_t i = 0; i < tx->num_vout; i++)
        if (!tx_out_serialize(&tx->vout[i], s)) return false;

    if (!stream_write_u32_le(s, tx->lock_time)) return false;

    if (is_overwinter_v3 || is_sapling_v4)
        if (!stream_write_u32_le(s, tx->expiry_height)) return false;

    if (is_sapling_v4) {
        if (!stream_write_i64_le(s, tx->value_balance)) return false;

        if (!stream_write_compact_size(s, tx->num_shielded_spend)) return false;
        for (size_t i = 0; i < tx->num_shielded_spend; i++)
            if (!spend_description_serialize(&tx->v_shielded_spend[i], s))
                return false;

        if (!stream_write_compact_size(s, tx->num_shielded_output)) return false;
        for (size_t i = 0; i < tx->num_shielded_output; i++)
            if (!output_description_serialize(&tx->v_shielded_output[i], s))
                return false;
    }

    if (tx->version >= 2) {
        if (!stream_write_compact_size(s, tx->num_joinsplit)) return false;
        bool use_groth = tx->overwintered && tx->version >= SAPLING_TX_VERSION;
        for (size_t i = 0; i < tx->num_joinsplit; i++)
            if (!js_description_serialize(&tx->v_joinsplit[i], s))
                return false;
        (void)use_groth;

        if (tx->num_joinsplit > 0) {
            if (!stream_write_bytes(s, tx->joinsplit_pubkey.data, 32))
                return false;
            if (!stream_write_bytes(s, tx->joinsplit_sig, 64))
                return false;
        }
    }

    if (is_sapling_v4 &&
        (tx->num_shielded_spend > 0 || tx->num_shielded_output > 0)) {
        if (!stream_write_bytes(s, tx->binding_sig, 64))
            return false;
    }

    return true;
}

bool transaction_deserialize(struct transaction *tx, struct byte_stream *s)
{
    transaction_init(tx);

    uint32_t header;
    if (!stream_read_u32_le(s, &header)) return false;
    tx->overwintered = (header >> 31) != 0;
    tx->version = (int32_t)(header & 0x7FFFFFFF);

    if (tx->overwintered) {
        if (!stream_read_u32_le(s, &tx->version_group_id)) return false;
    }

    bool is_overwinter_v3 = tx->overwintered &&
        tx->version_group_id == OVERWINTER_VERSION_GROUP_ID &&
        tx->version == OVERWINTER_TX_VERSION;
    bool is_sapling_v4 = tx->overwintered &&
        tx->version_group_id == SAPLING_VERSION_GROUP_ID &&
        tx->version == SAPLING_TX_VERSION;

    if (tx->overwintered && !(is_overwinter_v3 || is_sapling_v4))
        return false;

    uint64_t num_vin;
    if (!stream_read_compact_size(s, &num_vin)) return false;
    if (num_vin > MAX_TX_INPUTS) return false;
    if (!transaction_alloc(tx, (size_t)num_vin, 0)) return false;

    for (size_t i = 0; i < tx->num_vin; i++)
        if (!tx_in_deserialize(&tx->vin[i], s)) return false;

    uint64_t num_vout;
    if (!stream_read_compact_size(s, &num_vout)) return false;
    if (num_vout > MAX_TX_OUTPUTS) return false;
    tx->vout = calloc((size_t)num_vout, sizeof(struct tx_out));
    if (num_vout > 0 && !tx->vout) return false;
    tx->num_vout = (size_t)num_vout;
    for (size_t i = 0; i < tx->num_vout; i++) {
        tx_out_set_null(&tx->vout[i]);
        if (!tx_out_deserialize(&tx->vout[i], s)) return false;
    }

    if (!stream_read_u32_le(s, &tx->lock_time)) return false;

    if (is_overwinter_v3 || is_sapling_v4) {
        if (!stream_read_u32_le(s, &tx->expiry_height)) return false;
    }

    if (is_sapling_v4) {
        if (!stream_read_i64_le(s, &tx->value_balance)) return false;

        uint64_t num_spend;
        if (!stream_read_compact_size(s, &num_spend)) return false;
        if (num_spend > MAX_SHIELDED_SPENDS) return false;
        if (num_spend > 0) {
            tx->v_shielded_spend = calloc((size_t)num_spend,
                                           sizeof(struct spend_description));
            if (!tx->v_shielded_spend) return false;
            tx->num_shielded_spend = (size_t)num_spend;
            for (size_t i = 0; i < tx->num_shielded_spend; i++)
                if (!spend_description_deserialize(&tx->v_shielded_spend[i], s))
                    return false;
        }

        uint64_t num_output;
        if (!stream_read_compact_size(s, &num_output)) return false;
        if (num_output > MAX_SHIELDED_OUTPUTS) return false;
        if (num_output > 0) {
            tx->v_shielded_output = calloc((size_t)num_output,
                                            sizeof(struct output_description));
            if (!tx->v_shielded_output) return false;
            tx->num_shielded_output = (size_t)num_output;
            for (size_t i = 0; i < tx->num_shielded_output; i++)
                if (!output_description_deserialize(&tx->v_shielded_output[i], s))
                    return false;
        }
    }

    if (tx->version >= 2) {
        bool use_groth = tx->overwintered && tx->version >= SAPLING_TX_VERSION;
        uint64_t num_js;
        if (!stream_read_compact_size(s, &num_js)) return false;
        if (num_js > MAX_JOINSPLITS) return false;
        if (num_js > 0) {
            tx->v_joinsplit = calloc((size_t)num_js,
                                      sizeof(struct js_description));
            if (!tx->v_joinsplit) return false;
            tx->num_joinsplit = (size_t)num_js;
            for (size_t i = 0; i < tx->num_joinsplit; i++)
                if (!js_description_deserialize(&tx->v_joinsplit[i],
                                                 use_groth, s))
                    return false;

            if (!stream_read_bytes(s, tx->joinsplit_pubkey.data, 32))
                return false;
            if (!stream_read_bytes(s, tx->joinsplit_sig, 64))
                return false;
        }
    }

    if (is_sapling_v4 &&
        (tx->num_shielded_spend > 0 || tx->num_shielded_output > 0)) {
        if (!stream_read_bytes(s, tx->binding_sig, 64))
            return false;
    }

    transaction_compute_hash(tx);
    return true;
}

size_t transaction_serialize_size(const struct transaction *tx)
{
    struct byte_stream s;
    stream_init(&s, 4096);
    transaction_serialize(tx, &s);
    size_t result = s.size;
    stream_free(&s);
    return result;
}
