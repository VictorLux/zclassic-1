/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/p2p_message.h"
#include <string.h>
#include "util/log_macros.h"

bool version_message_serialize(const struct version_message *v,
                               struct byte_stream *s)
{
    if (!stream_write_i32_le(s, v->protocol_version)) LOG_FAIL("p2p_msg", "version serialize: write protocol_version failed");
    if (!stream_write_u64_le(s, v->services)) LOG_FAIL("p2p_msg", "version serialize: write services failed");
    if (!stream_write_i64_le(s, v->timestamp)) LOG_FAIL("p2p_msg", "version serialize: write timestamp failed");
    if (!net_address_serialize(&v->addr_recv, s, false)) LOG_FAIL("p2p_msg", "version serialize: write addr_recv failed");
    if (!net_address_serialize(&v->addr_from, s, false)) LOG_FAIL("p2p_msg", "version serialize: write addr_from failed");
    if (!stream_write_u64_le(s, v->nonce)) LOG_FAIL("p2p_msg", "version serialize: write nonce failed");

    size_t subver_len = strlen(v->sub_version);
    if (!stream_write_compact_size(s, subver_len)) LOG_FAIL("p2p_msg", "version serialize: write subver_len failed");
    if (subver_len > 0) {
        if (!stream_write_bytes(s, (const unsigned char *)v->sub_version,
                                subver_len))
            LOG_FAIL("p2p_msg", "version serialize: write sub_version failed (%zu bytes)", subver_len);
    }

    if (!stream_write_i32_le(s, v->start_height)) LOG_FAIL("p2p_msg", "version serialize: write start_height failed");
    uint8_t relay_byte = v->relay ? 1 : 0;
    if (!stream_write_u8(s, relay_byte)) LOG_FAIL("p2p_msg", "version serialize: write relay failed");
    return true;
}

bool version_message_deserialize(struct version_message *v,
                                 struct byte_stream *s)
{
    if (!stream_read_i32_le(s, &v->protocol_version)) LOG_FAIL("p2p_msg", "version deserialize: read protocol_version failed");
    if (!stream_read_u64_le(s, &v->services)) LOG_FAIL("p2p_msg", "version deserialize: read services failed");
    if (!stream_read_i64_le(s, &v->timestamp)) LOG_FAIL("p2p_msg", "version deserialize: read timestamp failed");
    if (!net_address_deserialize(&v->addr_recv, s, false)) LOG_FAIL("p2p_msg", "version deserialize: read addr_recv failed");
    if (!net_address_deserialize(&v->addr_from, s, false)) LOG_FAIL("p2p_msg", "version deserialize: read addr_from failed");
    if (!stream_read_u64_le(s, &v->nonce)) LOG_FAIL("p2p_msg", "version deserialize: read nonce failed");

    uint64_t subver_len;
    if (!stream_read_compact_size(s, &subver_len)) LOG_FAIL("p2p_msg", "version deserialize: read subver_len failed");
    if (subver_len >= MAX_SUBVER_LENGTH) LOG_FAIL("p2p_msg", "version deserialize: subver_len %lu exceeds max %d", (unsigned long)subver_len, MAX_SUBVER_LENGTH);
    if (subver_len > 0) {
        if (!stream_read_bytes(s, (unsigned char *)v->sub_version, subver_len))
            LOG_FAIL("p2p_msg", "version deserialize: read sub_version failed (%lu bytes)", (unsigned long)subver_len);
    }
    v->sub_version[subver_len] = '\0';

    if (!stream_read_i32_le(s, &v->start_height)) LOG_FAIL("p2p_msg", "version deserialize: read start_height failed");

    if (s->read_pos < s->size) {
        uint8_t relay_byte;
        if (!stream_read_u8(s, &relay_byte)) LOG_FAIL("p2p_msg", "version deserialize: read relay byte failed");
        v->relay = relay_byte != 0;
    } else {
        v->relay = true;
    }
    return true;
}

bool getdata_blocks_serialize(struct byte_stream *s,
                              const struct uint256 *hashes,
                              size_t count)
{
    if (!s || (!hashes && count > 0))
        LOG_FAIL("p2p_msg", "getdata_blocks: invalid args (s=%p, hashes=%p, count=%zu)", (void *)s, (const void *)hashes, count);
    if (!stream_write_compact_size(s, (uint64_t)count))
        LOG_FAIL("p2p_msg", "getdata_blocks: write count %zu failed", count);

    for (size_t i = 0; i < count; i++) {
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_BLOCK, &hashes[i]);
        if (!inv_item_serialize(&inv, s))
            LOG_FAIL("p2p_msg", "getdata_blocks: serialize inv item %zu/%zu failed", i, count);
    }

    return true;
}

bool getheaders_serialize(struct byte_stream *s,
                          const struct block_locator *locator,
                          const struct uint256 *stop_hash)
{
    struct uint256 zero;

    if (!s || !locator)
        LOG_FAIL("p2p_msg", "getheaders: invalid args (s=%p, locator=%p)", (void *)s, (const void *)locator);
    if (!block_locator_serialize(locator, s))
        LOG_FAIL("p2p_msg", "getheaders: block_locator_serialize failed");

    if (!stop_hash) {
        uint256_set_null(&zero);
        stop_hash = &zero;
    }
    return stream_write_bytes(s, stop_hash->data, 32);
}
