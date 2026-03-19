/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/transaction_controller.h"
#include "controllers/strong_params.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "encoding/utilstrencodings.h"
#include "keys/key.h"
#include "keys/key_io.h"
#include "json/json.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"
#include "validation/check_transaction.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "coins/coins_view.h"
#include "models/database.h"
#include "models/utxo.h"
#include "net/connman.h"
#include "wallet/keystore.h"
#include "support/cleanse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct main_state *g_ms = NULL;
static struct tx_mempool *g_mp = NULL;
static struct coins_view_cache *g_coins_tip = NULL;
static const char *g_datadir = NULL;
static struct basic_keystore *g_keystore = NULL;
static struct connman *g_connman = NULL;
extern struct node_db *g_node_db;

void rpc_rawtx_set_state(struct main_state *ms, struct tx_mempool *mp,
                          struct coins_view_cache *coins_tip,
                          const char *datadir)
{
    g_ms = ms;
    g_mp = mp;
    g_coins_tip = coins_tip;
    g_datadir = datadir;
}

void rpc_rawtx_set_keystore(struct basic_keystore *ks)
{
    g_keystore = ks;
}

void rpc_rawtx_set_connman(struct connman *cm)
{
    g_connman = cm;
}

static bool rpc_getrawtransaction(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    RPC_HELP(help, result,
        "getrawtransaction \"txid\" ( verbose )\n"
        "Return the raw transaction data.\n"
        "Arguments:\n"
        "1. \"txid\"    (string, required) The transaction id\n"
        "2. verbose   (numeric, optional, default=0) "
        "If 0, return hex; if 1, return JSON object");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    int verbose = (int)rpc_permit_int(&p, 1, "verbose", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct uint256 hash;
    if (!parse_hash_str(txid_str, &hash)) {
        json_set_str(result, "Invalid txid format");
        return false;
    }

    struct transaction tx;
    transaction_init(&tx);
    struct uint256 hash_block;
    uint256_set_null(&hash_block);
    bool found = false;

    /* 1. Check mempool */
    if (g_mp && tx_mempool_lookup(g_mp, &hash, &tx)) {
        found = true;
    }

    /* 2. Check txindex for O(1) disk lookup */
    extern struct block_tree_db *g_active_block_tree;
    if (!found && g_active_block_tree && g_ms && g_ms->fTxIndex) {
        struct disk_tx_pos pos;
        if (block_tree_db_read_tx_index(g_active_block_tree, &hash, &pos)) {
            FILE *f = open_block_file(g_datadir, &pos.block_pos, true);
            if (f) {
                unsigned char hdr_buf[256];
                size_t hdr_read = fread(hdr_buf, 1, sizeof(hdr_buf), f);
                if (hdr_read > 0) {
                    struct byte_stream hs;
                    stream_init_from_data(&hs, hdr_buf, hdr_read);
                    struct block_header bh;
                    block_header_deserialize(&bh, &hs);
                    block_header_get_hash(&bh, &hash_block);
                }
                fseek(f, (long)pos.block_pos.nPos + (long)pos.nTxOffset,
                      SEEK_SET);
                unsigned char tx_buf[2 * 1024 * 1024];
                size_t tx_read = fread(tx_buf, 1, sizeof(tx_buf), f);
                fclose(f);
                if (tx_read > 0) {
                    struct byte_stream ts;
                    stream_init_from_data(&ts, tx_buf, tx_read);
                    transaction_free(&tx);
                    transaction_init(&tx);
                    if (transaction_deserialize(&tx, &ts))
                        found = true;
                }
            }
        }
    }

    /* 3. Fallback: use coins DB to find block, then scan block */
    if (!found && g_coins_tip && g_ms && g_datadir) {
        struct coins entry;
        coins_init(&entry);
        if (coins_view_cache_get_coins(g_coins_tip, &hash, &entry)) {
            if (entry.height > 0) {
                struct block_index *bi = active_chain_at(
                    &g_ms->chain_active, entry.height);
                if (bi) {
                    struct block blk;
                    block_init(&blk);
                    if (read_block_from_disk_index(&blk, bi, g_datadir)) {
                        for (size_t i = 0; i < blk.num_vtx; i++) {
                            if (uint256_cmp(&blk.vtx[i].hash, &hash) == 0) {
                                transaction_free(&tx);
                                transaction_init(&tx);
                                transaction_copy(&tx, &blk.vtx[i]);
                                block_header_get_hash(&blk.header,
                                                      &hash_block);
                                found = true;
                                break;
                            }
                        }
                    }
                    block_free(&blk);
                }
            }
            coins_free(&entry);
        }
    }

    if (!found) {
        transaction_free(&tx);
        json_set_str(result, "Transaction not found");
        return false;
    }

    if (verbose == 0) {
        char *hex = malloc(2 * 1024 * 1024);
        if (!hex) {
            transaction_free(&tx);
            return false;
        }
        size_t hex_len = encode_hex_tx(&tx, hex, 2 * 1024 * 1024);
        hex[hex_len] = '\0';
        json_set_str(result, hex);
        free(hex);
    } else {
        tx_to_json(&tx, &hash_block, result);
    }

    transaction_free(&tx);
    return true;
}

static bool rpc_decoderawtransaction(const struct json_value *params, bool help,
                                      struct json_value *result)
{
    RPC_HELP(help, result,
        "decoderawtransaction \"hexstring\"\n"
        "Return a JSON object representing the serialized transaction.\n"
        "Arguments:\n"
        "1. \"hexstring\" (string, required) The transaction hex string");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *hex_str = rpc_require_str(&p, 0, "hexstring");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct transaction tx;
    transaction_init(&tx);
    if (!decode_hex_tx(&tx, hex_str)) {
        transaction_free(&tx);
        json_set_str(result, "TX decode failed");
        return false;
    }

    struct uint256 null_hash;
    uint256_set_null(&null_hash);
    tx_to_json(&tx, &null_hash, result);
    transaction_free(&tx);
    return true;
}

static bool rpc_sendrawtransaction(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    RPC_HELP(help, result,
        "sendrawtransaction \"hexstring\" ( allowhighfees )\n"
        "Submits raw transaction to local node and network.\n"
        "Arguments:\n"
        "1. \"hexstring\" (string, required) The hex string of the raw tx\n"
        "2. allowhighfees (boolean, optional, default=false)");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hex_str = rpc_require_str(&p, 0, "hexstring");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct transaction tx;
    transaction_init(&tx);
    if (!decode_hex_tx(&tx, hex_str)) {
        transaction_free(&tx);
        json_set_str(result, "TX decode failed");
        return false;
    }

    transaction_compute_hash(&tx);
    struct uint256 hash = tx.hash;

    if (g_mp && tx_mempool_exists(g_mp, &hash)) {
        /* Already in mempool — re-relay to peers */
        if (g_connman)
            connman_relay_transaction(g_connman, &hash);
        char hex[65];
        uint256_get_hex(&hash, hex);
        json_set_str(result, hex);
        transaction_free(&tx);
        return true;
    }

    struct validation_state state;
    validation_state_init(&state);

    if (!check_transaction(&tx, &state)) {
        char msg[512];
        format_state_message(&state, msg, sizeof(msg));
        json_set_str(result, msg);
        transaction_free(&tx);
        return false;
    }

    if (g_mp) {
        int tip_height = active_chain_height(&g_ms->chain_active);
        uint32_t branch_id = consensus_current_epoch_branch_id(
            tip_height + 1, &chain_params_get()->consensus);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 0, (int64_t)time(NULL), 0.0,
                           (unsigned int)(tip_height + 1),
                           tx_mempool_has_no_inputs_of(g_mp, &tx),
                           false, branch_id);

        if (!tx_mempool_add_unchecked(g_mp, &hash, &entry)) {
            mempool_entry_free(&entry);
            json_set_str(result, "Failed to add to mempool");
            transaction_free(&tx);
            return false;
        }
    }

    /* Relay to peers */
    if (g_connman)
        connman_relay_transaction(g_connman, &hash);

    char hex[65];
    uint256_get_hex(&hash, hex);
    json_set_str(result, hex);
    transaction_free(&tx);
    return true;
}

static bool rpc_createrawtransaction(const struct json_value *params, bool help,
                                      struct json_value *result)
{
    RPC_HELP(help, result,
        "createrawtransaction [{\"txid\":\"id\",\"vout\":n},...] "
        "{\"address\":amount,...}\n"
        "Create a transaction spending the given inputs.\n"
        "Arguments:\n"
        "1. \"inputs\"  (array, required) JSON array of inputs\n"
        "2. \"outputs\" (object, required) JSON object of outputs");

    if (json_size(params) < 2) {
        json_set_str(result, "Missing required parameters: inputs and outputs");
        return false;
    }

    const struct json_value *inputs = json_at(params, 0);
    const struct json_value *outputs = json_at(params, 1);

    if (!inputs || inputs->type != JSON_ARR ||
        !outputs || outputs->type != JSON_OBJ) {
        json_set_str(result, "Invalid parameters");
        return false;
    }

    struct transaction tx;
    transaction_init(&tx);

    int tip_height = g_ms ?
        active_chain_height(&g_ms->chain_active) : 0;
    const struct consensus_params *cp = &chain_params_get()->consensus;
    int epoch = consensus_current_epoch(tip_height + 1, cp);

    if (epoch >= (int)UPGRADE_SAPLING) {
        tx.overwintered = true;
        tx.version = SAPLING_TX_VERSION;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
        tx.expiry_height = (uint32_t)(tip_height + 500);
    } else if (epoch >= (int)UPGRADE_OVERWINTER) {
        tx.overwintered = true;
        tx.version = OVERWINTER_TX_VERSION;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
        tx.expiry_height = (uint32_t)(tip_height + 500);
    } else {
        tx.version = 1;
    }

    for (size_t i = 0; i < json_size(inputs); i++) {
        const struct json_value *inp = json_at(inputs, i);
        if (!inp || inp->type != JSON_OBJ) continue;

        const struct json_value *txid_v = json_get(inp, "txid");
        const struct json_value *vout_v = json_get(inp, "vout");
        if (!txid_v || !vout_v) continue;

        struct tx_in vin;
        tx_in_init(&vin);
        parse_hash_str(json_get_str(txid_v), &vin.prevout.hash);
        vin.prevout.n = (uint32_t)json_get_int(vout_v);

        const struct json_value *seq_v = json_get(inp, "sequence");
        if (seq_v) vin.sequence = (uint32_t)json_get_int(seq_v);

        size_t new_count = tx.num_vin + 1;
        struct tx_in *new_vin = realloc(tx.vin, new_count * sizeof(struct tx_in));
        if (!new_vin) { transaction_free(&tx); return false; }
        tx.vin = new_vin;
        tx.vin[tx.num_vin] = vin;
        tx.num_vin = new_count;
    }

    for (size_t i = 0; i < json_size(outputs); i++) {
        if (!outputs->keys || !outputs->keys[i]) continue;
        const char *addr = outputs->keys[i];
        const struct json_value *amt_v = &outputs->children[i];

        struct tx_out vout;
        tx_out_set_null(&vout);

        int64_t amount = 0;
        if (amt_v->type == JSON_REAL) {
            double d = json_get_real(amt_v);
            if (d < 0 || d > 21000000.0) {
                json_set_str(result, "Amount out of range");
                transaction_free(&tx);
                return false;
            }
            amount = (int64_t)(d * 100000000.0);
        } else if (amt_v->type == JSON_INT) {
            int64_t v = json_get_int(amt_v);
            if (v < 0 || v > 21000000) {
                json_set_str(result, "Amount out of range");
                transaction_free(&tx);
                return false;
            }
            amount = v * 100000000;
        }
        vout.value = amount;

        const struct chain_params *cp2 = chain_params_get();
        size_t pk_len, sc_len;
        const unsigned char *pk_pfx = chain_params_base58_prefix(
            cp2, B58_PUBKEY_ADDRESS, &pk_len);
        const unsigned char *sc_pfx = chain_params_base58_prefix(
            cp2, B58_SCRIPT_ADDRESS, &sc_len);
        struct tx_destination dest;
        if (decode_destination(addr, pk_pfx, pk_len, sc_pfx, sc_len, &dest)) {
            script_for_destination(&vout.script_pub_key, &dest);
        }

        size_t new_count = tx.num_vout + 1;
        struct tx_out *new_vout = realloc(tx.vout,
                                          new_count * sizeof(struct tx_out));
        if (!new_vout) { transaction_free(&tx); return false; }
        tx.vout = new_vout;
        tx.vout[tx.num_vout] = vout;
        tx.num_vout = new_count;
    }

    char *hex = malloc(2 * 1024 * 1024);
    if (!hex) { transaction_free(&tx); return false; }
    size_t hex_len = encode_hex_tx(&tx, hex, 2 * 1024 * 1024);
    hex[hex_len] = '\0';
    json_set_str(result, hex);
    free(hex);
    transaction_free(&tx);
    return true;
}

static bool sign_one_input(struct transaction *tx, unsigned int idx,
                           const struct script *script_pub_key,
                           int64_t amount, uint32_t branch_id,
                           struct basic_keystore *ks)
{
    enum txnouttype type;
    unsigned char solutions[20][65];
    size_t solution_sizes[20];
    size_t num_solutions = 0;

    const struct script *signing_script = script_pub_key;
    struct script redeem;
    bool is_p2sh = false;

    if (!script_solver(script_pub_key, &type, solutions, solution_sizes,
                       &num_solutions))
        return false;

    if (type == TX_SCRIPTHASH) {
        struct uint160 script_hash;
        memcpy(script_hash.data, solutions[0], 20);
        if (!keystore_get_cscript(ks, &script_hash, &redeem))
            return false;
        is_p2sh = true;
        signing_script = &redeem;
        if (!script_solver(&redeem, &type, solutions, solution_sizes,
                           &num_solutions))
            return false;
    }

    struct sighash_type ht;
    ht.raw = SIGHASH_ALL;
    struct precomputed_tx_data txdata;
    precompute_tx_data(tx, &txdata);

    struct uint256 sighash;
    if (!signature_hash(signing_script, tx, idx, ht, amount,
                        branch_id, &txdata, &sighash))
        return false;

    struct script *ss = &tx->vin[idx].script_sig;
    ss->size = 0;

    if (type == TX_PUBKEYHASH) {
        struct key_id kid;
        memcpy(kid.id.data, solutions[0], 20);
        struct privkey skey;
        if (!keystore_get_key(ks, &kid, &skey))
            return false;
        struct pubkey spk;
        privkey_get_pubkey(&skey, &spk);

        unsigned char sig[SIGNATURE_SIZE + 1];
        size_t siglen = 0;
        if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
            memory_cleanse(skey.vch, 32);
            return false;
        }
        memory_cleanse(skey.vch, 32);
        sig[siglen++] = 0x01; /* SIGHASH_ALL */

        ss->data[ss->size++] = (unsigned char)siglen;
        memcpy(&ss->data[ss->size], sig, siglen);
        ss->size += siglen;
        ss->data[ss->size++] = (unsigned char)spk.size;
        memcpy(&ss->data[ss->size], spk.vch, spk.size);
        ss->size += spk.size;
    } else if (type == TX_MULTISIG) {
        int n_required = solutions[0][0] - 0x50;
        int n_keys = (int)num_solutions - 2;

        ss->data[ss->size++] = OP_0; /* dummy for CHECKMULTISIG bug */

        int sigs_added = 0;
        for (int k = 0; k < n_keys && sigs_added < n_required; k++) {
            struct pubkey pk;
            pubkey_set(&pk, solutions[k + 1], solution_sizes[k + 1]);
            struct key_id kid = pubkey_get_id(&pk);
            struct privkey skey;
            if (!keystore_get_key(ks, &kid, &skey))
                continue;

            unsigned char sig[SIGNATURE_SIZE + 1];
            size_t siglen = 0;
            if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
                memory_cleanse(skey.vch, 32);
                continue;
            }
            memory_cleanse(skey.vch, 32);
            sig[siglen++] = 0x01;

            ss->data[ss->size++] = (unsigned char)siglen;
            memcpy(&ss->data[ss->size], sig, siglen);
            ss->size += siglen;
            sigs_added++;
        }

        if (sigs_added < n_required)
            return false;
    } else if (type == TX_PUBKEY) {
        struct pubkey pk;
        pubkey_set(&pk, solutions[0], solution_sizes[0]);
        struct key_id kid = pubkey_get_id(&pk);
        struct privkey skey;
        if (!keystore_get_key(ks, &kid, &skey))
            return false;

        unsigned char sig[SIGNATURE_SIZE + 1];
        size_t siglen = 0;
        if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
            memory_cleanse(skey.vch, 32);
            return false;
        }
        memory_cleanse(skey.vch, 32);
        sig[siglen++] = 0x01;

        ss->data[ss->size++] = (unsigned char)siglen;
        memcpy(&ss->data[ss->size], sig, siglen);
        ss->size += siglen;
    } else {
        return false;
    }

    if (is_p2sh) {
        size_t push_len = redeem.size;
        if (push_len < 76) {
            ss->data[ss->size++] = (unsigned char)push_len;
        } else if (push_len <= 0xff) {
            ss->data[ss->size++] = OP_PUSHDATA1;
            ss->data[ss->size++] = (unsigned char)push_len;
        } else {
            ss->data[ss->size++] = OP_PUSHDATA2;
            ss->data[ss->size++] = (unsigned char)(push_len & 0xff);
            ss->data[ss->size++] = (unsigned char)(push_len >> 8);
        }
        memcpy(&ss->data[ss->size], redeem.data, push_len);
        ss->size += push_len;
    }

    return true;
}

static bool rpc_signrawtransaction(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    RPC_HELP(help, result,
        "signrawtransaction \"hexstring\" "
        "( [{\"txid\":\"id\",\"vout\":n,\"scriptPubKey\":\"hex\","
        "\"amount\":n},...] [\"privatekey\",...] sighashtype )\n"
        "Sign inputs for raw transaction.\n"
        "Arguments:\n"
        "1. \"hexstring\"   (string, required) The transaction hex\n"
        "2. \"prevtxs\"     (array, optional) Previous outputs being spent\n"
        "3. \"privkeys\"    (array, optional) Private keys for signing\n"
        "4. \"sighashtype\" (string, optional, default=ALL)");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 4);
    const char *hex_str = rpc_require_str(&p, 0, "hexstring");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct transaction tx;
    transaction_init(&tx);
    if (!decode_hex_tx(&tx, hex_str)) {
        transaction_free(&tx);
        json_set_str(result, "TX decode failed");
        return false;
    }

    /* Use wallet keystore directly — no copy needed.
     * Extra keys from param 3 are added to the wallet keystore. */
    struct basic_keystore *sign_ks = g_keystore;

    if (json_size(params) >= 3) {
        const struct json_value *privkeys = json_at(params, 2);
        if (privkeys && privkeys->type == JSON_ARR && sign_ks) {
            const struct chain_params *cp = chain_params_get();
            size_t sec_pfx_len;
            const unsigned char *sec_pfx = chain_params_base58_prefix(
                cp, B58_SECRET_KEY, &sec_pfx_len);
            for (size_t i = 0; i < json_size(privkeys); i++) {
                const struct json_value *kv = json_at(privkeys, i);
                if (!kv || kv->type != JSON_STR) continue;
                struct privkey pk;
                if (decode_secret(json_get_str(kv), sec_pfx, sec_pfx_len, &pk))
                    keystore_add_key(sign_ks, &pk);
                memory_cleanse(pk.vch, 32);
            }
        }
    }

    /* Collect prevout scriptPubKeys and amounts from param 2 */
    struct {
        struct uint256 txid;
        uint32_t vout;
        struct script script_pub_key;
        int64_t amount;
        bool valid;
    } prevouts[256];
    size_t num_prevouts = 0;

    if (json_size(params) >= 2) {
        const struct json_value *prev_arr = json_at(params, 1);
        if (prev_arr && prev_arr->type == JSON_ARR) {
            for (size_t i = 0; i < json_size(prev_arr) && num_prevouts < 256; i++) {
                const struct json_value *po = json_at(prev_arr, i);
                if (!po || po->type != JSON_OBJ) continue;

                const struct json_value *tid = json_get(po, "txid");
                const struct json_value *vn = json_get(po, "vout");
                const struct json_value *spk = json_get(po, "scriptPubKey");
                if (!tid || !vn || !spk) continue;

                parse_hash_str(json_get_str(tid), &prevouts[num_prevouts].txid);
                prevouts[num_prevouts].vout = (uint32_t)json_get_int(vn);

                const char *spk_hex = json_get_str(spk);
                if (!spk_hex) continue;
                size_t spk_len = strlen(spk_hex) / 2;
                if (spk_len > MAX_SCRIPT_SIZE) spk_len = MAX_SCRIPT_SIZE;
                unsigned char spk_bytes[MAX_SCRIPT_SIZE];
                ParseHex(spk_hex, spk_bytes, spk_len);
                prevouts[num_prevouts].script_pub_key.size = spk_len;
                memcpy(prevouts[num_prevouts].script_pub_key.data,
                       spk_bytes, spk_len);

                const struct json_value *amt = json_get(po, "amount");
                prevouts[num_prevouts].amount = amt ?
                    (int64_t)(json_get_real(amt) * 100000000.0 + 0.5) : 0;
                prevouts[num_prevouts].valid = true;

                /* If prevout has redeemScript, add it to keystore */
                const struct json_value *rs = json_get(po, "redeemScript");
                if (rs && rs->type == JSON_STR) {
                    struct script redeem;
                    const char *rs_hex = json_get_str(rs);
                    if (!rs_hex) continue;
                    size_t rs_len = strlen(rs_hex) / 2;
                    if (rs_len > MAX_SCRIPT_SIZE) rs_len = MAX_SCRIPT_SIZE;
                    ParseHex(rs_hex, redeem.data, rs_len);
                    redeem.size = rs_len;
                    keystore_add_cscript(sign_ks, &redeem);
                }

                num_prevouts++;
            }
        }
    }

    int tip_height = g_ms ?
        active_chain_height(&g_ms->chain_active) : 0;
    uint32_t branch_id = consensus_current_epoch_branch_id(
        tip_height + 1, &chain_params_get()->consensus);

    bool all_complete = true;
    struct json_value errors = {0};
    json_set_array(&errors);

    for (unsigned int i = 0; i < tx.num_vin; i++) {
        const struct script *prev_script = NULL;
        int64_t prev_amount = 0;

        /* Find scriptPubKey for this input */
        for (size_t j = 0; j < num_prevouts; j++) {
            if (prevouts[j].valid &&
                uint256_cmp(&tx.vin[i].prevout.hash, &prevouts[j].txid) == 0 &&
                tx.vin[i].prevout.n == prevouts[j].vout) {
                prev_script = &prevouts[j].script_pub_key;
                prev_amount = prevouts[j].amount;
                break;
            }
        }

        /* Try SQLite UTXO index first (instant) */
        if (!prev_script && g_node_db && g_node_db->open) {
            struct db_utxo u;
            if (db_utxo_find(g_node_db, tx.vin[i].prevout.hash.data,
                             tx.vin[i].prevout.n, &u) && num_prevouts < 256) {
                prevouts[num_prevouts].txid = tx.vin[i].prevout.hash;
                prevouts[num_prevouts].vout = tx.vin[i].prevout.n;
                prevouts[num_prevouts].script_pub_key.size = u.script_len;
                if (u.script_len <= MAX_SCRIPT_SIZE)
                    memcpy(prevouts[num_prevouts].script_pub_key.data,
                           u.script, u.script_len);
                prevouts[num_prevouts].amount = u.value;
                prevouts[num_prevouts].valid = true;
                prev_script = &prevouts[num_prevouts].script_pub_key;
                prev_amount = prevouts[num_prevouts].amount;
                num_prevouts++;
                db_utxo_free(&u);
            }
        }

        /* Fall back to coins DB (LevelDB) */
        if (!prev_script && g_coins_tip) {
            struct coins entry;
            coins_init(&entry);
            if (coins_view_cache_get_coins(g_coins_tip,
                    &tx.vin[i].prevout.hash, &entry)) {
                if (tx.vin[i].prevout.n < entry.num_vout &&
                    !tx_out_is_null(&entry.vout[tx.vin[i].prevout.n])) {
                    if (num_prevouts < 256) {
                        prevouts[num_prevouts].txid = tx.vin[i].prevout.hash;
                        prevouts[num_prevouts].vout = tx.vin[i].prevout.n;
                        prevouts[num_prevouts].script_pub_key =
                            entry.vout[tx.vin[i].prevout.n].script_pub_key;
                        prevouts[num_prevouts].amount =
                            entry.vout[tx.vin[i].prevout.n].value;
                        prevouts[num_prevouts].valid = true;
                        prev_script = &prevouts[num_prevouts].script_pub_key;
                        prev_amount = prevouts[num_prevouts].amount;
                        num_prevouts++;
                    }
                }
                coins_free(&entry);
            }
        }

        if (!prev_script) {
            struct json_value err = {0};
            json_set_object(&err);
            json_push_kv_int(&err, "vout", (int64_t)i);
            json_push_kv_str(&err, "error", "Input not found or already spent");
            json_push_back(&errors, &err);
            json_free(&err);
            all_complete = false;
            continue;
        }


        if (!sign_one_input(&tx, i, prev_script, prev_amount,
                            branch_id, sign_ks)) {
            struct json_value err = {0};
            json_set_object(&err);
            json_push_kv_int(&err, "vout", (int64_t)i);
            json_push_kv_str(&err, "error", "Unable to sign input");
            json_push_back(&errors, &err);
            json_free(&err);
            all_complete = false;
        }
    }

    transaction_compute_hash(&tx);

    json_set_object(result);
    char *hex = malloc(2 * 1024 * 1024);
    if (hex) {
        size_t hex_len = encode_hex_tx(&tx, hex, 2 * 1024 * 1024);
        hex[hex_len] = '\0';
        json_push_kv_str(result, "hex", hex);
        free(hex);
    }
    json_push_kv_bool(result, "complete", all_complete);
    if (!all_complete)
        json_push_kv(result, "errors", &errors);

    json_free(&errors);
    transaction_free(&tx);
    return true;
}

void register_rawtransaction_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "rawtransactions", "getrawtransaction",
          rpc_getrawtransaction, true },
        { "rawtransactions", "decoderawtransaction",
          rpc_decoderawtransaction, true },
        { "rawtransactions", "sendrawtransaction",
          rpc_sendrawtransaction, false },
        { "rawtransactions", "createrawtransaction",
          rpc_createrawtransaction, false },
        { "rawtransactions", "signrawtransaction",
          rpc_signrawtransaction, false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
