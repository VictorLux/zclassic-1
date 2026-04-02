/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP command service — command-side workflow helpers. */

#include "services/zslp_command_service.h"
#include "models/zslp.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "script/standard.h"
#include "support/cleanse.h"
#include "validation/sighash.h"
#include "validation/txmempool.h"
#include "wallet/keystore.h"
#include "wallet/wallet.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

bool zslp_command_commit_with_op_return(struct wallet *wallet,
                                        struct tx_mempool *mempool,
                                        struct wallet_tx *wtx,
                                        const uint8_t *op_script,
                                        size_t script_len)
{
    const struct chain_params *cp;
    size_t old_nout;
    struct tx_out *new_vout;
    int height;
    uint32_t branch_id;

    if (!wallet || !mempool || !wtx || !op_script || script_len == 0)
        return false;

    old_nout = wtx->tx.num_vout;
    new_vout = calloc(old_nout + 1, sizeof(struct tx_out));
    if (!new_vout)
        return false;

    new_vout[0].value = 0;
    new_vout[0].script_pub_key.size = script_len;
    memcpy(new_vout[0].script_pub_key.data, op_script, script_len);
    for (size_t i = 0; i < old_nout; i++)
        new_vout[i + 1] = wtx->tx.vout[i];
    free(wtx->tx.vout);
    wtx->tx.vout = new_vout;
    wtx->tx.num_vout = old_nout + 1;

    cp = chain_params_get();
    height = wallet->best_block_height;
    branch_id = consensus_current_epoch_branch_id(height + 1, &cp->consensus);

    zcl_mutex_lock(&wallet->cs);
    for (size_t i = 0; i < wtx->tx.num_vin; i++) {
        struct wallet_tx *prev_wtx = NULL;
        struct tx_destination prev_dest;
        struct privkey skey;
        struct pubkey spk;
        struct sighash_type ht;
        struct precomputed_tx_data txdata;
        struct uint256 sighash;
        unsigned char sig[SIGNATURE_SIZE + 1];
        size_t siglen = 0;
        struct script *ss;
        const struct tx_out *prev_out;

        for (size_t j = 0; j < wallet->num_wallet_tx; j++) {
            if (uint256_eq(&wallet->map_wallet[j].tx.hash,
                           &wtx->tx.vin[i].prevout.hash)) {
                prev_wtx = &wallet->map_wallet[j];
                break;
            }
        }
        if (!prev_wtx)
            continue;

        prev_out = &prev_wtx->tx.vout[wtx->tx.vin[i].prevout.n];
        if (!script_extract_destination(&prev_out->script_pub_key, &prev_dest))
            continue;
        if (!keystore_get_key(&wallet->keystore, &prev_dest.id.key, &skey))
            continue;

        privkey_get_pubkey(&skey, &spk);
        ht.raw = SIGHASH_ALL;
        precompute_tx_data(&wtx->tx, &txdata);
        if (!signature_hash(&prev_out->script_pub_key, &wtx->tx,
                            (unsigned int)i, ht, prev_out->value,
                            branch_id, &txdata, &sighash)) {
            memory_cleanse(skey.vch, 32);
            continue;
        }
        if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
            memory_cleanse(skey.vch, 32);
            continue;
        }
        sig[siglen++] = 0x01;

        ss = &wtx->tx.vin[i].script_sig;
        ss->size = 0;
        ss->data[ss->size++] = (unsigned char)siglen;
        memcpy(&ss->data[ss->size], sig, siglen);
        ss->size += siglen;
        ss->data[ss->size++] = (unsigned char)spk.size;
        memcpy(&ss->data[ss->size], spk.vch, spk.size);
        ss->size += spk.size;
        memory_cleanse(skey.vch, 32);
    }
    zcl_mutex_unlock(&wallet->cs);

    transaction_compute_hash(&wtx->tx);
    return wallet_commit_transaction(wallet, wtx, mempool);
}

bool zslp_command_build_genesis_base_tx(struct wallet *wallet,
                                        struct wallet_tx *wtx,
                                        int64_t *fee_paid,
                                        const char **tx_error)
{
    struct pubkey our_pk;
    struct key_id our_kid;
    struct tx_destination our_dest;
    struct tx_destination dests[2];
    int64_t vals[2] = { 546, 546 };

    if (!wallet || !wtx || !fee_paid)
        return false;
    if (!wallet_get_key_from_pool(wallet, &our_pk))
        return false;

    our_kid = pubkey_get_id(&our_pk);
    our_dest.type = DEST_KEY_ID;
    our_dest.id.key = our_kid;
    dests[0] = our_dest;
    dests[1] = our_dest;
    return wallet_create_transaction_multi(wallet, dests, vals, 2, wtx,
                                           fee_paid, tx_error);
}

bool zslp_command_build_send_base_tx(struct wallet *wallet,
                                     const char *to_addr,
                                     struct wallet_tx *wtx,
                                     int64_t *fee_paid,
                                     const char **tx_error)
{
    struct tx_destination dest;
    int64_t vals[1] = { 546 };

    if (!wallet || !to_addr || !wtx || !fee_paid)
        return false;
    if (!zslp_service_decode_transparent_destination(to_addr, &dest))
        return false;
    return wallet_create_transaction_multi(wallet, &dest, vals, 1, wtx,
                                           fee_paid, tx_error);
}

static bool zslp_command_pick_token_id(const char *broadcast_txid,
                                       const struct zslp_token_create_request *req,
                                       char token_id_out[ZSLP_TOKEN_KEY_MAX + 1])
{
    const char *token_id = broadcast_txid;

    if (!req || !token_id_out)
        return false;
    if (!token_id || token_id[0] == '\0')
        token_id = req->ticker;
    if (!token_id || token_id[0] == '\0')
        return false;

    snprintf(token_id_out, ZSLP_TOKEN_KEY_MAX + 1, "%s", token_id);
    return true;
}

bool zslp_command_finalize_genesis(const char *datadir,
                                   const char *broadcast_txid,
                                   const struct zslp_token_create_request *req,
                                   char token_id_out[ZSLP_TOKEN_KEY_MAX + 1])
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    const char *validation_error;

    if (!datadir || !req || !token_id_out)
        return false;

    validation_error = zslp_service_validate_create_request(req);
    if (validation_error)
        return false;
    if (!zslp_command_pick_token_id(broadcast_txid, req, token_id_out))
        return false;
    if (!zslp_service_open_db(datadir, &db, &owns_db))
        return false;

    if (!zslp_service_store_token(db, token_id_out, req->ticker, req->name,
                                  req->decimals, (int64_t)req->initial_supply)) {
        zslp_service_close_db(db, owns_db);
        return false;
    }
    zslp_service_close_db(db, owns_db);
    return true;
}

bool zslp_command_credit_transfer(const char *datadir,
                                  const struct zslp_token_transfer_request *req)
{
    sqlite3 *db = NULL;
    bool owns_db = false;
    const char *validation_error;

    if (!datadir || !req)
        return false;

    validation_error = zslp_service_validate_transfer_request(req);
    if (validation_error)
        return false;
    if (!zslp_service_open_db(datadir, &db, &owns_db))
        return false;
    if (!zslp_service_credit_balance(db, req->token_id,
                                     req->recipient_addr, req->amount)) {
        zslp_service_close_db(db, owns_db);
        return false;
    }
    zslp_service_close_db(db, owns_db);
    return true;
}
