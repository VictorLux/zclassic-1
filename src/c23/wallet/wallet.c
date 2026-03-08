/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "wallet/wallet.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "core/random.h"
#include "core/utiltime.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "support/cleanse.h"
#include "validation/txmempool.h"
#include "validation/check_transaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wallet_init(struct wallet *w)
{
    zcl_mutex_init(&w->cs);
    keystore_init(&w->keystore);
    memset(w->map_wallet, 0, sizeof(w->map_wallet));
    w->num_wallet_tx = 0;
    w->key_pool_size = 0;
    w->next_key_pool_index = 0;
    w->oldest_key_pool_time = 0;
    w->time_first_key = 0;
    w->has_master_key = false;
    w->hd_chain_counter = 0;
    w->default_fee = 10000;
    w->min_fee = 1000;
    w->spend_zero_conf_change = true;
    w->best_block = NULL;
    w->best_block_height = 0;
}

void wallet_free(struct wallet *w)
{
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (w->map_wallet[i].used) {
            transaction_free(&w->map_wallet[i].tx);
            w->map_wallet[i].used = false;
        }
    }
    w->num_wallet_tx = 0;
    keystore_free(&w->keystore);
    zcl_mutex_destroy(&w->cs);
}

bool wallet_generate_new_key(struct wallet *w, struct pubkey *pk_out)
{
    struct privkey key;
    privkey_init(&key);
    privkey_make_new(&key, true);

    if (!privkey_is_valid(&key))
        return false;

    struct pubkey pk;
    if (!privkey_get_pubkey(&key, &pk)) {
        memory_cleanse(key.vch, 32);
        return false;
    }

    zcl_mutex_lock(&w->cs);
    bool ok = keystore_add_key(&w->keystore, &key);
    zcl_mutex_unlock(&w->cs);

    memory_cleanse(key.vch, 32);

    if (ok && pk_out)
        *pk_out = pk;
    return ok;
}

bool wallet_get_new_address(struct wallet *w, char *addr_out, size_t addr_size)
{
    struct pubkey pk;
    if (!wallet_get_key_from_pool(w, &pk))
        return false;

    struct key_id kid = pubkey_get_id(&pk);
    struct tx_destination dest;
    dest.type = DEST_KEY_ID;
    dest.id.key = kid;

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    return encode_destination(&dest, pk_pfx, pk_pfx_len,
                              sc_pfx, sc_pfx_len, addr_out, addr_size);
}

bool wallet_top_up_key_pool(struct wallet *w, unsigned int target_size)
{
    if (target_size == 0)
        target_size = DEFAULT_KEYPOOL_SIZE;
    if (target_size > MAX_KEY_POOL)
        target_size = MAX_KEY_POOL;

    zcl_mutex_lock(&w->cs);
    while (w->key_pool_size < target_size) {
        struct pubkey pk;
        if (!wallet_generate_new_key(w, &pk)) {
            zcl_mutex_unlock(&w->cs);
            return false;
        }
        w->key_pool[w->key_pool_size] = w->next_key_pool_index++;
        w->key_pool_size++;
    }
    zcl_mutex_unlock(&w->cs);
    return true;
}

bool wallet_get_key_from_pool(struct wallet *w, struct pubkey *pk_out)
{
    zcl_mutex_lock(&w->cs);
    if (w->key_pool_size == 0) {
        zcl_mutex_unlock(&w->cs);
        if (!wallet_top_up_key_pool(w, DEFAULT_KEYPOOL_SIZE))
            return false;
        zcl_mutex_lock(&w->cs);
    }

    if (w->key_pool_size == 0) {
        zcl_mutex_unlock(&w->cs);
        return wallet_generate_new_key(w, pk_out);
    }

    w->key_pool_size--;
    zcl_mutex_unlock(&w->cs);

    return wallet_generate_new_key(w, pk_out);
}

static size_t wallet_find_slot(const struct wallet *w, const struct uint256 *hash)
{
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (w->map_wallet[i].used &&
            uint256_eq(&w->map_wallet[i].tx.hash, hash))
            return i;
    }
    return MAX_WALLET_TX;
}

static size_t wallet_find_free_slot(const struct wallet *w)
{
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used)
            return i;
    }
    return MAX_WALLET_TX;
}

bool wallet_add_to_wallet(struct wallet *w, const struct wallet_tx *wtx)
{
    zcl_mutex_lock(&w->cs);

    size_t idx = wallet_find_slot(w, &wtx->tx.hash);
    if (idx < MAX_WALLET_TX) {
        w->map_wallet[idx].hash_block = wtx->hash_block;
        w->map_wallet[idx].confirms = wtx->confirms;
        w->map_wallet[idx].debit_cached_valid = false;
        w->map_wallet[idx].credit_cached_valid = false;
        w->map_wallet[idx].available_credit_cached_valid = false;
        zcl_mutex_unlock(&w->cs);
        return true;
    }

    idx = wallet_find_free_slot(w);
    if (idx >= MAX_WALLET_TX) {
        zcl_mutex_unlock(&w->cs);
        return false;
    }

    w->map_wallet[idx] = *wtx;
    w->map_wallet[idx].used = true;

    if (wtx->tx.num_vin > 0 && wtx->tx.vin) {
        struct transaction *dst = &w->map_wallet[idx].tx;
        dst->vin = malloc(wtx->tx.num_vin * sizeof(struct tx_in));
        if (!dst->vin) {
            w->map_wallet[idx].used = false;
            zcl_mutex_unlock(&w->cs);
            return false;
        }
        memcpy(dst->vin, wtx->tx.vin,
               wtx->tx.num_vin * sizeof(struct tx_in));
    }
    if (wtx->tx.num_vout > 0 && wtx->tx.vout) {
        struct transaction *dst = &w->map_wallet[idx].tx;
        dst->vout = malloc(wtx->tx.num_vout * sizeof(struct tx_out));
        if (!dst->vout) {
            free(dst->vin);
            dst->vin = NULL;
            w->map_wallet[idx].used = false;
            zcl_mutex_unlock(&w->cs);
            return false;
        }
        memcpy(dst->vout, wtx->tx.vout,
               wtx->tx.num_vout * sizeof(struct tx_out));
    }

    w->num_wallet_tx++;
    zcl_mutex_unlock(&w->cs);
    return true;
}

bool wallet_have_tx(const struct wallet *w, const struct uint256 *hash)
{
    return wallet_find_slot(w, hash) < MAX_WALLET_TX;
}

const struct wallet_tx *wallet_get_tx(const struct wallet *w,
                                       const struct uint256 *hash)
{
    size_t idx = wallet_find_slot(w, hash);
    if (idx < MAX_WALLET_TX)
        return &w->map_wallet[idx];
    return NULL;
}

void wallet_mark_dirty(struct wallet_tx *wtx)
{
    wtx->debit_cached_valid = false;
    wtx->credit_cached_valid = false;
    wtx->immature_credit_cached_valid = false;
    wtx->available_credit_cached_valid = false;
}

bool wallet_is_mine(const struct wallet *w, const struct tx_out *txout)
{
    struct tx_destination dest;
    if (!script_extract_destination(&txout->script_pub_key, &dest))
        return false;

    if (dest.type == DEST_KEY_ID)
        return keystore_have_key(&w->keystore, &dest.id.key);
    if (dest.type == DEST_SCRIPT_ID)
        return keystore_have_cscript(&w->keystore, &dest.id.script.hash);
    return false;
}

bool wallet_is_from_me(const struct wallet *w, const struct transaction *tx)
{
    return wallet_get_debit(w, tx) > 0;
}

bool wallet_is_change(const struct wallet *w, const struct tx_out *txout)
{
    struct tx_destination dest;
    if (!script_extract_destination(&txout->script_pub_key, &dest))
        return false;
    if (dest.type != DEST_KEY_ID)
        return false;
    return keystore_have_key(&w->keystore, &dest.id.key);
}

int64_t wallet_get_debit(const struct wallet *w, const struct transaction *tx)
{
    int64_t debit = 0;
    for (size_t i = 0; i < tx->num_vin; i++) {
        const struct outpoint *prevout = &tx->vin[i].prevout;
        size_t idx = wallet_find_slot(w, &prevout->hash);
        if (idx < MAX_WALLET_TX) {
            const struct wallet_tx *prev = &w->map_wallet[idx];
            if (prevout->n < prev->tx.num_vout) {
                if (wallet_is_mine(w, &prev->tx.vout[prevout->n]))
                    debit += prev->tx.vout[prevout->n].value;
            }
        }
    }
    return debit;
}

int64_t wallet_get_credit(const struct wallet *w, const struct tx_out *txout)
{
    if (!MoneyRange(txout->value))
        return 0;
    return wallet_is_mine(w, txout) ? txout->value : 0;
}

int64_t wallet_get_balance(const struct wallet *w)
{
    int64_t balance = 0;
    zcl_mutex_lock((zcl_mutex_t *)&w->cs);
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used)
            continue;
        const struct wallet_tx *wtx = &w->map_wallet[i];
        if (wtx->confirms < 1)
            continue;
        if (transaction_is_coinbase(&wtx->tx) &&
            wallet_tx_get_blocks_to_maturity(wtx) > 0)
            continue;

        for (size_t j = 0; j < wtx->tx.num_vout; j++) {
            if (wallet_is_mine(w, &wtx->tx.vout[j])) {
                bool spent = false;
                for (size_t k = 0; k < MAX_WALLET_TX && !spent; k++) {
                    if (!w->map_wallet[k].used) continue;
                    const struct transaction *stx = &w->map_wallet[k].tx;
                    for (size_t m = 0; m < stx->num_vin; m++) {
                        if (uint256_eq(&stx->vin[m].prevout.hash, &wtx->tx.hash) &&
                            stx->vin[m].prevout.n == (uint32_t)j) {
                            spent = true;
                            break;
                        }
                    }
                }
                if (!spent)
                    balance += wtx->tx.vout[j].value;
            }
        }
    }
    zcl_mutex_unlock((zcl_mutex_t *)&w->cs);
    return balance;
}

int64_t wallet_get_unconfirmed_balance(const struct wallet *w)
{
    int64_t balance = 0;
    zcl_mutex_lock((zcl_mutex_t *)&w->cs);
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used)
            continue;
        const struct wallet_tx *wtx = &w->map_wallet[i];
        if (wtx->confirms != 0)
            continue;
        for (size_t j = 0; j < wtx->tx.num_vout; j++) {
            if (wallet_is_mine(w, &wtx->tx.vout[j]))
                balance += wtx->tx.vout[j].value;
        }
    }
    zcl_mutex_unlock((zcl_mutex_t *)&w->cs);
    return balance;
}

int64_t wallet_get_immature_balance(const struct wallet *w)
{
    int64_t balance = 0;
    zcl_mutex_lock((zcl_mutex_t *)&w->cs);
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used)
            continue;
        const struct wallet_tx *wtx = &w->map_wallet[i];
        if (transaction_is_coinbase(&wtx->tx) &&
            wallet_tx_get_blocks_to_maturity(wtx) > 0) {
            for (size_t j = 0; j < wtx->tx.num_vout; j++) {
                if (wallet_is_mine(w, &wtx->tx.vout[j]))
                    balance += wtx->tx.vout[j].value;
            }
        }
    }
    zcl_mutex_unlock((zcl_mutex_t *)&w->cs);
    return balance;
}

void wallet_available_coins(const struct wallet *w,
                             struct coin_entry *coins_out,
                             size_t *num_coins, size_t max_coins,
                             bool only_confirmed, bool include_zero_value)
{
    *num_coins = 0;
    zcl_mutex_lock((zcl_mutex_t *)&w->cs);

    for (size_t i = 0; i < MAX_WALLET_TX && *num_coins < max_coins; i++) {
        if (!w->map_wallet[i].used)
            continue;
        const struct wallet_tx *wtx = &w->map_wallet[i];

        if (only_confirmed && wtx->confirms < 1)
            continue;

        if (transaction_is_coinbase(&wtx->tx) &&
            wallet_tx_get_blocks_to_maturity(wtx) > 0)
            continue;

        for (size_t j = 0; j < wtx->tx.num_vout && *num_coins < max_coins; j++) {
            const struct tx_out *out = &wtx->tx.vout[j];
            if (!include_zero_value && out->value == 0)
                continue;
            if (!wallet_is_mine(w, out))
                continue;

            bool spent = false;
            for (size_t k = 0; k < MAX_WALLET_TX && !spent; k++) {
                if (!w->map_wallet[k].used) continue;
                const struct transaction *stx = &w->map_wallet[k].tx;
                for (size_t m = 0; m < stx->num_vin; m++) {
                    if (uint256_eq(&stx->vin[m].prevout.hash, &wtx->tx.hash) &&
                        stx->vin[m].prevout.n == (uint32_t)j) {
                        spent = true;
                        break;
                    }
                }
            }
            if (spent)
                continue;

            coins_out[*num_coins].wtx = wtx;
            coins_out[*num_coins].i = (unsigned int)j;
            coins_out[*num_coins].depth = wtx->confirms;
            coins_out[*num_coins].spendable = true;
            coins_out[*num_coins].solvable = true;
            (*num_coins)++;
        }
    }
    zcl_mutex_unlock((zcl_mutex_t *)&w->cs);
}

bool wallet_select_coins(const struct wallet *w,
                          const struct coin_entry *available, size_t num_available,
                          int64_t target_value,
                          struct coin_entry *selected, size_t *num_selected,
                          size_t max_selected, int64_t *value_out)
{
    (void)w;
    *num_selected = 0;
    *value_out = 0;

    for (size_t i = 0; i < num_available && *num_selected < max_selected; i++) {
        if (!available[i].spendable)
            continue;
        int64_t coin_value = available[i].wtx->tx.vout[available[i].i].value;
        selected[*num_selected] = available[i];
        (*num_selected)++;
        *value_out += coin_value;
        if (*value_out >= target_value)
            return true;
    }
    return *value_out >= target_value;
}

bool wallet_create_transaction(struct wallet *w,
                                const struct tx_destination *dest,
                                int64_t value,
                                struct wallet_tx *wtx_out,
                                int64_t *fee_out,
                                const char **error)
{
    if (value <= 0) {
        *error = "Invalid amount";
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    int64_t fee = w->default_fee;

    struct coin_entry available[4096];
    size_t num_available = 0;
    wallet_available_coins(w, available, &num_available, 4096, true, false);

    struct coin_entry selected[4096];
    size_t num_selected = 0;
    int64_t selected_value = 0;

    if (!wallet_select_coins(w, available, num_available, value + fee,
                             selected, &num_selected, 4096, &selected_value)) {
        *error = "Insufficient funds";
        return false;
    }

    memset(wtx_out, 0, sizeof(*wtx_out));
    transaction_init(&wtx_out->tx);

    int height = w->best_block_height;
    int epoch = consensus_current_epoch(height, &cp->consensus);

    if (epoch >= UPGRADE_SAPLING) {
        wtx_out->tx.overwintered = true;
        wtx_out->tx.version = SAPLING_TX_VERSION;
        wtx_out->tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        wtx_out->tx.expiry_height = (uint32_t)(height + 20);
    } else if (epoch >= UPGRADE_OVERWINTER) {
        wtx_out->tx.overwintered = true;
        wtx_out->tx.version = OVERWINTER_TX_VERSION;
        wtx_out->tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        wtx_out->tx.expiry_height = (uint32_t)(height + 20);
    }

    size_t num_out = (selected_value > value + fee) ? 2 : 1;
    if (!transaction_alloc(&wtx_out->tx, num_selected, num_out)) {
        *error = "Transaction allocation failed";
        return false;
    }

    struct script dest_script;
    script_for_destination(&dest_script, dest);
    wtx_out->tx.vout[0].value = value;
    wtx_out->tx.vout[0].script_pub_key = dest_script;

    if (num_out == 2) {
        int64_t change = selected_value - value - fee;
        struct pubkey change_pk;
        if (!wallet_get_key_from_pool(w, &change_pk)) {
            transaction_free(&wtx_out->tx);
            *error = "Cannot get change address";
            return false;
        }
        struct key_id change_kid = pubkey_get_id(&change_pk);
        struct tx_destination change_dest;
        change_dest.type = DEST_KEY_ID;
        change_dest.id.key = change_kid;
        struct script change_script;
        script_for_destination(&change_script, &change_dest);
        wtx_out->tx.vout[1].value = change;
        wtx_out->tx.vout[1].script_pub_key = change_script;
    }

    for (size_t i = 0; i < num_selected; i++) {
        wtx_out->tx.vin[i].prevout.hash = selected[i].wtx->tx.hash;
        wtx_out->tx.vin[i].prevout.n = selected[i].i;
        wtx_out->tx.vin[i].sequence = UINT32_MAX - 1;
    }

    zcl_mutex_lock(&w->cs);
    for (size_t i = 0; i < num_selected; i++) {
        struct privkey skey;
        const struct tx_out *prev_out =
            &selected[i].wtx->tx.vout[selected[i].i];
        struct tx_destination prev_dest;
        if (!script_extract_destination(&prev_out->script_pub_key, &prev_dest)) {
            zcl_mutex_unlock(&w->cs);
            transaction_free(&wtx_out->tx);
            *error = "Cannot determine input destination";
            return false;
        }

        if (!keystore_get_key(&w->keystore, &prev_dest.id.key, &skey)) {
            zcl_mutex_unlock(&w->cs);
            transaction_free(&wtx_out->tx);
            *error = "Private key not available";
            return false;
        }

        struct pubkey spk;
        privkey_get_pubkey(&skey, &spk);

        struct uint256 sighash;
        uint256_set_null(&sighash);

        unsigned char sig[SIGNATURE_SIZE + 1];
        size_t siglen = 0;
        if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
            memory_cleanse(skey.vch, 32);
            zcl_mutex_unlock(&w->cs);
            transaction_free(&wtx_out->tx);
            *error = "Signing failed";
            return false;
        }
        sig[siglen++] = 0x01;

        struct script *ss = &wtx_out->tx.vin[i].script_sig;
        ss->size = 0;
        ss->data[ss->size++] = (unsigned char)siglen;
        memcpy(&ss->data[ss->size], sig, siglen);
        ss->size += siglen;
        ss->data[ss->size++] = (unsigned char)spk.size;
        memcpy(&ss->data[ss->size], spk.vch, spk.size);
        ss->size += spk.size;

        memory_cleanse(skey.vch, 32);
    }
    zcl_mutex_unlock(&w->cs);

    transaction_compute_hash(&wtx_out->tx);
    wtx_out->time_received = GetTime();
    wtx_out->from_me = true;
    wtx_out->used = true;

    if (fee_out)
        *fee_out = fee;

    return true;
}

bool wallet_commit_transaction(struct wallet *w, struct wallet_tx *wtx,
                                struct tx_mempool *mempool)
{
    if (!wallet_add_to_wallet(w, wtx))
        return false;

    struct validation_state vs;
    validation_state_init(&vs);

    int64_t fee = 0;
    int64_t value_out = transaction_get_value_out(&wtx->tx);
    int64_t value_in = wallet_get_debit(w, &wtx->tx);
    if (value_in > value_out)
        fee = value_in - value_out;

    const struct chain_params *cp = chain_params_get();
    int height = w->best_block_height;
    uint32_t branch_id = consensus_current_epoch_branch_id(height, &cp->consensus);

    struct mempool_entry entry;
    mempool_entry_init(&entry, &wtx->tx, fee, GetTime(), 0.0,
                       (unsigned int)height, true, false, branch_id);

    zcl_mutex_lock(&mempool->cs);
    bool ok = tx_mempool_add_unchecked(mempool, &wtx->tx.hash, &entry);
    zcl_mutex_unlock(&mempool->cs);

    return ok;
}

void wallet_sync_transaction(struct wallet *w, const struct transaction *tx,
                              const struct block_index *pindex)
{
    zcl_mutex_lock(&w->cs);

    bool dominated = false;
    for (size_t i = 0; i < tx->num_vout; i++) {
        if (wallet_is_mine(w, &tx->vout[i])) {
            dominated = true;
            break;
        }
    }
    if (!dominated) {
        for (size_t i = 0; i < tx->num_vin; i++) {
            size_t idx = wallet_find_slot(w, &tx->vin[i].prevout.hash);
            if (idx < MAX_WALLET_TX) {
                dominated = true;
                break;
            }
        }
    }

    if (!dominated) {
        zcl_mutex_unlock(&w->cs);
        return;
    }

    struct wallet_tx wtx;
    memset(&wtx, 0, sizeof(wtx));
    wtx.tx = *tx;
    wtx.tx.vin = NULL;
    wtx.tx.vout = NULL;
    wtx.time_received = GetTime();
    wtx.from_me = false;
    wtx.used = true;

    if (pindex) {
        if (pindex->phashBlock)
            wtx.hash_block = *pindex->phashBlock;
        wtx.confirms = w->best_block_height - pindex->nHeight + 1;
    }

    for (size_t i = 0; i < tx->num_vin; i++) {
        size_t idx = wallet_find_slot(w, &tx->vin[i].prevout.hash);
        if (idx < MAX_WALLET_TX)
            wtx.from_me = true;
    }

    size_t existing = wallet_find_slot(w, &tx->hash);
    if (existing < MAX_WALLET_TX) {
        w->map_wallet[existing].hash_block = wtx.hash_block;
        w->map_wallet[existing].confirms = wtx.confirms;
        wallet_mark_dirty(&w->map_wallet[existing]);
    } else {
        wtx.tx.vin = (struct tx_in *)tx->vin;
        wtx.tx.vout = (struct tx_out *)tx->vout;
        zcl_mutex_unlock(&w->cs);
        wallet_add_to_wallet(w, &wtx);
        return;
    }

    zcl_mutex_unlock(&w->cs);
}

bool wallet_import_key(struct wallet *w, const struct privkey *key)
{
    zcl_mutex_lock(&w->cs);
    bool ok = keystore_add_key(&w->keystore, key);
    zcl_mutex_unlock(&w->cs);
    return ok;
}

bool wallet_dump_key(const struct wallet *w, const struct key_id *keyid,
                      struct privkey *key_out)
{
    return keystore_get_key(&w->keystore, keyid, key_out);
}

int wallet_tx_get_depth_in_main_chain(const struct wallet *w,
                                        const struct wallet_tx *wtx)
{
    (void)w;
    if (!wtx->used)
        return 0;
    return wtx->confirms > 0 ? wtx->confirms : 0;
}

int wallet_tx_get_blocks_to_maturity(const struct wallet_tx *wtx)
{
    if (!transaction_is_coinbase(&wtx->tx))
        return 0;
    int maturity = 100 - wtx->confirms;
    return maturity > 0 ? maturity : 0;
}
