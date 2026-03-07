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

#define FIRST_UNDEFINED_OP_VALUE 0xbc

static inline bool script_is_unspendable(const struct script *s)
{
    return s->size > 0 && s->data[0] == OP_RETURN;
}

static inline bool script_is_push_only(const struct script *s)
{
    size_t i = 0;
    while (i < s->size) {
        unsigned char op = s->data[i];
        if (op > OP_16)
            return false;
        if (op < OP_PUSHDATA1) {
            i += 1 + op;
        } else if (op == OP_PUSHDATA1) {
            if (i + 1 >= s->size) return false;
            i += 2 + s->data[i + 1];
        } else if (op == OP_PUSHDATA2) {
            if (i + 2 >= s->size) return false;
            uint16_t len = (uint16_t)s->data[i+1] | ((uint16_t)s->data[i+2] << 8);
            i += 3 + len;
        } else if (op == OP_PUSHDATA4) {
            if (i + 4 >= s->size) return false;
            uint32_t len = (uint32_t)s->data[i+1] | ((uint32_t)s->data[i+2] << 8) |
                           ((uint32_t)s->data[i+3] << 16) | ((uint32_t)s->data[i+4] << 24);
            i += 5 + len;
        } else {
            i++;
        }
    }
    return true;
}

static inline bool script_get_op(const struct script *s, size_t *pc,
                                 enum opcodetype *opcode,
                                 unsigned char *data, size_t *datalen)
{
    if (*pc >= s->size)
        return false;
    unsigned char op = s->data[*pc];
    (*pc)++;
    *opcode = (enum opcodetype)op;
    if (datalen) *datalen = 0;

    if (op <= OP_PUSHDATA4) {
        size_t nsize = 0;
        if (op < OP_PUSHDATA1) {
            nsize = op;
        } else if (op == OP_PUSHDATA1) {
            if (*pc >= s->size) return false;
            nsize = s->data[*pc]; (*pc)++;
        } else if (op == OP_PUSHDATA2) {
            if (*pc + 1 >= s->size) return false;
            nsize = (size_t)s->data[*pc] | ((size_t)s->data[*pc+1] << 8);
            *pc += 2;
        } else if (op == OP_PUSHDATA4) {
            if (*pc + 3 >= s->size) return false;
            nsize = (size_t)s->data[*pc] | ((size_t)s->data[*pc+1] << 8) |
                    ((size_t)s->data[*pc+2] << 16) | ((size_t)s->data[*pc+3] << 24);
            *pc += 4;
        }
        if (*pc + nsize > s->size) return false;
        if (data && datalen) {
            memcpy(data, s->data + *pc, nsize);
            *datalen = nsize;
        }
        *pc += nsize;
    }
    return true;
}

#define SCRIPT_NUM_DEFAULT_MAX_SIZE 4
#define SCRIPT_NUM_MAX_SIZE 8

struct script_num {
    int64_t value;
};

static inline struct script_num script_num_from_int(int64_t n)
{
    return (struct script_num){n};
}

static inline bool script_num_from_bytes(struct script_num *out,
                                         const unsigned char *data, size_t len,
                                         bool require_minimal,
                                         size_t max_size)
{
    if (len > max_size)
        return false;
    if (require_minimal && len > 0) {
        if ((data[len - 1] & 0x7f) == 0) {
            if (len <= 1 || (data[len - 2] & 0x80) == 0)
                return false;
        }
    }
    if (len == 0) { out->value = 0; return true; }
    int64_t result = 0;
    for (size_t i = 0; i < len; i++)
        result |= (int64_t)data[i] << (8 * i);
    if (data[len - 1] & 0x80)
        result = -(int64_t)(result & ~((int64_t)0x80 << (8 * (len - 1))));
    out->value = result;
    return true;
}

static inline size_t script_num_serialize(const struct script_num *sn,
                                          unsigned char *out, size_t outsize)
{
    int64_t value = sn->value;
    if (value == 0) return 0;
    bool neg = value < 0;
    uint64_t absval = neg ? (uint64_t)(-value) : (uint64_t)value;
    size_t len = 0;
    while (absval && len < outsize) {
        out[len++] = (unsigned char)(absval & 0xff);
        absval >>= 8;
    }
    if (len < outsize) {
        if (out[len - 1] & 0x80)
            out[len++] = neg ? 0x80 : 0x00;
        else if (neg)
            out[len - 1] |= 0x80;
    }
    return len;
}

static inline int script_num_get_int(const struct script_num *sn)
{
    if (sn->value > INT32_MAX) return INT32_MAX;
    if (sn->value < INT32_MIN) return INT32_MIN;
    return (int)sn->value;
}

const char *script_get_op_name(enum opcodetype opcode);

#endif

