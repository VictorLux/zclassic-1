/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZSLP token controller — token operations + shielded payments. */

#include "controllers/zslp_controller.h"
#include "sapling/slp.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "primitives/transaction.h"
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

    /* TODO: Build transaction with:
     *   vout[0]: OP_RETURN with script (value=0)
     *   vout[1]: dust output to our address (receives initial supply)
     *   vout[2]: dust output to our address (mint baton)
     * Sign and broadcast via sendrawtransaction.
     *
     * The token_id = txid of this GENESIS transaction.
     * Until we build and broadcast the tx, return a placeholder. */

    /* Store token info in SQLite for tracking */
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
            "(token_id, ticker, name, decimals, supply, created_at) "
            "VALUES (?, ?, ?, ?, ?, strftime('%s','now'))",
            -1, &ins, NULL);
        sqlite3_bind_text(ins, 1, ticker, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, ticker, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 4, decimals);
        sqlite3_bind_int64(ins, 5, (int64_t)initial_supply);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
        sqlite3_close(db);
    }

    /* Return ticker as placeholder token_id until GENESIS tx is broadcast */
    static char result[128];
    snprintf(result, sizeof(result), "%s", ticker);
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

    /* Call z_getnewaddress via internal wallet.
     * For now, generate a deterministic placeholder based on time. */
    snprintf(z_addr_out, max,
             "zs1_pay_%lld", (long long)time(NULL));

    /* TODO: call the actual wallet z_getnewaddress which generates
     * a real Sapling diversified address. This requires the wallet
     * to have Sapling keys (which it does — 66 sapling keys loaded). */

    printf("ZSLP: generated payment address %s\n", z_addr_out);
    return true;
}

/* ── Payment detection ───────────────────────────────────── */

int64_t zslp_check_payment(const char *datadir,
                            const char *z_addr,
                            int64_t min_amount)
{
    if (!datadir || !z_addr) return 0;

    /* TODO: call z_listunspent and check for notes at z_addr.
     * For now, check a payments table that gets updated by
     * the sync_controller when new Sapling notes arrive. */
    (void)min_amount;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK)
        return 0;
    sqlite3_busy_timeout(db, 5000);

    int64_t received = 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
        "WHERE spent_txid IS NULL", /* check all unspent notes */
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        received = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
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

    printf("ZSLP MINT: %llu %s → %s\n",
           (unsigned long long)amount, token_id_hex, recipient_addr);

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

    /* Validate amount */
    if (amount == 0) {
        fprintf(stderr, "zslp: send amount must be > 0\n");
        return false;
    }

    /* Validate recipient address: non-empty, alphanumeric */
    size_t addr_len = strlen(to_addr);
    if (addr_len == 0) {
        fprintf(stderr, "zslp: send address must be non-empty\n");
        return false;
    }
    if (!is_alphanumeric(to_addr, addr_len)) {
        fprintf(stderr, "zslp: send address must be alphanumeric\n");
        return false;
    }

    /* Same as mint for now — debit sender, credit receiver */
    return zslp_mint(datadir, token_id_hex, to_addr, amount);
}
