/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "script/standard_c.h"
#include "core/hash.h"
#include <string.h>

const char *get_txn_output_type(enum txnouttype t)
{
    switch (t) {
    case TX_NONSTANDARD: return "nonstandard";
    case TX_PUBKEY:      return "pubkey";
    case TX_PUBKEYHASH:  return "pubkeyhash";
    case TX_SCRIPTHASH:  return "scripthash";
    case TX_MULTISIG:    return "multisig";
    case TX_NULL_DATA:   return "nulldata";
    }
    return NULL;
}

void script_id_from_script(struct script_id *out, const struct script *s)
{
    hash160(s->data, s->size, out->hash.data);
}

bool script_solver(const struct script *s, enum txnouttype *type_out,
                   unsigned char solutions[][65], size_t solution_sizes[],
                   size_t *num_solutions)
{
    *num_solutions = 0;

    /* P2SH: OP_HASH160 <20> <hash> OP_EQUAL */
    if (s->size == 23 && s->data[0] == OP_HASH160 && s->data[1] == 20 &&
        s->data[22] == OP_EQUAL) {
        *type_out = TX_SCRIPTHASH;
        memcpy(solutions[0], s->data + 2, 20);
        solution_sizes[0] = 20;
        *num_solutions = 1;
        return true;
    }

    /* OP_RETURN (null data) */
    if (s->size >= 1 && s->data[0] == OP_RETURN) {
        size_t i = 1;
        while (i < s->size) {
            unsigned char op = s->data[i];
            if (op <= 0x4e) {
                size_t push_len = 0;
                if (op <= 75) {
                    push_len = op;
                    i++;
                } else if (op == OP_PUSHDATA1 && i + 1 < s->size) {
                    push_len = s->data[i + 1];
                    i += 2;
                } else if (op == OP_PUSHDATA2 && i + 2 < s->size) {
                    push_len = s->data[i + 1] | ((size_t)s->data[i + 2] << 8);
                    i += 3;
                } else if (op == OP_PUSHDATA4 && i + 4 < s->size) {
                    push_len = s->data[i + 1] | ((size_t)s->data[i + 2] << 8) |
                               ((size_t)s->data[i + 3] << 16) | ((size_t)s->data[i + 4] << 24);
                    i += 5;
                } else {
                    goto not_null_data;
                }
                i += push_len;
            } else {
                goto not_null_data;
            }
        }
        *type_out = TX_NULL_DATA;
        return true;
    }
not_null_data:

    /* P2PKH: OP_DUP OP_HASH160 <20> <hash> OP_EQUALVERIFY OP_CHECKSIG */
    if (s->size == 25 && s->data[0] == OP_DUP && s->data[1] == OP_HASH160 &&
        s->data[2] == 20 && s->data[23] == OP_EQUALVERIFY &&
        s->data[24] == OP_CHECKSIG) {
        *type_out = TX_PUBKEYHASH;
        memcpy(solutions[0], s->data + 3, 20);
        solution_sizes[0] = 20;
        *num_solutions = 1;
        return true;
    }

    /* P2PK compressed: <33> <compressed pubkey> OP_CHECKSIG */
    if (s->size == 35 && s->data[0] == 33 && s->data[34] == OP_CHECKSIG &&
        (s->data[1] == 0x02 || s->data[1] == 0x03)) {
        *type_out = TX_PUBKEY;
        memcpy(solutions[0], s->data + 1, 33);
        solution_sizes[0] = 33;
        *num_solutions = 1;
        return true;
    }

    /* P2PK uncompressed: <65> <04 pubkey> OP_CHECKSIG */
    if (s->size == 67 && s->data[0] == 65 && s->data[66] == OP_CHECKSIG &&
        s->data[1] == 0x04) {
        *type_out = TX_PUBKEY;
        memcpy(solutions[0], s->data + 1, 65);
        solution_sizes[0] = 65;
        *num_solutions = 1;
        return true;
    }

    /* Multisig: OP_m <pubkeys...> OP_n OP_CHECKMULTISIG */
    if (s->size >= 4 && s->data[s->size - 1] == OP_CHECKMULTISIG) {
        unsigned char last_op = s->data[s->size - 2];
        if (last_op >= OP_1 && last_op <= OP_16) {
            int n = last_op - (OP_1 - 1);
            unsigned char first_op = s->data[0];
            if (first_op >= OP_1 && first_op <= OP_16) {
                int m = first_op - (OP_1 - 1);
                if (m >= 1 && n >= 1 && m <= n) {
                    size_t pos = 1;
                    int key_count = 0;
                    solutions[0][0] = (unsigned char)m;
                    solution_sizes[0] = 1;
                    *num_solutions = 1;

                    while (key_count < n && pos < s->size - 2) {
                        unsigned char klen = s->data[pos];
                        if (klen != 33 && klen != 65) break;
                        pos++;
                        if (pos + klen > s->size - 2) break;
                        memcpy(solutions[*num_solutions], s->data + pos, klen);
                        solution_sizes[*num_solutions] = klen;
                        (*num_solutions)++;
                        pos += klen;
                        key_count++;
                    }

                    if (key_count == n && pos == s->size - 2) {
                        solutions[*num_solutions][0] = (unsigned char)n;
                        solution_sizes[*num_solutions] = 1;
                        (*num_solutions)++;
                        *type_out = TX_MULTISIG;
                        return true;
                    }
                    *num_solutions = 0;
                }
            }
        }
    }

    *type_out = TX_NONSTANDARD;
    return false;
}

int script_sig_args_expected(enum txnouttype t,
                             const unsigned char solutions[][65],
                             const size_t solution_sizes[],
                             size_t num_solutions)
{
    (void)solution_sizes;
    switch (t) {
    case TX_NONSTANDARD:
    case TX_NULL_DATA:
        return -1;
    case TX_PUBKEY:
        return 1;
    case TX_PUBKEYHASH:
        return 2;
    case TX_MULTISIG:
        if (num_solutions < 1 || solution_sizes[0] < 1)
            return -1;
        return solutions[0][0] + 1;
    case TX_SCRIPTHASH:
        return 1;
    }
    return -1;
}

bool script_extract_destination(const struct script *s,
                                struct tx_destination *dest_out)
{
    enum txnouttype type;
    unsigned char solutions[20][65];
    size_t solution_sizes[20];
    size_t num_solutions;

    if (!script_solver(s, &type, solutions, solution_sizes, &num_solutions))
        return false;

    if (type == TX_PUBKEY) {
        struct pubkey pk;
        pubkey_set(&pk, solutions[0], solution_sizes[0]);
        if (!pubkey_is_valid(&pk))
            return false;
        dest_out->type = DEST_KEY_ID;
        dest_out->id.key = pubkey_get_id(&pk);
        return true;
    }
    if (type == TX_PUBKEYHASH) {
        dest_out->type = DEST_KEY_ID;
        memcpy(dest_out->id.key.id.data, solutions[0], 20);
        return true;
    }
    if (type == TX_SCRIPTHASH) {
        dest_out->type = DEST_SCRIPT_ID;
        memcpy(dest_out->id.script.hash.data, solutions[0], 20);
        return true;
    }
    return false;
}

bool tx_destination_is_valid(const struct tx_destination *dest)
{
    return dest->type != DEST_NONE;
}

void script_for_p2pkh(struct script *out, const struct key_id *key)
{
    out->size = 25;
    out->data[0] = OP_DUP;
    out->data[1] = OP_HASH160;
    out->data[2] = 20;
    memcpy(out->data + 3, key->id.data, 20);
    out->data[23] = OP_EQUALVERIFY;
    out->data[24] = OP_CHECKSIG;
}

void script_for_p2sh(struct script *out, const struct script_id *script_hash)
{
    out->size = 23;
    out->data[0] = OP_HASH160;
    out->data[1] = 20;
    memcpy(out->data + 2, script_hash->hash.data, 20);
    out->data[22] = OP_EQUAL;
}

void script_for_multisig(struct script *out, int n_required,
                         const struct pubkey *keys, size_t num_keys)
{
    out->size = 0;
    out->data[out->size++] = (unsigned char)(OP_1 + n_required - 1);

    for (size_t i = 0; i < num_keys; i++) {
        unsigned char klen = (unsigned char)keys[i].size;
        out->data[out->size++] = klen;
        memcpy(out->data + out->size, keys[i].vch, klen);
        out->size += klen;
    }

    out->data[out->size++] = (unsigned char)(OP_1 + (int)num_keys - 1);
    out->data[out->size++] = OP_CHECKMULTISIG;
}

void script_for_destination(struct script *out, const struct tx_destination *dest)
{
    switch (dest->type) {
    case DEST_KEY_ID:
        script_for_p2pkh(out, &dest->id.key);
        break;
    case DEST_SCRIPT_ID:
        script_for_p2sh(out, &dest->id.script);
        break;
    case DEST_NONE:
        out->size = 0;
        break;
    }
}
