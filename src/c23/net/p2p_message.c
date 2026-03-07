/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/p2p_message.h"
#include <string.h>

bool version_message_serialize(const struct version_message *v,
                               struct byte_stream *s)
{
    if (!stream_write_i32_le(s, v->protocol_version)) return false;
    if (!stream_write_u64_le(s, v->services)) return false;
    if (!stream_write_i64_le(s, v->timestamp)) return false;
    if (!net_address_serialize(&v->addr_recv, s, false)) return false;
    if (!net_address_serialize(&v->addr_from, s, false)) return false;
    if (!stream_write_u64_le(s, v->nonce)) return false;

    size_t subver_len = strlen(v->sub_version);
    if (!stream_write_compact_size(s, subver_len)) return false;
    if (subver_len > 0) {
        if (!stream_write_bytes(s, (const unsigned char *)v->sub_version,
                                subver_len))
            return false;
    }

    if (!stream_write_i32_le(s, v->start_height)) return false;
    uint8_t relay_byte = v->relay ? 1 : 0;
    if (!stream_write_u8(s, relay_byte)) return false;
    return true;
}

bool version_message_deserialize(struct version_message *v,
                                 struct byte_stream *s)
{
    if (!stream_read_i32_le(s, &v->protocol_version)) return false;
    if (!stream_read_u64_le(s, &v->services)) return false;
    if (!stream_read_i64_le(s, &v->timestamp)) return false;
    if (!net_address_deserialize(&v->addr_recv, s, false)) return false;
    if (!net_address_deserialize(&v->addr_from, s, false)) return false;
    if (!stream_read_u64_le(s, &v->nonce)) return false;

    uint64_t subver_len;
    if (!stream_read_compact_size(s, &subver_len)) return false;
    if (subver_len >= MAX_SUBVER_LENGTH) return false;
    if (subver_len > 0) {
        if (!stream_read_bytes(s, (unsigned char *)v->sub_version, subver_len))
            return false;
    }
    v->sub_version[subver_len] = '\0';

    if (!stream_read_i32_le(s, &v->start_height)) return false;

    if (s->read_pos < s->size) {
        uint8_t relay_byte;
        if (!stream_read_u8(s, &relay_byte)) return false;
        v->relay = relay_byte != 0;
    } else {
        v->relay = true;
    }
    return true;
}
