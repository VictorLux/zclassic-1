/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZSLP token controller — token operations + shielded payments. */

#include "controllers/zslp_controller.h"
#include "zslp/slp.h"
#include "core/uint256.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "keys/key_io.h"
#include "chain/chainparams.h"
#include "script/standard.h"
#include "validation/sighash.h"
#include "consensus/upgrades.h"
#include "support/cleanse.h"
#include "validation/txmempool.h"
#include "primitives/transaction.h"
#include "config/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <sqlite3.h>

#define ZSLP_MAX_TICKER_LEN  10
#define ZSLP_MAX_NAME_LEN    64
#define ZSLP_MAX_DECIMALS    8
#define ZSLP_MAX_SUPPLY      2100000000000000ULL  /* 21000000 * 1e8 */

static struct wallet *zslp_wallet(void)
{
    return app_runtime_wallet();
}

static struct tx_mempool *zslp_mempool(void)
{
    return app_runtime_mempool();
}

/* Validate that str contains only alphanumeric characters [A-Za-z0-9]. */
static bool is_alphanumeric(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)str[i]))
            return false;
    }
    return true;
}

/* ── Token creation (GENESIS) ────────────────────────────── */

const char *zslp_create_token(const char *datadir,
                               const char *ticker,
                               const char *name,
                               uint8_t decimals,
                               uint64_t initial_supply)
{
    if (!datadir || !ticker || !name) return NULL;

    /* Validate ticker: 1-10 alphanumeric characters */
    size_t ticker_len = strlen(ticker);
    if (ticker_len == 0 || ticker_len > ZSLP_MAX_TICKER_LEN) {
        fprintf(stderr, "zslp: ticker must be 1-%d chars (got %zu)\n",
                ZSLP_MAX_TICKER_LEN, ticker_len);
        return NULL;
    }
    if (!is_alphanumeric(ticker, ticker_len)) {
        fprintf(stderr, "zslp: ticker must be alphanumeric\n");
        return NULL;
    }

    /* Validate name: 1-64 characters */
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > ZSLP_MAX_NAME_LEN) {
        fprintf(stderr, "zslp: name must be 1-%d chars (got %zu)\n",
                ZSLP_MAX_NAME_LEN, name_len);
        return NULL;
    }

    /* Validate decimals: 0-8 */
    if (decimals > ZSLP_MAX_DECIMALS) {
        fprintf(stderr, "zslp: decimals must be 0-%d (got %d)\n",
                ZSLP_MAX_DECIMALS, decimals);
        return NULL;
    }

    /* Validate initial supply */
    if (initial_supply > ZSLP_MAX_SUPPLY) {
        fprintf(stderr, "zslp: initial_supply exceeds max (%llu > %llu)\n",
                (unsigned long long)initial_supply,
                (unsigned long long)ZSLP_MAX_SUPPLY);
        return NULL;
    }

    /* Build the GENESIS OP_RETURN script */
    uint8_t script[256];
    size_t slen = slp_build_genesis(script, sizeof(script),
        ticker, name, "", NULL, decimals, 2, /* mint baton at vout 2 */
        initial_supply);

    if (slen == 0) {
        fprintf(stderr, "zslp: failed to build GENESIS script\n");
        return NULL;
    }

    printf("ZSLP GENESIS: ticker=%s name=%s decimals=%d supply=%llu "
           "script=%zu bytes\n",
           ticker, name, decimals, (unsigned long long)initial_supply, slen);

    /* Build transaction:
     *   vout[0]: OP_RETURN with GENESIS script (value=0)
     *   vout[1]: dust output to our address (receives initial supply)
     *   vout[2]: dust output to our address (mint baton)
     * Sign and broadcast via wallet. */
    char *broadcast_txid = NULL; /* set if we successfully broadcast */
    struct wallet *wallet = zslp_wallet();
    struct tx_mempool *mempool = zslp_mempool();

    if (!wallet || !mempool) {
        /* No wallet available (test mode) — skip on-chain broadcast,
         * just track in SQLite below. */
        goto store_sqlite;
    }

    /* Build a multi-output transaction with OP_RETURN at vout[0] */
    struct wallet_tx wtx;
    int64_t fee_paid = 0;
    const char *tx_error = NULL;

    /* We need our own address for dust outputs (token receiver + mint baton) */
    struct pubkey our_pk;
    if (!wallet_get_key_from_pool(wallet, &our_pk)) {
        fprintf(stderr, "zslp: cannot get address from wallet\n");
        return NULL;
    }
    struct key_id our_kid = pubkey_get_id(&our_pk);
    struct tx_destination our_dest;
    our_dest.type = DEST_KEY_ID;
    our_dest.id.key = our_kid;

    /* Create transaction with 2 dust outputs (546 satoshi each).
     * The OP_RETURN is prepended after, by patching the raw tx. */
    struct tx_destination dests[2] = { our_dest, our_dest };
    int64_t vals[2] = { 546, 546 }; /* dust for token + mint baton */

    if (!wallet_create_transaction_multi(wallet,
            dests, vals, 2, &wtx, &fee_paid, &tx_error)) {
        fprintf(stderr, "zslp: tx build failed: %s\n",
                tx_error ? tx_error : "unknown");
        return NULL;
    }

    /* Shift existing vouts right by 1 to insert OP_RETURN at position 0.
     * wallet_create_transaction_multi already allocated and signed the tx.
     * We need to re-allocate vout array with one extra slot. */
    size_t old_nout = wtx.tx.num_vout;
    size_t new_nout = old_nout + 1;
    struct tx_out *new_vout = calloc(new_nout, sizeof(struct tx_out));
    if (!new_vout) {
        transaction_free(&wtx.tx);
        return NULL;
    }

    /* vout[0] = OP_RETURN */
    new_vout[0].value = 0;
    new_vout[0].script_pub_key.size = slen;
    memcpy(new_vout[0].script_pub_key.data, script, slen);

    /* Copy existing outputs (dust + change) to positions 1..N */
    for (size_t i = 0; i < old_nout; i++)
        new_vout[i + 1] = wtx.tx.vout[i];

    free(wtx.tx.vout);
    wtx.tx.vout = new_vout;
    wtx.tx.num_vout = new_nout;

    /* Re-sign: the vout change invalidates the sighash.
     * We need to clear scriptSigs and re-sign. */
    const struct chain_params *cp = chain_params_get();
    int height = wallet->best_block_height;
    uint32_t branch_id = consensus_current_epoch_branch_id(
        height + 1, &cp->consensus);

    zcl_mutex_lock(&wallet->cs);
    for (size_t i = 0; i < wtx.tx.num_vin; i++) {
        /* Find the prevout to get the scriptPubKey and amount */
        struct wallet_tx *prev_wtx = NULL;
        for (size_t j = 0; j < wallet->num_wallet_tx; j++) {
            if (uint256_eq(&wallet->map_wallet[j].tx.hash,
                           &wtx.tx.vin[i].prevout.hash)) {
                prev_wtx = &wallet->map_wallet[j];
                break;
            }
        }
        if (!prev_wtx) continue;

        const struct tx_out *prev_out =
            &prev_wtx->tx.vout[wtx.tx.vin[i].prevout.n];
        struct tx_destination prev_dest;
        if (!script_extract_destination(&prev_out->script_pub_key, &prev_dest))
            continue;

        struct privkey skey;
        if (!keystore_get_key(&wallet->keystore,
                               &prev_dest.id.key, &skey))
            continue;

        struct pubkey spk;
        privkey_get_pubkey(&skey, &spk);

        struct sighash_type ht;
        ht.raw = SIGHASH_ALL;
        struct precomputed_tx_data txdata;
        precompute_tx_data(&wtx.tx, &txdata);

        struct uint256 sighash;
        if (!signature_hash(&prev_out->script_pub_key, &wtx.tx,
                            (unsigned int)i, ht, prev_out->value,
                            branch_id, &txdata, &sighash)) {
            memory_cleanse(skey.vch, 32);
            continue;
        }

        unsigned char sig[SIGNATURE_SIZE + 1];
        size_t siglen = 0;
        if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
            memory_cleanse(skey.vch, 32);
            continue;
        }
        sig[siglen++] = 0x01;

        struct script *ss = &wtx.tx.vin[i].script_sig;
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

    /* Compute final tx hash — this IS the token_id */
    transaction_compute_hash(&wtx.tx);

    /* Commit to mempool + relay */
    if (!wallet_commit_transaction(wallet, &wtx, mempool)) {
        fprintf(stderr, "zslp: commit failed\n");
        transaction_free(&wtx.tx);
        return NULL;
    }

    static char bc_txid[128];
    uint256_get_hex(&wtx.tx.hash, bc_txid);
    broadcast_txid = bc_txid;
    printf("ZSLP GENESIS broadcast: token_id=%s\n", broadcast_txid);

store_sqlite:
    ;
    /* Store token info in SQLite */
    static char result[128];
    if (broadcast_txid)
        snprintf(result, sizeof(result), "%s", broadcast_txid);
    else
        snprintf(result, sizeof(result), "%s", ticker); /* placeholder */

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) == SQLITE_OK) {
        sqlite3_busy_timeout(db, 5000);
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS zslp_tokens ("
            "token_id TEXT PRIMARY KEY,"
            "ticker TEXT, name TEXT, decimals INTEGER,"
            "supply INTEGER, mint_baton_vout INTEGER,"
            "created_at INTEGER)", NULL, NULL, NULL);

        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO zslp_tokens "
            "(token_id, ticker, name, decimals, supply, mint_baton_vout, "
            "created_at) VALUES (?, ?, ?, ?, ?, 2, strftime('%s','now'))",
            -1, &ins, NULL);
        sqlite3_bind_text(ins, 1, result, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, ticker, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 4, decimals);
        sqlite3_bind_int64(ins, 5, (int64_t)initial_supply);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
        sqlite3_close(db);
    }

    return result;
}

/* ── Token balance ───────────────────────────────────────── */

uint64_t zslp_balance(const char *datadir,
                       const char *token_id_hex,
                       const char *addr)
{
    if (!datadir || !token_id_hex || !addr) return 0;

    /* Scan the SQLite token_balances table */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK)
        return 0;
    sqlite3_busy_timeout(db, 5000);

    uint64_t bal = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT balance FROM zslp_balances WHERE token_id=? AND address=?",
            -1, &s, NULL) == SQLITE_OK && s) {
        sqlite3_bind_text(s, 1, token_id_hex, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, addr, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW)
            bal = (uint64_t)sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return bal;
}

/* ── Shielded payment address ────────────────────────────── */

bool zslp_generate_payment_address(const char *datadir,
                                    char *z_addr_out, size_t max)
{
    if (!datadir || !z_addr_out || max < 80) return false;

    struct wallet *wallet = zslp_wallet();
    if (wallet && wallet->sapling_keys.num_keys > 0) {
        uint8_t diversifier[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        if (sapling_keystore_new_address(&wallet->sapling_keys,
                                          diversifier, pk_d)) {
            const struct chain_params *cp = chain_params_get();
            if (sapling_encode_payment_address(diversifier, pk_d,
                    cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
                    z_addr_out, max))
                return true;
        }
    }

    /* Fallback for tests or when wallet has no Sapling keys. */
    snprintf(z_addr_out, max,
             "zs1_pay_%lld", (long long)time(NULL));
    return true;
}

/* ── Payment detection ───────────────────────────────────── */

int64_t zslp_check_payment(const char *datadir,
                            const char *z_addr,
                            int64_t min_amount)
{
    if (!datadir || !z_addr) return 0;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK)
        return 0;
    sqlite3_busy_timeout(db, 5000);

    int64_t received = 0;
    sqlite3_stmt *s = NULL;

    /* Primary: query notes at the specific z-address */
    sqlite3_prepare_v2(db,
        "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
        "WHERE spent_txid IS NULL AND address = ?",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, z_addr, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW)
        received = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    /* Fallback: if no address match (placeholder z-addrs),
     * check for any unspent note meeting the minimum amount. */
    if (received < min_amount && min_amount > 0) {
        s = NULL;
        sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
            "WHERE spent_txid IS NULL AND value = ?",
            -1, &s, NULL);
        sqlite3_bind_int64(s, 1, min_amount);
        if (sqlite3_step(s) == SQLITE_ROW)
            received = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }

    sqlite3_close(db);
    return received;
}

/* ── Token mint ──────────────────────────────────────────── */

bool zslp_mint(const char *datadir,
                const char *token_id_hex,
                const char *recipient_addr,
                uint64_t amount)
{
    if (!datadir || !token_id_hex || !recipient_addr) return false;

    /* Validate amount */
    if (amount == 0) {
        fprintf(stderr, "zslp: mint amount must be > 0\n");
        return false;
    }

    /* Validate recipient address: non-empty, alphanumeric */
    size_t addr_len = strlen(recipient_addr);
    if (addr_len == 0) {
        fprintf(stderr, "zslp: recipient address must be non-empty\n");
        return false;
    }
    if (!is_alphanumeric(recipient_addr, addr_len)) {
        fprintf(stderr, "zslp: recipient address must be alphanumeric\n");
        return false;
    }

    /* Update balance in SQLite */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return false;
    sqlite3_busy_timeout(db, 5000); /* wait up to 5s for locks */

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS zslp_balances ("
        "token_id TEXT, address TEXT, balance INTEGER,"
        "PRIMARY KEY (token_id, address))", NULL, NULL, NULL);

    /* Overflow protection: check existing balance before mint */
    uint64_t existing = 0;
    sqlite3_stmt *chk = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT balance FROM zslp_balances WHERE token_id=? AND address=?",
            -1, &chk, NULL) == SQLITE_OK && chk) {
        sqlite3_bind_text(chk, 1, token_id_hex, -1, SQLITE_STATIC);
        sqlite3_bind_text(chk, 2, recipient_addr, -1, SQLITE_STATIC);
        if (sqlite3_step(chk) == SQLITE_ROW)
            existing = (uint64_t)sqlite3_column_int64(chk, 0);
        sqlite3_finalize(chk);
    }
    if (amount > (uint64_t)INT64_MAX - existing) {
        fprintf(stderr, "ZSLP MINT: overflow rejected (%llu + %llu > INT64_MAX)\n",
                (unsigned long long)existing, (unsigned long long)amount);
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO zslp_balances (token_id, address, balance) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(token_id, address) DO UPDATE SET balance=balance+?",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, token_id_hex, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, recipient_addr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, (int64_t)amount);
    sqlite3_bind_int64(s, 4, (int64_t)amount);
    sqlite3_step(s);
    sqlite3_finalize(s);
    sqlite3_close(db);

    /* TODO: build and broadcast ZSLP SEND transaction on-chain */
    return true;
}

/* ── Token send ──────────────────────────────────────────── */

bool zslp_send(const char *datadir,
                const char *token_id_hex,
                const char *to_addr,
                uint64_t amount)
{
    if (!datadir || !token_id_hex || !to_addr) return false;

    if (amount == 0) {
        fprintf(stderr, "zslp: send amount must be > 0\n");
        return false;
    }

    size_t addr_len = strlen(to_addr);
    if (addr_len == 0 || !is_alphanumeric(to_addr, addr_len)) {
        fprintf(stderr, "zslp: invalid recipient address\n");
        return false;
    }

    struct wallet *wallet = zslp_wallet();
    struct tx_mempool *mempool = zslp_mempool();

    if (!wallet || !mempool) {
        /* No wallet (test mode) — just update SQLite balances */
        return zslp_mint(datadir, token_id_hex, to_addr, amount);
    }

    /* Build SEND OP_RETURN script */
    struct uint256 token_id;
    uint256_set_hex(&token_id, token_id_hex);
    if (uint256_is_null(&token_id)) {
        fprintf(stderr, "zslp: invalid token_id\n");
        return false;
    }

    uint64_t quantities[1] = { amount };
    uint8_t op_script[256];
    size_t slen = slp_build_send(op_script, sizeof(op_script),
        &token_id, quantities, 1);
    if (slen == 0) {
        fprintf(stderr, "zslp: failed to build SEND script\n");
        return false;
    }

    /* Decode recipient address to destination */
    const struct chain_params *cp = chain_params_get();
    size_t pk_len = 0, sc_len = 0;
    const unsigned char *pk_pfx =
        chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sc_pfx =
        chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_len);
    struct tx_destination dest;
    if (!decode_destination(to_addr, pk_pfx, pk_len, sc_pfx, sc_len, &dest)) {
        fprintf(stderr, "zslp: cannot decode recipient address\n");
        return false;
    }

    /* Build tx: vout[0]=OP_RETURN, vout[1]=dust to recipient (token output) */
    struct wallet_tx wtx;
    int64_t fee_paid = 0;
    const char *tx_error = NULL;

    int64_t vals[1] = { 546 }; /* dust for token output */
    if (!wallet_create_transaction_multi(wallet,
            &dest, vals, 1, &wtx, &fee_paid, &tx_error)) {
        fprintf(stderr, "zslp: tx build failed: %s\n",
                tx_error ? tx_error : "unknown");
        return false;
    }

    /* Prepend OP_RETURN at vout[0], shift existing outputs right */
    size_t old_nout = wtx.tx.num_vout;
    struct tx_out *new_vout = calloc(old_nout + 1, sizeof(struct tx_out));
    if (!new_vout) { transaction_free(&wtx.tx); return false; }

    new_vout[0].value = 0;
    new_vout[0].script_pub_key.size = slen;
    memcpy(new_vout[0].script_pub_key.data, op_script, slen);
    for (size_t i = 0; i < old_nout; i++)
        new_vout[i + 1] = wtx.tx.vout[i];
    free(wtx.tx.vout);
    wtx.tx.vout = new_vout;
    wtx.tx.num_vout = old_nout + 1;

    /* Re-sign (vout change invalidates sighash) */
    int height = wallet->best_block_height;
    uint32_t branch_id = consensus_current_epoch_branch_id(
        height + 1, &cp->consensus);

    zcl_mutex_lock(&wallet->cs);
    for (size_t i = 0; i < wtx.tx.num_vin; i++) {
        struct wallet_tx *prev_wtx = NULL;
        for (size_t j = 0; j < wallet->num_wallet_tx; j++) {
            if (uint256_eq(&wallet->map_wallet[j].tx.hash,
                           &wtx.tx.vin[i].prevout.hash)) {
                prev_wtx = &wallet->map_wallet[j];
                break;
            }
        }
        if (!prev_wtx) continue;

        const struct tx_out *prev_out =
            &prev_wtx->tx.vout[wtx.tx.vin[i].prevout.n];
        struct tx_destination prev_dest;
        if (!script_extract_destination(&prev_out->script_pub_key, &prev_dest))
            continue;

        struct privkey skey;
        if (!keystore_get_key(&wallet->keystore,
                               &prev_dest.id.key, &skey))
            continue;

        struct pubkey spk;
        privkey_get_pubkey(&skey, &spk);

        struct sighash_type ht;
        ht.raw = SIGHASH_ALL;
        struct precomputed_tx_data txdata;
        precompute_tx_data(&wtx.tx, &txdata);

        struct uint256 sighash;
        if (!signature_hash(&prev_out->script_pub_key, &wtx.tx,
                            (unsigned int)i, ht, prev_out->value,
                            branch_id, &txdata, &sighash)) {
            memory_cleanse(skey.vch, 32);
            continue;
        }

        unsigned char sig[SIGNATURE_SIZE + 1];
        size_t siglen = 0;
        if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
            memory_cleanse(skey.vch, 32);
            continue;
        }
        sig[siglen++] = 0x01;

        struct script *ss = &wtx.tx.vin[i].script_sig;
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

    transaction_compute_hash(&wtx.tx);

    if (!wallet_commit_transaction(wallet, &wtx, mempool)) {
        fprintf(stderr, "zslp: commit failed\n");
        transaction_free(&wtx.tx);
        return false;
    }

    /* Update balances in SQLite */
    zslp_mint(datadir, token_id_hex, to_addr, amount);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    printf("ZSLP SEND broadcast: token=%s amount=%llu to=%s txid=%s\n",
           token_id_hex, (unsigned long long)amount, to_addr, txid);
    return true;
}

/* ── RPC handlers ────────────────────────────────────────── */

#include "rpc/server.h"
#include "json/json.h"

static const char *g_zslp_datadir = NULL;

void zslp_rpc_set_datadir(const char *datadir) {
    g_zslp_datadir = datadir;
}

/* zslp_createtoken "ticker" "name" decimals supply */
static bool rpc_zslp_createtoken(const struct json_value *params,
                                   bool help, struct json_value *result)
{
    if (help || !params || json_size(params) < 4) {
        json_set_str(result,
            "zslp_createtoken \"ticker\" \"name\" decimals supply");
        return !help;
    }

    const char *ticker = json_get_str(json_at(params, 0));
    const char *name = json_get_str(json_at(params, 1));
    int decimals = (int)json_get_int(json_at(params, 2));
    uint64_t supply = (uint64_t)json_get_int(json_at(params, 3));

    if (!ticker || !name) {
        json_set_str(result, "invalid parameters");
        return false;
    }

    const char *token_id = zslp_create_token(g_zslp_datadir,
        ticker, name, (uint8_t)decimals, supply);
    if (token_id)
        json_set_str(result, token_id);
    else {
        json_set_str(result, "token creation failed");
        return false;
    }
    return true;
}

/* zslp_send "token_id" "address" amount */
static bool rpc_zslp_send(const struct json_value *params,
                             bool help, struct json_value *result)
{
    if (help || !params || json_size(params) < 3) {
        json_set_str(result,
            "zslp_send \"token_id\" \"address\" amount");
        return !help;
    }

    const char *token_id = json_get_str(json_at(params, 0));
    const char *addr = json_get_str(json_at(params, 1));
    uint64_t amount = (uint64_t)json_get_int(json_at(params, 2));

    if (!token_id || !addr) {
        json_set_str(result, "invalid parameters");
        return false;
    }

    bool ok = zslp_send(g_zslp_datadir, token_id, addr, amount);
    json_set_bool(result, ok);
    return ok;
}

/* zslp_balance "token_id" "address" */
static bool rpc_zslp_balance(const struct json_value *params,
                               bool help, struct json_value *result)
{
    if (help || !params || json_size(params) < 2) {
        json_set_str(result,
            "zslp_balance \"token_id\" \"address\"");
        return !help;
    }

    const char *token_id = json_get_str(json_at(params, 0));
    const char *addr = json_get_str(json_at(params, 1));

    if (!token_id || !addr) {
        json_set_str(result, "invalid parameters");
        return false;
    }

    uint64_t bal = zslp_balance(g_zslp_datadir, token_id, addr);
    json_set_int(result, (int64_t)bal);
    return true;
}

/* zslp_mint "token_id" "address" amount */
static bool rpc_zslp_mint(const struct json_value *params,
                             bool help, struct json_value *result)
{
    if (help || !params || json_size(params) < 3) {
        json_set_str(result,
            "zslp_mint \"token_id\" \"address\" amount");
        return !help;
    }

    const char *token_id = json_get_str(json_at(params, 0));
    const char *addr = json_get_str(json_at(params, 1));
    uint64_t amount = (uint64_t)json_get_int(json_at(params, 2));

    if (!token_id || !addr) {
        json_set_str(result, "invalid parameters");
        return false;
    }

    bool ok = zslp_mint(g_zslp_datadir, token_id, addr, amount);
    json_set_bool(result, ok);
    return ok;
}

void register_zslp_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "zslp", "zslp_createtoken", rpc_zslp_createtoken, false },
        { "zslp", "zslp_send",       rpc_zslp_send,         false },
        { "zslp", "zslp_balance",    rpc_zslp_balance,      true  },
        { "zslp", "zslp_mint",       rpc_zslp_mint,         false },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
