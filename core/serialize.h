/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SERIALIZE_H
#define ZCL_SERIALIZE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum ser_type {
    SER_NETWORK = (1 << 0),
    SER_DISK    = (1 << 1),
    SER_GETHASH = (1 << 2)
};

struct byte_stream {
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t read_pos;
    bool error;
    bool owns_data;
};

void stream_init(struct byte_stream *s, size_t initial_capacity);
void stream_init_from_data(struct byte_stream *s, const unsigned char *data,
                           size_t len);
void stream_free(struct byte_stream *s);

bool stream_write(struct byte_stream *s, const void *buf, size_t len);
bool stream_read(struct byte_stream *s, void *buf, size_t len);
size_t stream_remaining(const struct byte_stream *s);

bool stream_write_u8(struct byte_stream *s, uint8_t v);
bool stream_write_u16_le(struct byte_stream *s, uint16_t v);
bool stream_write_u32_le(struct byte_stream *s, uint32_t v);
bool stream_write_u64_le(struct byte_stream *s, uint64_t v);
bool stream_write_i32_le(struct byte_stream *s, int32_t v);
bool stream_write_i64_le(struct byte_stream *s, int64_t v);

bool stream_read_u8(struct byte_stream *s, uint8_t *v);
bool stream_read_u16_le(struct byte_stream *s, uint16_t *v);
bool stream_read_u32_le(struct byte_stream *s, uint32_t *v);
bool stream_read_u64_le(struct byte_stream *s, uint64_t *v);
bool stream_read_i32_le(struct byte_stream *s, int32_t *v);
bool stream_read_i64_le(struct byte_stream *s, int64_t *v);

bool stream_write_compact_size(struct byte_stream *s, uint64_t size);
bool stream_read_compact_size(struct byte_stream *s, uint64_t *size);

static inline size_t compact_size_sizeof(uint64_t n)
{
    if (n < 253)         return 1;
    else if (n <= 0xffff) return 3;
    else if (n <= 0xffffffffULL) return 5;
    else                  return 9;
}

bool stream_write_varint(struct byte_stream *s, uint64_t n);
bool stream_read_varint(struct byte_stream *s, uint64_t *n);

bool stream_write_bytes(struct byte_stream *s, const unsigned char *data, size_t len);
bool stream_read_bytes(struct byte_stream *s, unsigned char *data, size_t len);

#endif
