/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SCRIPT_STANDARD_C_H
#define ZCL_SCRIPT_STANDARD_C_H

#include "pubkey_c.h"
#include "script/script_c.h"
#include "uint256.h"
#include <stdbool.h>
#include <stdint.h>

enum txnouttype {
    TX_NONSTANDARD,
    TX_PUBKEY,
    TX_PUBKEYHASH,
    TX_SCRIPTHASH,
    TX_MULTISIG,
    TX_NULL_DATA
};

#define MAX_OP_RETURN_RELAY 223

struct script_id {
    struct uint160 hash;
};

enum tx_dest_type {
    DEST_NONE,
    DEST_KEY_ID,
    DEST_SCRIPT_ID
};

struct tx_destination {
    enum tx_dest_type type;
    union {
        struct key_id key;
        struct script_id script;
    } id;
};

const char *get_txn_output_type(enum txnouttype t);

bool script_solver(const struct script *s, enum txnouttype *type_out,
                   unsigned char solutions[][65], size_t solution_sizes[],
                   size_t *num_solutions);

int script_sig_args_expected(enum txnouttype t,
                             const unsigned char solutions[][65],
                             const size_t solution_sizes[],
                             size_t num_solutions);

bool script_extract_destination(const struct script *s,
                                struct tx_destination *dest_out);

void script_for_p2pkh(struct script *out, const struct key_id *key);
void script_for_p2sh(struct script *out, const struct script_id *script_hash);
void script_for_multisig(struct script *out, int n_required,
                         const struct pubkey *keys, size_t num_keys);

void script_id_from_script(struct script_id *out, const struct script *s);

bool tx_destination_is_valid(const struct tx_destination *dest);
void script_for_destination(struct script *out, const struct tx_destination *dest);

#endif
