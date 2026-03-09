/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "wallet/sapling_keys.h"
#include "core/random.h"
#include "encoding/bech32.h"
#include "encoding/utilstrencodings.h"
#include "zcash/sapling.h"
#include "support/cleanse.h"
#include <string.h>
#include <stdio.h>

void sapling_keystore_init(struct sapling_keystore *sks)
{
    zcl_mutex_init(&sks->cs);
    memset(sks->seed, 0, 32);
    sks->has_seed = false;
    sks->next_child_index = 0;
    sks->num_keys = 0;
    memset(sks->keys, 0, sizeof(sks->keys));
}

void sapling_keystore_free(struct sapling_keystore *sks)
{
    memory_cleanse(sks->seed, 32);
    memory_cleanse(&sks->master_xsk, sizeof(sks->master_xsk));
    for (size_t i = 0; i < sks->num_keys; i++) {
        if (sks->keys[i].used)
            memory_cleanse(&sks->keys[i].xsk, sizeof(struct zip32_xsk));
    }
    zcl_mutex_destroy(&sks->cs);
}

bool sapling_keystore_generate_seed(struct sapling_keystore *sks)
{
    zcl_mutex_lock(&sks->cs);
    GetRandBytes(sks->seed, 32);
    sks->has_seed = true;
    zip32_xsk_master(&sks->master_xsk, sks->seed, 32);
    sks->next_child_index = 0;
    zcl_mutex_unlock(&sks->cs);
    return true;
}

bool sapling_keystore_set_seed(struct sapling_keystore *sks,
                                const uint8_t seed[32])
{
    zcl_mutex_lock(&sks->cs);
    memcpy(sks->seed, seed, 32);
    sks->has_seed = true;
    zip32_xsk_master(&sks->master_xsk, sks->seed, 32);
    sks->next_child_index = 0;
    zcl_mutex_unlock(&sks->cs);
    return true;
}

bool sapling_keystore_new_address(struct sapling_keystore *sks,
                                   uint8_t diversifier_out[ZC_DIVERSIFIER_SIZE],
                                   uint8_t pk_d_out[32])
{
    zcl_mutex_lock(&sks->cs);

    if (!sks->has_seed) {
        GetRandBytes(sks->seed, 32);
        sks->has_seed = true;
        zip32_xsk_master(&sks->master_xsk, sks->seed, 32);
    }

    if (sks->num_keys >= MAX_SAPLING_KEYS) {
        zcl_mutex_unlock(&sks->cs);
        return false;
    }

    /* Derive child: m/32'/coin_type'/account'
     * ZClassic uses coin_type = 147 (registered in SLIP-0044) */
    struct zip32_xsk purpose_key, coin_key, account_key;
    zip32_xsk_derive(&purpose_key, &sks->master_xsk,
                     32 | ZIP32_HARDENED_KEY_LIMIT);
    zip32_xsk_derive(&coin_key, &purpose_key,
                     147 | ZIP32_HARDENED_KEY_LIMIT);
    zip32_xsk_derive(&account_key, &coin_key,
                     sks->next_child_index | ZIP32_HARDENED_KEY_LIMIT);

    struct zip32_xfvk xfvk;
    zip32_xsk_to_xfvk(&xfvk, &account_key);

    uint8_t diversifier[ZC_DIVERSIFIER_SIZE];
    uint8_t pk_d[32];
    if (!zip32_xfvk_address(&xfvk, diversifier, pk_d)) {
        memory_cleanse(&purpose_key, sizeof(purpose_key));
        memory_cleanse(&coin_key, sizeof(coin_key));
        memory_cleanse(&account_key, sizeof(account_key));
        zcl_mutex_unlock(&sks->cs);
        return false;
    }

    /* Compute IVK for this key */
    uint8_t ivk[32];
    sapling_crh_ivk(xfvk.fvk.ak, xfvk.fvk.nk, ivk);

    struct sapling_key_entry *entry = &sks->keys[sks->num_keys];
    entry->xsk = account_key;
    entry->xfvk = xfvk;
    memcpy(entry->diversifier, diversifier, ZC_DIVERSIFIER_SIZE);
    memcpy(entry->pk_d, pk_d, 32);
    memcpy(entry->ivk, ivk, 32);
    entry->child_index = sks->next_child_index;
    entry->used = true;
    sks->num_keys++;
    sks->next_child_index++;

    memcpy(diversifier_out, diversifier, ZC_DIVERSIFIER_SIZE);
    memcpy(pk_d_out, pk_d, 32);

    memory_cleanse(&purpose_key, sizeof(purpose_key));
    memory_cleanse(&coin_key, sizeof(coin_key));
    memory_cleanse(&account_key, sizeof(account_key));

    zcl_mutex_unlock(&sks->cs);
    return true;
}

bool sapling_encode_payment_address(const uint8_t diversifier[ZC_DIVERSIFIER_SIZE],
                                     const uint8_t pk_d[32],
                                     const char *hrp,
                                     char *out, size_t out_size)
{
    /* Serialize: diversifier(11) || pk_d(32) = 43 bytes */
    uint8_t raw[43];
    memcpy(raw, diversifier, ZC_DIVERSIFIER_SIZE);
    memcpy(raw + ZC_DIVERSIFIER_SIZE, pk_d, 32);

    /* Convert 8-bit to 5-bit for Bech32 */
    uint8_t data5[69]; /* ceil(43 * 8 / 5) = 69 */
    size_t data5_len = 0;
    if (!ConvertBits(8, 5, true, raw, 43, data5, sizeof(data5), &data5_len))
        return false;

    return bech32_encode(out, out_size, hrp, data5, data5_len);
}

bool sapling_decode_payment_address(const char *str,
                                     uint8_t diversifier_out[ZC_DIVERSIFIER_SIZE],
                                     uint8_t pk_d_out[32])
{
    char hrp[64];
    uint8_t data5[128];
    size_t data5_len = 0;
    if (!bech32_decode(hrp, sizeof(hrp), data5, sizeof(data5), &data5_len, str))
        return false;

    uint8_t raw[64];
    size_t raw_len = 0;
    if (!ConvertBits(5, 8, false, data5, data5_len, raw, sizeof(raw), &raw_len))
        return false;

    if (raw_len != 43)
        return false;

    memcpy(diversifier_out, raw, ZC_DIVERSIFIER_SIZE);
    memcpy(pk_d_out, raw + ZC_DIVERSIFIER_SIZE, 32);
    return true;
}

bool sapling_keystore_have_spending_key(const struct sapling_keystore *sks,
                                         const uint8_t ivk[32])
{
    for (size_t i = 0; i < sks->num_keys; i++) {
        if (sks->keys[i].used && memcmp(sks->keys[i].ivk, ivk, 32) == 0)
            return true;
    }
    return false;
}

const struct sapling_key_entry *sapling_keystore_find_by_ivk(
    const struct sapling_keystore *sks, const uint8_t ivk[32])
{
    for (size_t i = 0; i < sks->num_keys; i++) {
        if (sks->keys[i].used && memcmp(sks->keys[i].ivk, ivk, 32) == 0)
            return &sks->keys[i];
    }
    return NULL;
}
