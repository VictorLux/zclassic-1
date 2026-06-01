/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite-backed wallet storage. Replaces wallet_db (LevelDB) for runtime.
 * Uses node.db's wallet_keys, wallet_sapling_keys, wallet_scripts,
 * wallet_seed, wallet_watch_only, and wallet_transactions tables.
 *
 * Two parallel APIs:
 *   - New rich-error primary:  *_r functions return `struct zcl_result`.
 *     Use these in new code.
 *   - Legacy bool wrappers:    the original `bool`-returning names are
 *     kept as thin wrappers that call the *_r implementation and
 *     LOG_FAIL on non-ok. Marked ZCL_DEPRECATED; migrate callers
 *     incrementally.
 *
 * Current deviation from the canonical §5.2 signatures (bare names
 * returning zcl_result) is transitional — controller work will migrate
 * callers, after which the bool wrappers can be dropped and the *_r
 * suffix removed. */

#ifndef ZCL_WALLET_SQLITE_H
#define ZCL_WALLET_SQLITE_H

#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "script/script.h"
#include "core/uint256.h"
#include "util/result.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

/* Tag transitional bool wrappers so the compiler nudges callers to
 * the rich-error variant. */
#ifndef ZCL_DEPRECATED
#  if defined(__GNUC__) || defined(__clang__)
#    define ZCL_DEPRECATED(msg) __attribute__((deprecated(msg)))
#  else
#    define ZCL_DEPRECATED(msg)
#  endif
#endif

/* Error codes for the wallet_sqlite layer.  Negative to avoid colliding
 * with SQLITE_* which use small positive ints.  Keep in sync with
 * plan §5.2. */
enum wallet_sqlite_err {
    WSQL_OK                      = 0,
    WSQL_NULL_ARG                = -100,
    WSQL_DB_NOT_OPEN             = -101,
    WSQL_ALREADY_OPEN            = -102,
    WSQL_PREPARE_FAIL            = -103,
    WSQL_SCHEMA_MISSING          = -104,
    WSQL_CANARY_WRITE_FAIL       = -110,
    WSQL_CANARY_READ_MISMATCH    = -111,
    WSQL_WRITE_FAIL              = -120,
    WSQL_READ_FAIL               = -121,
    WSQL_TXN_BEGIN_FAIL          = -130,
    WSQL_TXN_COMMIT_FAIL         = -131,
    WSQL_INVARIANT_PUBKEY        = -140,
    WSQL_INVARIANT_PRIVKEY       = -141,
    WSQL_INVARIANT_HASH_MISMATCH = -142,
};

/* Aggregate health snapshot.  Non-destructive; safe from any thread
 * that has already published a wallet_sqlite struct. */
struct wallet_sqlite_health {
    bool    open;                    /* subsystem open */
    bool    canary_ok;               /* last self-test passed */
    int64_t canary_last_ok_ts;       /* unix time of last success */
    int     row_count;               /* SELECT count(*) FROM wallet_keys */
    int     keystore_count;          /* caller-supplied in-memory count */
    bool    mismatch;                /* row_count != keystore_count */
    char    last_error[ZCL_RESULT_MSG_MAX];  /* most recent failed call */
};

struct wallet_sqlite {
    sqlite3 *db;  /* borrowed handle to node.db */
    bool open;

    /* Prepared statements */
    sqlite3_stmt *stmt_key_write;
    sqlite3_stmt *stmt_key_read;
    sqlite3_stmt *stmt_key_read_one;   /* SELECT ... WHERE pubkey_hash = ? */
    sqlite3_stmt *stmt_key_delete;     /* DELETE ... WHERE pubkey_hash = ? */
    sqlite3_stmt *stmt_tx_write;
    sqlite3_stmt *stmt_tx_read;
    sqlite3_stmt *stmt_seed_write;
    sqlite3_stmt *stmt_seed_read;
    sqlite3_stmt *stmt_zkey_write;
    sqlite3_stmt *stmt_zkey_read;
    sqlite3_stmt *stmt_script_write;
    sqlite3_stmt *stmt_script_read;
    sqlite3_stmt *stmt_watch_write;
    sqlite3_stmt *stmt_watch_read;

    /* Best-block and scan-height pointers live in node_state under
     * fixed keys.  Cached so hot paths (every post-block flush,
     * every rescan step) don't pay prepare+finalize per call. */
    sqlite3_stmt *stmt_best_block_write;
    sqlite3_stmt *stmt_best_block_read;
    sqlite3_stmt *stmt_scan_height_write;
    sqlite3_stmt *stmt_scan_height_read;

    /* Health bookkeeping. Updated by self-test and by every failed
     * public call.  Read by wallet_sqlite_get_health(). */
    bool    canary_ok;
    int64_t canary_last_ok_ts;
    char    last_error[ZCL_RESULT_MSG_MAX];
};

/* ── Rich-error API (preferred) ─────────────────────────────────── */

/* Rows dropped by wallet_sqlite_read_keys_r since process start.
 * Surfaces in getwalletinfo.persistence.corrupt_rows so operators
 * see decode/decrypt drift instead of learning about it when a
 * spend fails.  Reset only by process restart. */
int wallet_sqlite_read_keys_corrupt_count(void);

struct zcl_result wallet_sqlite_open_r(struct wallet_sqlite *ws, sqlite3 *db);
struct zcl_result wallet_sqlite_self_test(struct wallet_sqlite *ws);
struct zcl_result wallet_sqlite_read_keys_r(struct wallet_sqlite *ws,
                                            struct wallet *w);
struct zcl_result wallet_sqlite_read_single_key(struct wallet_sqlite *ws,
                                                const struct pubkey *pk,
                                                struct privkey *out_key);
struct zcl_result wallet_sqlite_write_key_r(struct wallet_sqlite *ws,
                                            const struct pubkey *pk,
                                            const struct privkey *key);
struct zcl_result wallet_sqlite_delete_key_r(struct wallet_sqlite *ws,
                                             const struct pubkey *pk);
struct zcl_result wallet_sqlite_flush_r(struct wallet_sqlite *ws,
                                        struct wallet *w);
struct wallet_sqlite_health
    wallet_sqlite_get_health(struct wallet_sqlite *ws, int keystore_count);

/* ── Legacy bool API (deprecated wrappers) ──────────────────────── */

ZCL_DEPRECATED("use wallet_sqlite_open_r for richer errors")
bool wallet_sqlite_open(struct wallet_sqlite *ws, sqlite3 *db);

void wallet_sqlite_close(struct wallet_sqlite *ws);

ZCL_DEPRECATED("use wallet_sqlite_write_key_r")
bool wallet_sqlite_write_key(struct wallet_sqlite *ws, const struct pubkey *pk,
                              const struct privkey *key);
ZCL_DEPRECATED("use wallet_sqlite_read_keys_r")
bool wallet_sqlite_read_keys(struct wallet_sqlite *ws, struct wallet *w);

bool wallet_sqlite_write_tx(struct wallet_sqlite *ws,
                              const struct wallet_tx *wtx);
bool wallet_sqlite_read_txs(struct wallet_sqlite *ws, struct wallet *w);

bool wallet_sqlite_write_best_block(struct wallet_sqlite *ws,
                                      const struct uint256 *hash);
bool wallet_sqlite_read_best_block(struct wallet_sqlite *ws,
                                     struct uint256 *hash);

bool wallet_sqlite_write_scan_height(struct wallet_sqlite *ws, int height);
bool wallet_sqlite_read_scan_height(struct wallet_sqlite *ws, int *height);

bool wallet_sqlite_write_sapling_seed(struct wallet_sqlite *ws,
                                        const uint8_t seed[32]);
bool wallet_sqlite_read_sapling_seed(struct wallet_sqlite *ws,
                                       uint8_t seed[32]);
bool wallet_sqlite_write_sapling_key(struct wallet_sqlite *ws,
                                       uint32_t child_index,
                                       const struct sapling_key_entry *entry);
bool wallet_sqlite_read_sapling_keys(struct wallet_sqlite *ws,
                                       struct wallet *w);

bool wallet_sqlite_write_script(struct wallet_sqlite *ws,
                                  const struct uint160 *script_id,
                                  const struct script *redeem_script);
bool wallet_sqlite_read_scripts(struct wallet_sqlite *ws, struct wallet *w);

bool wallet_sqlite_write_watch_only(struct wallet_sqlite *ws,
                                      const uint8_t address_hash[20],
                                      const char *address);
bool wallet_sqlite_read_watch_only(struct wallet_sqlite *ws, struct wallet *w);

ZCL_DEPRECATED("use wallet_sqlite_flush_r")
bool wallet_sqlite_flush(struct wallet_sqlite *ws, struct wallet *w);

#endif
