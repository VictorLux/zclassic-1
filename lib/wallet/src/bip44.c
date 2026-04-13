/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BIP44 multi-account hierarchy for ZClassic.
 * Path: m/44'/147'/account'/change/index
 */

#include "wallet/bip44.h"
#include "util/log_macros.h"
#include "support/cleanse.h"
#include <string.h>
#include <stdio.h>

#define DOMAIN "bip44"

bool bip44_derive_account(const struct ext_key *master,
                          struct ext_key *account_out,
                          uint32_t account)
{
    GUARD_NOT_NULL(master, DOMAIN, "master");
    GUARD_NOT_NULL(account_out, DOMAIN, "account_out");
    GUARD(account <= BIP44_MAX_ACCOUNT, DOMAIN,
          "account index too large: %u", account);

    /* m/44'/147'/account' */
    uint32_t indices[3] = {
        BIP44_PURPOSE  | BIP32_HARDENED,
        BIP44_ZCL_COIN | BIP32_HARDENED,
        account        | BIP32_HARDENED
    };

    if (!hd_derive_path(master, account_out, indices, 3))
        LOG_FAIL(DOMAIN, "failed to derive account %u", account);

    return true;
}

bool bip44_derive_chain(const struct ext_key *master,
                        struct ext_key *chain_out,
                        uint32_t account, uint32_t change)
{
    GUARD_NOT_NULL(master, DOMAIN, "master");
    GUARD_NOT_NULL(chain_out, DOMAIN, "chain_out");
    GUARD(change == BIP44_EXTERNAL || change == BIP44_INTERNAL,
          DOMAIN, "change must be 0 or 1, got %u", change);

    /* m/44'/147'/account'/change */
    uint32_t indices[4] = {
        BIP44_PURPOSE  | BIP32_HARDENED,
        BIP44_ZCL_COIN | BIP32_HARDENED,
        account        | BIP32_HARDENED,
        change
    };

    if (!hd_derive_path(master, chain_out, indices, 4))
        LOG_FAIL(DOMAIN, "failed to derive chain account=%u change=%u",
                 account, change);

    return true;
}

bool bip44_derive_key(const struct ext_key *master,
                      struct ext_key *key_out,
                      uint32_t account, uint32_t change, uint32_t index)
{
    GUARD_NOT_NULL(master, DOMAIN, "master");
    GUARD_NOT_NULL(key_out, DOMAIN, "key_out");
    GUARD(change == BIP44_EXTERNAL || change == BIP44_INTERNAL,
          DOMAIN, "change must be 0 or 1, got %u", change);
    GUARD(index <= BIP44_MAX_INDEX, DOMAIN,
          "address index too large: %u", index);

    /* m/44'/147'/account'/change/index */
    uint32_t indices[5] = {
        BIP44_PURPOSE  | BIP32_HARDENED,
        BIP44_ZCL_COIN | BIP32_HARDENED,
        account        | BIP32_HARDENED,
        change,
        index
    };

    if (!hd_derive_path(master, key_out, indices, 5))
        LOG_FAIL(DOMAIN, "failed to derive key account=%u change=%u index=%u",
                 account, change, index);

    return true;
}

bool bip44_derive_keypair(const struct ext_key *master,
                          struct privkey *priv_out,
                          struct pubkey *pub_out,
                          uint32_t account, uint32_t change, uint32_t index)
{
    GUARD_NOT_NULL(priv_out, DOMAIN, "priv_out");
    GUARD_NOT_NULL(pub_out, DOMAIN, "pub_out");

    struct ext_key child;
    if (!bip44_derive_key(master, &child, account, change, index)) {
        memory_cleanse(&child, sizeof(child));
        return false;
    }

    *priv_out = child.key;
    if (!privkey_get_pubkey(&child.key, pub_out)) {
        memory_cleanse(&child, sizeof(child));
        LOG_FAIL(DOMAIN, "failed to get pubkey for account=%u change=%u index=%u",
                 account, change, index);
    }

    memory_cleanse(&child, sizeof(child));
    return true;
}

int bip44_format_path(char *buf, size_t buf_size,
                      uint32_t account, uint32_t change, uint32_t index)
{
    if (!buf || buf_size == 0)
        return -1;

    int n = snprintf(buf, buf_size, "m/44'/147'/%u'/%u/%u",
                     account, change, index);
    if (n < 0 || (size_t)n >= buf_size)
        return -1;
    return n;
}
