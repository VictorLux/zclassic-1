/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_WALLET_KEYSTORE_H
#define ZCL_WALLET_KEYSTORE_H

#include "keys/key.h"
#include "keys/pubkey.h"
#include "script/script.h"
#include "script/standard.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stddef.h>

#define MAX_KEYSTORE_KEYS 4096
#define MAX_KEYSTORE_SCRIPTS 4096
#define MAX_KEYSTORE_WATCHING 4096

struct key_entry {
    struct key_id keyid;
    struct privkey key;
    bool used;
};

struct script_entry {
    struct uint160 script_id;
    struct script redeem_script;
    bool used;
};

struct watching_entry {
    struct key_id keyid;
    struct pubkey key;
    bool used;
};

struct basic_keystore {
    zcl_mutex_t cs;

    struct key_entry keys[MAX_KEYSTORE_KEYS];
    size_t num_keys;

    struct script_entry scripts[MAX_KEYSTORE_SCRIPTS];
    size_t num_scripts;

    struct watching_entry watching[MAX_KEYSTORE_WATCHING];
    size_t num_watching;
};

void keystore_init(struct basic_keystore *ks);
void keystore_free(struct basic_keystore *ks);

bool keystore_add_key(struct basic_keystore *ks, const struct privkey *key);
bool keystore_have_key(const struct basic_keystore *ks,
                        const struct key_id *keyid);
bool keystore_get_key(const struct basic_keystore *ks,
                       const struct key_id *keyid, struct privkey *key_out);
bool keystore_get_pubkey(const struct basic_keystore *ks,
                          const struct key_id *keyid, struct pubkey *pk_out);
size_t keystore_get_keys(const struct basic_keystore *ks,
                          struct key_id *out, size_t max_out);

bool keystore_add_cscript(struct basic_keystore *ks,
                            const struct script *redeem_script);
bool keystore_have_cscript(const struct basic_keystore *ks,
                             const struct uint160 *script_id);
bool keystore_get_cscript(const struct basic_keystore *ks,
                            const struct uint160 *script_id,
                            struct script *script_out);

bool keystore_add_watch_only(struct basic_keystore *ks,
                               const struct pubkey *pk);
bool keystore_have_watch_only(const struct basic_keystore *ks,
                                const struct key_id *keyid);
bool keystore_remove_watch_only(struct basic_keystore *ks,
                                  const struct key_id *keyid);

#endif
