/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Zcash note types — Sprout and Sapling. */

#include "sapling/note.h"
#include "sapling/prf.h"
#include "crypto/sha256.h"
#include "core/serialize.h"
#include <string.h>

void sprout_note_cm(const struct sprout_note *note, struct uint256 *out)
{
    unsigned char discriminant = 0xb0;
    struct sha256_ctx hasher;
    sha256_init(&hasher);
    sha256_write(&hasher, &discriminant, 1);
    sha256_write(&hasher, note->a_pk.data, 32);

    unsigned char value_le[8];
    for (int i = 0; i < 8; i++)
        value_le[i] = (unsigned char)(note->value >> (8 * i));
    sha256_write(&hasher, value_le, 8);

    sha256_write(&hasher, note->rho.data, 32);
    sha256_write(&hasher, note->r.data, 32);
    sha256_finalize(&hasher, out->data);
}

void sprout_note_nullifier(const struct sprout_note *note,
                            const struct sprout_spending_key *a_sk,
                            struct uint256 *out)
{
    prf_nf(a_sk->data, &note->rho, out);
}

bool sprout_note_plaintext_serialize(const struct sprout_note_plaintext *np,
                                      struct byte_stream *s)
{
    unsigned char leading = 0x00;
    if (!stream_write_bytes(s, &leading, 1)) return false;

    unsigned char value_le[8];
    for (int i = 0; i < 8; i++)
        value_le[i] = (unsigned char)(np->value >> (8 * i));
    if (!stream_write_bytes(s, value_le, 8)) return false;
    if (!stream_write_bytes(s, np->rho.data, 32)) return false;
    if (!stream_write_bytes(s, np->r.data, 32)) return false;
    if (!stream_write_bytes(s, np->memo, ZC_MEMO_SIZE)) return false;
    return true;
}

bool sprout_note_plaintext_deserialize(struct sprout_note_plaintext *np,
                                        struct byte_stream *s)
{
    unsigned char leading;
    if (!stream_read_bytes(s, &leading, 1)) return false;
    if (leading != 0x00) return false;

    unsigned char value_le[8];
    if (!stream_read_bytes(s, value_le, 8)) return false;
    np->value = 0;
    for (int i = 0; i < 8; i++)
        np->value |= (uint64_t)value_le[i] << (8 * i);
    if (!stream_read_bytes(s, np->rho.data, 32)) return false;
    if (!stream_read_bytes(s, np->r.data, 32)) return false;
    if (!stream_read_bytes(s, np->memo, ZC_MEMO_SIZE)) return false;
    return true;
}

bool sapling_note_plaintext_serialize(const struct sapling_note_plaintext *np,
                                       struct byte_stream *s)
{
    unsigned char leading = 0x01;
    if (!stream_write_bytes(s, &leading, 1)) return false;
    if (!stream_write_bytes(s, np->d, ZC_DIVERSIFIER_SIZE)) return false;

    unsigned char value_le[8];
    for (int i = 0; i < 8; i++)
        value_le[i] = (unsigned char)(np->value >> (8 * i));
    if (!stream_write_bytes(s, value_le, 8)) return false;
    if (!stream_write_bytes(s, np->rcm.data, 32)) return false;
    if (!stream_write_bytes(s, np->memo, ZC_MEMO_SIZE)) return false;
    return true;
}

bool sapling_note_plaintext_deserialize(struct sapling_note_plaintext *np,
                                         struct byte_stream *s)
{
    unsigned char leading;
    if (!stream_read_bytes(s, &leading, 1)) return false;
    if (leading != 0x01) return false;

    if (!stream_read_bytes(s, np->d, ZC_DIVERSIFIER_SIZE)) return false;

    unsigned char value_le[8];
    if (!stream_read_bytes(s, value_le, 8)) return false;
    np->value = 0;
    for (int i = 0; i < 8; i++)
        np->value |= (uint64_t)value_le[i] << (8 * i);
    if (!stream_read_bytes(s, np->rcm.data, 32)) return false;
    if (!stream_read_bytes(s, np->memo, ZC_MEMO_SIZE)) return false;
    return true;
}
