/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SCRIPT_SCRIPT_C_H
#define ZCL_SCRIPT_SCRIPT_C_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_SCRIPT_ELEMENT_SIZE 520
#define MAX_SCRIPT_SIZE 10000
#define LOCKTIME_THRESHOLD 500000000

enum opcodetype {
    OP_0 = 0x00,
    OP_PUSHDATA1 = 0x4c, OP_PUSHDATA2 = 0x4d, OP_PUSHDATA4 = 0x4e,
    OP_1NEGATE = 0x4f, OP_RESERVED = 0x50,
    OP_1 = 0x51, OP_2, OP_3, OP_4, OP_5, OP_6, OP_7, OP_8,
    OP_9, OP_10, OP_11, OP_12, OP_13, OP_14, OP_15, OP_16,
    OP_NOP = 0x61, OP_VER, OP_IF, OP_NOTIF, OP_VERIF, OP_VERNOTIF,
    OP_ELSE, OP_ENDIF, OP_VERIFY, OP_RETURN,
    OP_TOALTSTACK = 0x6b, OP_FROMALTSTACK, OP_2DROP, OP_2DUP, OP_3DUP,
    OP_2OVER, OP_2ROT, OP_2SWAP, OP_IFDUP, OP_DEPTH, OP_DROP, OP_DUP,
    OP_NIP, OP_OVER, OP_PICK, OP_ROLL, OP_ROT, OP_SWAP, OP_TUCK,
    OP_CAT = 0x7e, OP_SUBSTR, OP_LEFT, OP_RIGHT, OP_SIZE,
    OP_INVERT = 0x83, OP_AND, OP_OR, OP_XOR,
    OP_EQUAL, OP_EQUALVERIFY, OP_RESERVED1, OP_RESERVED2,
    OP_1ADD = 0x8b, OP_1SUB, OP_2MUL, OP_2DIV,
    OP_NEGATE, OP_ABS, OP_NOT, OP_0NOTEQUAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LSHIFT, OP_RSHIFT,
    OP_BOOLAND, OP_BOOLOR, OP_NUMEQUAL, OP_NUMEQUALVERIFY,
    OP_NUMNOTEQUAL, OP_LESSTHAN, OP_GREATERTHAN,
    OP_LESSTHANOREQUAL, OP_GREATERTHANOREQUAL, OP_MIN, OP_MAX,
    OP_WITHIN = 0xa5,
    OP_RIPEMD160 = 0xa6, OP_SHA1, OP_SHA256, OP_HASH160, OP_HASH256,
    OP_CODESEPARATOR, OP_CHECKSIG, OP_CHECKSIGVERIFY,
    OP_CHECKMULTISIG, OP_CHECKMULTISIGVERIFY,
    OP_NOP1 = 0xb0, OP_NOP2, OP_NOP3, OP_NOP4, OP_NOP5,
    OP_NOP6, OP_NOP7, OP_NOP8, OP_NOP9, OP_NOP10,
    OP_CHECKDATASIG = 0xba, OP_CHECKDATASIGVERIFY,
    OP_SMALLINTEGER = 0xfa, OP_PUBKEYS = 0xfb,
    OP_PUBKEYHASH = 0xfd, OP_PUBKEY = 0xfe,
    OP_INVALIDOPCODE = 0xff,
};

#define OP_FALSE OP_0
#define OP_TRUE OP_1
#define OP_CHECKLOCKTIMEVERIFY OP_NOP2

struct script {
    unsigned char data[MAX_SCRIPT_SIZE];
    size_t size;
};

static inline void script_init(struct script *s) { s->size = 0; }

static inline void script_set(struct script *s,
                              const unsigned char *data, size_t len)
{
    if (len > MAX_SCRIPT_SIZE) len = MAX_SCRIPT_SIZE;
    memcpy(s->data, data, len);
    s->size = len;
}

static inline bool script_push_op(struct script *s, enum opcodetype op)
{
    if (s->size >= MAX_SCRIPT_SIZE) return false;
    s->data[s->size++] = (unsigned char)op;
    return true;
}

static inline bool script_push_data(struct script *s,
                                    const unsigned char *data, size_t len)
{
    if (len < OP_PUSHDATA1) {
        if (s->size + 1 + len > MAX_SCRIPT_SIZE) return false;
        s->data[s->size++] = (unsigned char)len;
    } else if (len <= 0xff) {
        if (s->size + 2 + len > MAX_SCRIPT_SIZE) return false;
        s->data[s->size++] = OP_PUSHDATA1;
        s->data[s->size++] = (unsigned char)len;
    } else if (len <= 0xffff) {
        if (s->size + 3 + len > MAX_SCRIPT_SIZE) return false;
        s->data[s->size++] = OP_PUSHDATA2;
        s->data[s->size++] = (unsigned char)(len & 0xff);
        s->data[s->size++] = (unsigned char)(len >> 8);
    } else {
        if (s->size + 5 + len > MAX_SCRIPT_SIZE) return false;
        s->data[s->size++] = OP_PUSHDATA4;
        s->data[s->size++] = (unsigned char)(len & 0xff);
        s->data[s->size++] = (unsigned char)((len >> 8) & 0xff);
        s->data[s->size++] = (unsigned char)((len >> 16) & 0xff);
        s->data[s->size++] = (unsigned char)((len >> 24) & 0xff);
    }
    memcpy(s->data + s->size, data, len);
    s->size += len;
    return true;
}

static inline bool script_is_p2sh(const struct script *s)
{
    return s->size == 23 &&
           s->data[0] == OP_HASH160 &&
           s->data[1] == 0x14 &&
           s->data[22] == OP_EQUAL;
}

static inline bool script_is_p2pkh(const struct script *s)
{
    return s->size == 25 &&
           s->data[0] == OP_DUP &&
           s->data[1] == OP_HASH160 &&
           s->data[2] == 0x14 &&
           s->data[23] == OP_EQUALVERIFY &&
           s->data[24] == OP_CHECKSIG;
}

static inline int script_decode_op_n(enum opcodetype op)
{
    if (op == OP_0) return 0;
    return (int)(op - (OP_1 - 1));
}

const char *script_get_op_name(enum opcodetype opcode);

#endif
