/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Zcash note types — Sprout and Sapling. */

#ifndef ZCL_ZCASH_NOTE_H
#define ZCL_ZCASH_NOTE_H

#include "core/uint256.h"
#include "zcash/zcash.h"
#include "zcash/address.h"
#include <stdbool.h>
#include <stdint.h>

struct sprout_note {
    struct uint256 a_pk;
    uint64_t value;
    struct uint256 rho;
    struct uint256 r;
};

struct sapling_note {
    unsigned char d[ZC_DIVERSIFIER_SIZE];
    struct uint256 pk_d;
    uint64_t value;
    struct uint256 r;
};

struct sprout_note_plaintext {
    uint64_t value;
    struct uint256 rho;
    struct uint256 r;
    unsigned char memo[ZC_MEMO_SIZE];
};

struct sapling_note_plaintext {
    unsigned char d[ZC_DIVERSIFIER_SIZE];
    uint64_t value;
    struct uint256 rcm;
    unsigned char memo[ZC_MEMO_SIZE];
};

struct sapling_outgoing_plaintext {
    struct uint256 pk_d;
    struct uint256 esk;
};

void sprout_note_cm(const struct sprout_note *note, struct uint256 *out);
void sprout_note_nullifier(const struct sprout_note *note,
                            const struct sprout_spending_key *a_sk,
                            struct uint256 *out);

struct byte_stream;
bool sprout_note_plaintext_serialize(const struct sprout_note_plaintext *np,
                                      struct byte_stream *s);
bool sprout_note_plaintext_deserialize(struct sprout_note_plaintext *np,
                                        struct byte_stream *s);
bool sapling_note_plaintext_serialize(const struct sapling_note_plaintext *np,
                                       struct byte_stream *s);
bool sapling_note_plaintext_deserialize(struct sapling_note_plaintext *np,
                                         struct byte_stream *s);

#endif
