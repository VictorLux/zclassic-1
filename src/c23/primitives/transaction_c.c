/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "primitives/transaction_c.h"
#include "core/hash.h"
#include "core/serialize_c.h"
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
    uint256_set_null(&tx->hash);
}

void transaction_free(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
    tx->vin = NULL;
    tx->vout = NULL;
    tx->num_vin = 0;
    tx->num_vout = 0;
}

bool transaction_alloc(struct transaction *tx, size_t num_vin, size_t num_vout)
{
    if (num_vin > MAX_TX_INPUTS || num_vout > MAX_TX_OUTPUTS)
        return false;

    tx->vin = calloc(num_vin, sizeof(struct tx_in));
    tx->vout = calloc(num_vout, sizeof(struct tx_out));
    if ((num_vin && !tx->vin) || (num_vout && !tx->vout)) {
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

int64_t transaction_get_value_out(const struct transaction *tx)
{
    int64_t total = 0;
    for (size_t i = 0; i < tx->num_vout; i++) {
        total += tx->vout[i].value;
        if (!MoneyRange(tx->vout[i].value) || !MoneyRange(total))
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

void transaction_compute_hash(struct transaction *tx)
{
    struct byte_stream s;
    stream_init(&s, 512);
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

    if (!stream_write_compact_size(s, tx->num_vin)) return false;
    for (size_t i = 0; i < tx->num_vin; i++)
        if (!tx_in_serialize(&tx->vin[i], s)) return false;

    if (!stream_write_compact_size(s, tx->num_vout)) return false;
    for (size_t i = 0; i < tx->num_vout; i++)
        if (!tx_out_serialize(&tx->vout[i], s)) return false;

    if (!stream_write_u32_le(s, tx->lock_time)) return false;

    if (tx->overwintered)
        if (!stream_write_u32_le(s, tx->expiry_height)) return false;

    return true;
}

bool transaction_deserialize(struct transaction *tx, struct byte_stream *s)
{
    uint32_t header;
    if (!stream_read_u32_le(s, &header)) return false;
    tx->overwintered = (header >> 31) != 0;
    tx->version = (int32_t)(header & 0x7FFFFFFF);

    if (tx->overwintered) {
        if (!stream_read_u32_le(s, &tx->version_group_id)) return false;
    }

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

    if (tx->overwintered) {
        if (!stream_read_u32_le(s, &tx->expiry_height)) return false;
    }

    transaction_compute_hash(tx);
    return true;
}

size_t transaction_serialize_size(const struct transaction *tx)
{
    struct byte_stream s;
    stream_init(&s, 512);
    transaction_serialize(tx, &s);
    size_t result = s.size;
    stream_free(&s);
    return result;
}
