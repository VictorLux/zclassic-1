/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_PRIMITIVES_TRANSACTION_H
#define ZCL_PRIMITIVES_TRANSACTION_H

#include "core/amount.h"
#include "script/script.h"
#include "core/uint256.h"
#include "zcash/zcash.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define OVERWINTER_TX_VERSION 3
#define SAPLING_TX_VERSION 4
#define OVERWINTER_VERSION_GROUP_ID 0x03C48270U
#define SAPLING_VERSION_GROUP_ID 0x892F2085U

struct outpoint {
    struct uint256 hash;
    uint32_t n;
};

static inline void outpoint_set_null(struct outpoint *op)
{
    uint256_set_null(&op->hash);
    op->n = UINT32_MAX;
}

static inline bool outpoint_is_null(const struct outpoint *op)
{
    return uint256_is_null(&op->hash) && op->n == UINT32_MAX;
}

static inline int outpoint_cmp(const struct outpoint *a, const struct outpoint *b)
{
    int r = uint256_cmp(&a->hash, &b->hash);
    if (r != 0) return r;
    if (a->n < b->n) return -1;
    if (a->n > b->n) return 1;
    return 0;
}

struct tx_in {
    struct outpoint prevout;
    struct script script_sig;
    uint32_t sequence;
};

static inline void tx_in_init(struct tx_in *in)
{
    outpoint_set_null(&in->prevout);
    in->script_sig.size = 0;
    in->sequence = UINT32_MAX;
}

static inline bool tx_in_is_final(const struct tx_in *in)
{
    return in->sequence == UINT32_MAX;
}

struct tx_out {
    int64_t value;
    struct script script_pub_key;
};

static inline void tx_out_set_null(struct tx_out *out)
{
    out->value = -1;
    out->script_pub_key.size = 0;
}

static inline bool tx_out_is_null(const struct tx_out *out)
{
    return out->value == -1;
}

struct spend_description {
    struct uint256 cv;
    struct uint256 anchor;
    struct uint256 nullifier;
    struct uint256 rk;
    unsigned char zkproof[GROTH_PROOF_SIZE];
    unsigned char spend_auth_sig[64];
};

struct output_description {
    struct uint256 cv;
    struct uint256 cm;
    struct uint256 ephemeral_key;
    unsigned char enc_ciphertext[ZC_SAPLING_ENCCIPHERTEXT_SIZE];
    unsigned char out_ciphertext[ZC_SAPLING_OUTCIPHERTEXT_SIZE];
    unsigned char zkproof[GROTH_PROOF_SIZE];
};

struct js_description {
    int64_t vpub_old;
    int64_t vpub_new;
    struct uint256 anchor;
    struct uint256 nullifiers[ZC_NUM_JS_INPUTS];
    struct uint256 commitments[ZC_NUM_JS_OUTPUTS];
    struct uint256 ephemeral_key;
    struct uint256 random_seed;
    struct uint256 macs[ZC_NUM_JS_INPUTS];
    bool use_groth;
    unsigned char proof[PHGR_PROOF_SIZE];
    unsigned char ciphertexts[ZC_NUM_JS_OUTPUTS][ZC_SPROUT_CIPHERTEXT_SIZE];
};

#define MAX_TX_INPUTS 4096
#define MAX_TX_OUTPUTS 4096
#define MAX_SHIELDED_SPENDS 4096
#define MAX_SHIELDED_OUTPUTS 4096
#define MAX_JOINSPLITS 4096

struct transaction {
    bool overwintered;
    int32_t version;
    uint32_t version_group_id;
    struct tx_in *vin;
    size_t num_vin;
    struct tx_out *vout;
    size_t num_vout;
    uint32_t lock_time;
    uint32_t expiry_height;
    int64_t value_balance;
    struct spend_description *v_shielded_spend;
    size_t num_shielded_spend;
    struct output_description *v_shielded_output;
    size_t num_shielded_output;
    struct js_description *v_joinsplit;
    size_t num_joinsplit;
    struct uint256 joinsplit_pubkey;
    unsigned char joinsplit_sig[64];
    unsigned char binding_sig[64];
    struct uint256 hash;
};

static inline bool transaction_is_coinbase(const struct transaction *tx)
{
    return tx->num_vin == 1 && outpoint_is_null(&tx->vin[0].prevout);
}

void transaction_init(struct transaction *tx);
void transaction_free(struct transaction *tx);
bool transaction_alloc(struct transaction *tx, size_t num_vin, size_t num_vout);
bool transaction_copy(struct transaction *dst, const struct transaction *src);
int64_t transaction_get_value_out(const struct transaction *tx);
void outpoint_to_string(const struct outpoint *op, char *buf, size_t buflen);

struct byte_stream;

bool outpoint_serialize(const struct outpoint *op, struct byte_stream *s);
bool outpoint_deserialize(struct outpoint *op, struct byte_stream *s);
bool tx_in_serialize(const struct tx_in *in, struct byte_stream *s);
bool tx_in_deserialize(struct tx_in *in, struct byte_stream *s);
bool tx_out_serialize(const struct tx_out *out, struct byte_stream *s);
bool tx_out_deserialize(struct tx_out *out, struct byte_stream *s);
void transaction_compute_hash(struct transaction *tx);

bool spend_description_serialize(const struct spend_description *sd, struct byte_stream *s);
bool spend_description_deserialize(struct spend_description *sd, struct byte_stream *s);
bool output_description_serialize(const struct output_description *od, struct byte_stream *s);
bool output_description_deserialize(struct output_description *od, struct byte_stream *s);
bool js_description_serialize(const struct js_description *jsd, struct byte_stream *s);
bool js_description_deserialize(struct js_description *jsd, bool use_groth, struct byte_stream *s);

bool transaction_serialize(const struct transaction *tx, struct byte_stream *s);
bool transaction_deserialize(struct transaction *tx, struct byte_stream *s);
size_t transaction_serialize_size(const struct transaction *tx);

int64_t transaction_get_shielded_value_in(const struct transaction *tx);

#endif
