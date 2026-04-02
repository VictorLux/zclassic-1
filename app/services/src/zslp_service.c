/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP application service — validation and persistence helpers. */

#include "services/zslp_service.h"
#include "config/runtime.h"
#include "models/database.h"
#include "models/zslp.h"
#include "chain/chainparams.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ZSLP_MAX_TOKEN_KEY_LEN 64
#define ZSLP_MAX_TICKER_LEN 10
#define ZSLP_MAX_NAME_LEN 64
#define ZSLP_MAX_DECIMALS 8
#define ZSLP_MAX_SUPPLY 2100000000000000ULL

static void zslp_service_canonicalize_token_key(const char *src,
                                                char dest[ZSLP_MAX_TOKEN_KEY_LEN + 1])
{
    size_t i = 0;
    if (!dest)
        return;
    dest[0] = '\0';
    if (!src)
        return;
    for (; src[i] && i < ZSLP_MAX_TOKEN_KEY_LEN; i++)
        dest[i] = (char)toupper((unsigned char)src[i]);
    dest[i] = '\0';
}

static void zslp_service_wrap_sqlite(sqlite3 *db, struct node_db *ndb)
{
    memset(ndb, 0, sizeof(*ndb));
    ndb->db = db;
    ndb->open = (db != NULL);
}

bool zslp_service_is_alphanumeric(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)str[i]))
            return false;
    }
    return true;
}

bool zslp_service_is_hex_string(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)str[i]))
            return false;
    }
    return true;
}

bool zslp_service_validate_token_key(const char *token_key)
{
    size_t len;
    if (!token_key)
        return false;
    len = strlen(token_key);
    if (len == 0 || len > ZSLP_MAX_TOKEN_KEY_LEN)
        return false;
    return zslp_service_is_alphanumeric(token_key, len) ||
           (len == 64 && zslp_service_is_hex_string(token_key, len));
}

bool zslp_service_decode_transparent_destination(const char *addr,
                                                 struct tx_destination *dest)
{
    const struct chain_params *cp;
    size_t pk_len = 0, sc_len = 0;
    const unsigned char *pk_pfx;
    const unsigned char *sc_pfx;

    if (!addr || !dest)
        return false;

    cp = chain_params_get();
    pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    sc_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_len);
    return decode_destination(addr, pk_pfx, pk_len, sc_pfx, sc_len, dest);
}

bool zslp_service_validate_recipient_addr(const char *addr,
                                          bool strict_chain_addr)
{
    size_t len;
    struct tx_destination dest;

    if (!addr)
        return false;
    len = strlen(addr);
    if (len == 0 || len > 128)
        return false;
    if (strict_chain_addr)
        return zslp_service_decode_transparent_destination(addr, &dest);
    return zslp_service_is_alphanumeric(addr, len) ||
           zslp_service_decode_transparent_destination(addr, &dest);
}

const char *zslp_service_validate_create_request(
    const struct zslp_token_create_request *req)
{
    size_t ticker_len, name_len;

    if (!req)
        return "request is required";
    if (!req->ticker || !req->name)
        return "ticker and name are required";

    ticker_len = strlen(req->ticker);
    if (ticker_len == 0 || ticker_len > ZSLP_MAX_TICKER_LEN)
        return "ticker must be 1-10 alphanumeric characters";
    if (!zslp_service_is_alphanumeric(req->ticker, ticker_len))
        return "ticker must be alphanumeric";

    name_len = strlen(req->name);
    if (name_len == 0 || name_len > ZSLP_MAX_NAME_LEN)
        return "name must be 1-64 printable characters";
    if (!zslp_service_is_alphanumeric(req->name, name_len) &&
        strspn(req->name,
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_./")
            != name_len)
        return "name contains unsupported characters";

    if (req->decimals > ZSLP_MAX_DECIMALS)
        return "decimals must be between 0 and 8";
    if (req->initial_supply > ZSLP_MAX_SUPPLY)
        return "initial supply exceeds maximum";
    return NULL;
}

const char *zslp_service_validate_transfer_request(
    const struct zslp_token_transfer_request *req)
{
    if (!req)
        return "request is required";
    if (!zslp_service_validate_token_key(req->token_id))
        return "token_id must be alphanumeric or 64-char hex";
    if (!zslp_service_validate_recipient_addr(req->recipient_addr,
                                              req->strict_chain_addr))
        return "address is invalid";
    if (req->amount == 0)
        return "amount must be a positive integer";
    if (req->amount > (uint64_t)INT64_MAX)
        return "amount exceeds supported maximum";
    return NULL;
}

bool zslp_service_open_db(const char *datadir, sqlite3 **db_out, bool *owns_db)
{
    struct node_db *ndb = app_runtime_node_db();

    if (!db_out || !owns_db) return false;
    *db_out = NULL;
    *owns_db = false;

    if (ndb && ndb->open && ndb->db) {
        *db_out = ndb->db;
        return true;
    }
    if (!datadir)
        return false;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    if (sqlite3_open(db_path, db_out) != SQLITE_OK) {
        if (*db_out) {
            sqlite3_close(*db_out);
            *db_out = NULL;
        }
        return false;
    }
    sqlite3_busy_timeout(*db_out, 5000);
    sqlite3_exec(*db_out,
        "CREATE TABLE IF NOT EXISTS zslp_balances ("
        "token_id TEXT NOT NULL,"
        "address TEXT NOT NULL,"
        "balance INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY (token_id, address))",
        NULL, NULL, NULL);
    *owns_db = true;
    return true;
}

void zslp_service_close_db(sqlite3 *db, bool owns_db)
{
    if (owns_db && db)
        sqlite3_close(db);
}

uint64_t zslp_service_get_balance(sqlite3 *db, const char *token_id,
                                  const char *addr)
{
    struct node_db ndb;
    struct db_zslp_balance balance;
    char token_key[ZSLP_MAX_TOKEN_KEY_LEN + 1];

    if (!db || !zslp_service_validate_token_key(token_id) ||
        !zslp_service_validate_recipient_addr(addr, false))
        return 0;

    zslp_service_canonicalize_token_key(token_id, token_key);
    zslp_service_wrap_sqlite(db, &ndb);
    if (!db_zslp_balance_find(&ndb, token_key, addr, &balance))
        return 0;
    return balance.balance < 0 ? 0 : (uint64_t)balance.balance;
}

bool zslp_service_get_token(sqlite3 *db, const char *token_id,
                            struct db_zslp_token_info *out)
{
    struct node_db ndb;
    char token_key[ZSLP_MAX_TOKEN_KEY_LEN + 1];

    if (!db || !out || !zslp_service_validate_token_key(token_id))
        return false;

    zslp_service_canonicalize_token_key(token_id, token_key);
    zslp_service_wrap_sqlite(db, &ndb);
    return db_zslp_token_find(&ndb, token_key, out);
}

int zslp_service_list_tokens(sqlite3 *db, struct db_zslp_token_info *out,
                             size_t max_out)
{
    struct node_db ndb;

    if (!db || !out || max_out == 0)
        return 0;

    zslp_service_wrap_sqlite(db, &ndb);
    return db_zslp_token_list(&ndb, out, max_out);
}

int zslp_service_list_transfers(sqlite3 *db, const char *token_id,
                                struct db_zslp_transfer_info *out,
                                size_t max_out)
{
    struct node_db ndb;
    char token_key[ZSLP_MAX_TOKEN_KEY_LEN + 1];

    if (!db || !out || max_out == 0 ||
        !zslp_service_validate_token_key(token_id))
        return 0;

    zslp_service_canonicalize_token_key(token_id, token_key);
    zslp_service_wrap_sqlite(db, &ndb);
    return db_zslp_transfer_list_by_token(&ndb, token_key, out, max_out);
}

bool zslp_service_credit_balance(sqlite3 *db, const char *token_id,
                                 const char *recipient_addr, uint64_t amount)
{
    struct node_db ndb;
    char token_key[ZSLP_MAX_TOKEN_KEY_LEN + 1];

    if (!db || amount == 0 || !zslp_service_validate_token_key(token_id) ||
        !zslp_service_validate_recipient_addr(recipient_addr, false))
        return false;
    if (amount > (uint64_t)INT64_MAX)
        return false;
    zslp_service_canonicalize_token_key(token_id, token_key);
    zslp_service_wrap_sqlite(db, &ndb);
    return db_zslp_balance_credit(&ndb, token_key, recipient_addr, (int64_t)amount);
}

bool zslp_service_store_token(sqlite3 *db, const char *token_id,
                              const char *ticker, const char *name,
                              int decimals, int64_t initial_supply)
{
    struct node_db ndb;
    char token_key[ZSLP_MAX_TOKEN_KEY_LEN + 1];

    if (!db || !token_id || !ticker || !name)
        return false;

    zslp_service_canonicalize_token_key(token_id, token_key);
    zslp_service_wrap_sqlite(db, &ndb);
    return db_zslp_token_save_key(&ndb, token_key, ticker, name, decimals,
                                  "", 0, initial_supply);
}
