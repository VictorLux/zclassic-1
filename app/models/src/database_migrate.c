/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * node.db key-value state store + schema migration runner.
 *
 * The migration runner is the primary consumer of the node_state KV store: it
 * persists schema_version after each versioned block, so these connection-handle
 * concerns ship together.
 *
 * ar-validate-skip:connection-handle-not-a-row
 *   These functions operate on the struct node_db connection handle and
 *   the node_state KV store / schema_migrations bookkeeping — none of
 *   which are row records. Row-level validation lives on the models that
 *   use this handle (same rationale as database.c). */

#include "util/log_macros.h"
#include "models/database.h"
#include "models/database_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool node_db_state_set(struct node_db *ndb, const char *key,
                       const void *value, size_t len)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_state_set;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, value, (int)len, SQLITE_STATIC);
    int rc = sqlite3_step(s);  // raw-sql-ok:kv-state-primitive
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    node_db_note_activity(ndb, "state_set", rc);
    return rc == SQLITE_DONE;
}

bool node_db_state_get(struct node_db *ndb, const char *key,
                       void *value, size_t max_len, size_t *out_len)
{
    if (!ndb->open) return false;

    /* Prepare a fresh statement per call rather than reusing the
     * cached ndb->stmt_state_get. SQLite statements are not
     * thread-safe — concurrent calls from chain_advance's worker
     * thread + the wallet/Sapling witness thread + the boot-state
     * logger thread race on the shared statement object, corrupting
     * internal buffers and triggering SIGABRT (FATAL SIGNAL 6 in libc
     * memcpy → stack-canary check).
     *
     * Per-call prepare adds a sub-millisecond cost per lookup; that's
     * fine because state_get is not a hot path (called once per
     * background task, not per block). The cached stmt_state_get
     * field is kept for source compatibility but no longer used. */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT value FROM node_state WHERE key=?",
            -1, &s, NULL) != SQLITE_OK || !s) {
        node_db_note_activity(ndb, "state_get",
                              s ? SQLITE_OK : SQLITE_ERROR);
        return false;
    }
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);  // raw-sql-ok:kv-state-primitive
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(s);
        node_db_note_activity(ndb, "state_get", rc);
        return false;
    }
    int blob_len = sqlite3_column_bytes(s, 0);
    if (blob_len <= 0) {
        sqlite3_finalize(s);
        node_db_note_activity(ndb, "state_get", SQLITE_DONE);
        return false;
    }
    size_t copy = (size_t)blob_len < max_len
                  ? (size_t)blob_len : max_len;
    memcpy(value, sqlite3_column_blob(s, 0), copy);
    if (out_len) *out_len = copy;
    sqlite3_finalize(s);
    node_db_note_activity(ndb, "state_get", rc);
    return true;
}

bool node_db_state_set_int(struct node_db *ndb,
                           const char *key, int64_t val)
{
    return node_db_state_set(ndb, key, &val, sizeof(val));
}

bool node_db_state_get_int(struct node_db *ndb,
                           const char *key, int64_t *val)
{
    size_t len = 0;
    if (!node_db_state_get(ndb, key, val, sizeof(*val), &len))
        return false;
    return len == sizeof(*val);
}

int node_db_schema_version(struct node_db *ndb)
{
    int32_t ver = 0;
    size_t len = 0;
    if (!node_db_state_get(ndb, "schema_version",
                           &ver, sizeof(ver), &len))
        return 0;
    return ver;
}

/* Persist the schema_version counter; halt migration on failure so
 * we don't silently re-apply the same migration on next boot. */
#define DB_MIGRATE_PERSIST_VERSION(ndb, ver) do { \
    int32_t _v = (int32_t)(ver); \
    if (!node_db_state_set((ndb), "schema_version", &_v, sizeof(_v))) { \
        LOG_ERR("db", "migrate: failed to persist " \
                "schema_version=%d; aborting migration to prevent " \
                "loop on next boot", (int)_v); \
    } \
} while (0)

int node_db_migrate(struct node_db *ndb, const char *datadir)
{
    (void)datadir;
    if (!ndb->open) return -1;

    /* Ensure schema_migrations table exists.  If this fails,
     * node_db_schema_version() will return 0 and every migration will
     * re-apply next boot — silent data corruption risk.  Halt now. */
    if (!node_db_exec(ndb,
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version TEXT PRIMARY KEY,"
        "  applied_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))"
        ");"
        "INSERT OR IGNORE INTO schema_migrations(version) VALUES('001');")) {
        LOG_ERR("db", "migrate: schema_migrations bootstrap failed; "
                "aborting to avoid re-applying migrations on next boot");
    }

    int applied = 0;
    int current_ver = node_db_schema_version(ndb);
    printf("db: current schema version %d\n", current_ver);

    /* Campaign C3: schema-downgrade detection.
     *
     * If the on-disk schema_version exceeds what this binary knows
     * about, refuse to proceed. The newer binary may have added
     * columns or constraints we don't understand; opening as-is and
     * writing through this binary's persistence layer would silently
     * corrupt the data the newer binary expected to see.
     *
     * The operator must either run a binary that supports schema vN+,
     * or restore from a backup taken before the upgrade. There is no
     * automatic downgrade path. */
    if (current_ver > NODE_DB_MAX_SCHEMA) {
        LOG_WARN("model", "\nFATAL: node.db schema_version=%d but this binary only knows up to %d.\n" "       Refusing to open a database written by a newer binary —\n" "       writes through this layer would silently corrupt the data.\n" "       Either run a binary that supports schema v%d+,\n" "       or restore node.db from a backup taken before the upgrade.\n", current_ver, NODE_DB_MAX_SCHEMA, current_ver);
        fflush(stderr);
        return -2;
    }

    /* Future migrations go here as versioned blocks.
     * Each block checks schema_migrations before running.
     *
     * Pattern:
     *   if (current_ver < N) {
     *       node_db_exec(ndb, "ALTER TABLE ... ; ...");
     *       node_db_exec(ndb,
     *           "INSERT OR IGNORE INTO schema_migrations(version) "
     *           "VALUES('00N')");
     *       current_ver = N;
     *       applied++;
     *   }
     * Don't forget to bump NODE_DB_MAX_SCHEMA in
     * app/models/include/models/database.h when you add a new block.
     */

    if (current_ver < 2) {
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('002')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 2);
        current_ver = 2;
        applied++;
    }

    if (current_ver < 3) {
        /* Add Sapling commitment tree tracking columns */
        node_db_exec(ndb,
            "ALTER TABLE blocks ADD COLUMN sapling_tree_data BLOB");
        node_db_exec(ndb,
            "ALTER TABLE wallet_sapling_notes ADD COLUMN witness_data BLOB");
        node_db_exec(ndb,
            "ALTER TABLE wallet_sapling_notes ADD COLUMN witness_height INTEGER DEFAULT 0");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('003')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 3);
        current_ver = 3;
        applied++;
    }

    if (current_ver < 4) {
        /* Block index cache — enables instant warm restart by skipping
         * the 11s LevelDB block index load. Verified cryptographically
         * via tip hash match with coins DB. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS block_index_cache ("
            "hash BLOB NOT NULL PRIMARY KEY,"
            "prev_hash BLOB NOT NULL,"
            "height INTEGER NOT NULL,"
            "n_bits INTEGER NOT NULL,"
            "n_time INTEGER NOT NULL,"
            "n_version INTEGER NOT NULL DEFAULT 4,"
            "n_status INTEGER NOT NULL DEFAULT 0,"
            "n_file INTEGER NOT NULL DEFAULT 0,"
            "n_data_pos INTEGER NOT NULL DEFAULT 0,"
            "n_undo_pos INTEGER NOT NULL DEFAULT 0,"
            "n_tx INTEGER NOT NULL DEFAULT 0,"
            "chain_work BLOB,"
            "merkle_root BLOB,"
            "final_sapling_root BLOB,"
            "nonce BLOB,"
            "solution BLOB,"
            "n_solution_size INTEGER NOT NULL DEFAULT 0,"
            "n_cached_branch_id INTEGER NOT NULL DEFAULT 0"
            ")");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_bic_height"
            " ON block_index_cache(height)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('004')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 4);
        current_ver = 4;
        applied++;
    }

    if (current_ver < 5) {
        /* v5: Explorer + REST API tables and indexes.
         * Optimized for high-performance read queries. */

        /* Addresses table — aggregated balance cache per address.
         * Rebuilt from UTXO set on demand. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS addresses ("
            "address_hash BLOB PRIMARY KEY,"
            "script_type INTEGER NOT NULL DEFAULT 0,"
            "balance INTEGER NOT NULL DEFAULT 0,"
            "utxo_count INTEGER NOT NULL DEFAULT 0,"
            "first_seen_height INTEGER NOT NULL DEFAULT 0,"
            "last_seen_height INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_addr_balance"
            " ON addresses(balance DESC)");

        /* Chain stats — pre-computed per-block aggregate stats.
         * Used for charts (difficulty, hashrate, supply). */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS chain_stats ("
            "height INTEGER PRIMARY KEY,"
            "time INTEGER NOT NULL,"
            "difficulty REAL NOT NULL DEFAULT 0,"
            "tx_count INTEGER NOT NULL DEFAULT 0,"
            "utxo_count INTEGER NOT NULL DEFAULT 0,"
            "total_supply INTEGER NOT NULL DEFAULT 0,"
            "shielded_supply INTEGER NOT NULL DEFAULT 0,"
            "block_size INTEGER NOT NULL DEFAULT 0)");

        /* ZSLP token registry — discovered from OP_RETURN GENESIS txs */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_tokens ("
            "token_id BLOB PRIMARY KEY,"
            "ticker TEXT NOT NULL DEFAULT '',"
            "name TEXT NOT NULL DEFAULT '',"
            "decimals INTEGER NOT NULL DEFAULT 0,"
            "document_url TEXT DEFAULT '',"
            "genesis_height INTEGER NOT NULL DEFAULT 0,"
            "total_minted INTEGER NOT NULL DEFAULT 0,"
            "total_burned INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_ticker"
            " ON zslp_tokens(ticker)");

        /* Additional covering index for block queries without status filter */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_height_all"
            " ON blocks(height)");

        /* Index for timestamp lookups (HODL wave chart) */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_time"
            " ON blocks(time)");

        /* Composite covering index for UTXO age distribution (HODL waves) */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_utxo_height_value"
            " ON utxos(height, value)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('005')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 5);
        current_ver = 5;
        applied++;
    }

    if (current_ver < 6) {
        /* v6: Add address column to wallet_sapling_notes for per-order
         * payment matching.  Tolerate "duplicate column name" on
         * re-apply; any other error (disk full, corruption) is now
         * logged instead of silently swallowed. */
        db_exec_tolerant(ndb->db,
            "ALTER TABLE wallet_sapling_notes ADD COLUMN address TEXT",
            "v6: add wallet_sapling_notes.address",
            "duplicate column name");
        db_exec_checked(ndb->db,
            "CREATE INDEX IF NOT EXISTS idx_snote_address"
            " ON wallet_sapling_notes(address) WHERE spent_txid IS NULL",
            "v6: idx_snote_address");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('006')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 6);
        current_ver = 6;
        applied++;
    }

    if (current_ver < 7) {
        /* v7: ZSLP token transfer tracking + OP_RETURN index */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_transfers ("
            "txid BLOB NOT NULL,"
            "block_height INTEGER NOT NULL,"
            "token_id BLOB NOT NULL,"
            "tx_type INTEGER NOT NULL," /* 1=GENESIS, 2=MINT, 3=SEND */
            "from_addr BLOB,"
            "to_addr BLOB,"
            "amount INTEGER NOT NULL DEFAULT 0,"
            "vout INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY (txid, vout))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_token"
            " ON zslp_transfers(token_id)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_height"
            " ON zslp_transfers(block_height DESC)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_addr"
            " ON zslp_transfers(to_addr) WHERE to_addr IS NOT NULL");

        /* OP_RETURN index — stores all OP_RETURN output data for scanning */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS op_returns ("
            "txid BLOB PRIMARY KEY,"
            "block_height INTEGER NOT NULL,"
            "script BLOB NOT NULL,"
            "is_slp INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_opret_height"
            " ON op_returns(block_height)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_opret_slp"
            " ON op_returns(is_slp) WHERE is_slp = 1");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('007')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 7);
        current_ver = 7;
        applied++;
    }

    if (current_ver < 8) {
        /* v8: Partial indexes on shielded value columns for fast stats queries.
         * Only index non-zero rows — most blocks have zero shielded activity. */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_sprout_value "
            "ON blocks(sprout_value) WHERE sprout_value != 0");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_sapling_value "
            "ON blocks(sapling_value) WHERE sapling_value != 0");

        /* Covering index for time-range queries on shielded blocks */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_time_sprout "
            "ON blocks(time, sprout_value) WHERE sprout_value != 0");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_time_sapling "
            "ON blocks(time, sapling_value) WHERE sapling_value != 0");

        /* Index for num_tx queries (block records) */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_blocks_num_tx "
            "ON blocks(num_tx DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('008')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 8);
        current_ver = 8;
        applied++;
    }

    if (current_ver < 9) {
        /* v9: Full chain indexing — permanent tx inputs/outputs,
         * Sapling spends/outputs, Sprout JoinSplits, SHA3 integrity. */

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS tx_outputs ("
            "txid BLOB NOT NULL, vout INTEGER NOT NULL,"
            "value INTEGER NOT NULL, script_type INTEGER NOT NULL DEFAULT 0,"
            "address_hash BLOB, block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, vout))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txo_addr"
            " ON tx_outputs(address_hash) WHERE address_hash IS NOT NULL");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txo_height"
            " ON tx_outputs(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS tx_inputs ("
            "txid BLOB NOT NULL, vin_index INTEGER NOT NULL,"
            "prev_txid BLOB NOT NULL, prev_vout INTEGER NOT NULL,"
            "block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, vin_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txi_prev"
            " ON tx_inputs(prev_txid, prev_vout)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_txi_height"
            " ON tx_inputs(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS sapling_spends ("
            "txid BLOB NOT NULL, spend_index INTEGER NOT NULL,"
            "cv BLOB NOT NULL, anchor BLOB NOT NULL,"
            "nullifier BLOB NOT NULL, rk BLOB NOT NULL,"
            "block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, spend_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_ss_nf"
            " ON sapling_spends(nullifier)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_ss_height"
            " ON sapling_spends(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS sapling_outputs ("
            "txid BLOB NOT NULL, output_index INTEGER NOT NULL,"
            "cv BLOB NOT NULL, cm BLOB NOT NULL,"
            "ephemeral_key BLOB NOT NULL, block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, output_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_so_height"
            " ON sapling_outputs(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS joinsplits ("
            "txid BLOB NOT NULL, js_index INTEGER NOT NULL,"
            "vpub_old INTEGER NOT NULL, vpub_new INTEGER NOT NULL,"
            "anchor BLOB NOT NULL, block_height INTEGER NOT NULL,"
            "PRIMARY KEY (txid, js_index))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_js_height"
            " ON joinsplits(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS sprout_nullifiers ("
            "nullifier BLOB PRIMARY KEY,"
            "txid BLOB NOT NULL, block_height INTEGER NOT NULL)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_spnf_height"
            " ON sprout_nullifiers(block_height)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS view_integrity ("
            "height INTEGER PRIMARY KEY,"
            "sha3_hash BLOB NOT NULL)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('009')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 9);
        current_ver = 9;
        applied++;
    }

    if (current_ver < 10) {
        /* v10: Add n_chain_tx to block_index_cache for full restart from SQLite.
         * Needed so difficulty validation (17-ancestor walk) works without LevelDB. */
        db_exec_tolerant(ndb->db,
            "ALTER TABLE block_index_cache ADD COLUMN n_chain_tx INTEGER NOT NULL DEFAULT 0",
            "v10: add block_index_cache.n_chain_tx",
            "duplicate column name");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('010')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 10);
        current_ver = 10;
        applied++;
    }

    if (current_ver < 11) {
        /* v11: Peer bandwidth scores + ZCL23 flag for fast reconnection.
         * New nodes should reconnect to fast ZCL23 peers first, enabling
         * instant swarm sync on subsequent starts. */
        db_exec_tolerant(ndb->db,
            "ALTER TABLE peers ADD COLUMN bandwidth_score INTEGER NOT NULL DEFAULT 0",
            "v11: add peers.bandwidth_score",
            "duplicate column name");
        db_exec_tolerant(ndb->db,
            "ALTER TABLE peers ADD COLUMN is_zcl23 INTEGER NOT NULL DEFAULT 0",
            "v11: add peers.is_zcl23",
            "duplicate column name");
        /* Index: prioritize fast ZCL23 peers for reconnection */
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_peers_zcl23_score "
            "ON peers(is_zcl23 DESC, bandwidth_score DESC)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('011')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 11);
        current_ver = 11;
        applied++;
    }

    if (current_ver < 12) {
        /* v12: ZSLP address balances as a first-class model-backed table. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zslp_balances ("
            "token_id TEXT NOT NULL,"
            "address TEXT NOT NULL,"
            "balance INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY (token_id, address))");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_balance_token "
            "ON zslp_balances(token_id)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zslp_balance_address "
            "ON zslp_balances(address)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('012')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 12);
        current_ver = 12;
        applied++;
    }

    if (current_ver < 13) {
        /* v13: App-facing lightweight models for wallet contacts and
         * onion announcement registry. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS contacts ("
            "address TEXT PRIMARY KEY,"
            "name TEXT NOT NULL,"
            "last_used INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_contacts_last_used "
            "ON contacts(last_used DESC)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS onion_announcements ("
            "onion_address TEXT PRIMARY KEY,"
            "announced_at INTEGER NOT NULL,"
            "script_hex TEXT NOT NULL DEFAULT '')");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_onion_announced_at "
            "ON onion_announcements(announced_at DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('013')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 13);
        current_ver = 13;
        applied++;
    }

    if (current_ver < 14) {
        /* v14: Store product/order tables as model-owned app schema. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS products ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL,"
            "description TEXT,"
            "price_zatoshi INTEGER NOT NULL,"
            "token_id TEXT,"
            "tokens_per_purchase INTEGER NOT NULL DEFAULT 1,"
            "active INTEGER NOT NULL DEFAULT 1)");

        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS orders ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "product_id INTEGER NOT NULL,"
            "customer_addr TEXT,"
            "payment_addr TEXT NOT NULL,"
            "amount_zatoshi INTEGER NOT NULL,"
            "payment_txid TEXT,"
            "mint_txid TEXT,"
            "status INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL,"
            "paid_at INTEGER)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_orders_status_created "
            "ON orders(status, created_at DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('014')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 14);
        current_ver = 14;
        applied++;
    }

    if (current_ver < 15) {
        /* v15: ZCL Market — file offers for crypto-incentivized sharing */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS file_offers ("
            "root_hash BLOB NOT NULL PRIMARY KEY,"
            "filename TEXT NOT NULL,"
            "size_bytes INTEGER NOT NULL,"
            "num_chunks INTEGER NOT NULL,"
            "price_per_mb INTEGER NOT NULL,"
            "z_addr BLOB,"
            "peer_ip BLOB,"
            "peer_port INTEGER,"
            "last_seen INTEGER,"
            "ttl INTEGER DEFAULT 4)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_file_offers_last_seen "
            "ON file_offers(last_seen DESC)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('015')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 15);
        current_ver = 15;
        applied++;
    }

    if (current_ver < 16) {
        /* v16: ZCL Names (ZNAM) — on-chain name registry */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS znam_names ("
            "name TEXT PRIMARY KEY,"
            "owner_address TEXT NOT NULL,"
            "target_type INTEGER NOT NULL,"
            "target_value TEXT NOT NULL,"
            "reg_txid BLOB NOT NULL,"
            "reg_height INTEGER NOT NULL,"
            "last_update_txid BLOB NOT NULL)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_znam_owner "
            "ON znam_names(owner_address)");

        /* ENS-inspired text records (TextResolver) */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS znam_text_records ("
            "name TEXT NOT NULL,"
            "key TEXT NOT NULL,"
            "value TEXT,"
            "PRIMARY KEY(name, key))");

        /* ENS-inspired multi-coin address records (AddrResolver) */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS znam_addr_records ("
            "name TEXT NOT NULL,"
            "coin_type INTEGER NOT NULL,"
            "address TEXT NOT NULL,"
            "PRIMARY KEY(name, coin_type))");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('016')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 16);
        current_ver = 16;
        applied++;
    }

    if (current_ver < 17) {
        /* v17: ZCL Messaging (ZMSG) — encrypted P2P messages */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zmsg_messages ("
            "msg_id BLOB PRIMARY KEY,"
            "direction INTEGER NOT NULL,"
            "channel INTEGER NOT NULL,"
            "sender TEXT NOT NULL,"
            "recipient TEXT NOT NULL,"
            "body TEXT NOT NULL,"
            "timestamp INTEGER NOT NULL,"
            "txid BLOB,"
            "read INTEGER NOT NULL DEFAULT 0)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zmsg_time "
            "ON zmsg_messages(timestamp DESC)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zmsg_unread "
            "ON zmsg_messages(read) WHERE read=0");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('017')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 17);
        current_ver = 17;
        applied++;
    }

    if (current_ver < 18) {
        /* v18: Atomic swaps (ZSWP) — HTLC contracts for BTC/LTC/DOGE */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS zswp_contracts ("
            "swap_id TEXT PRIMARY KEY,"
            "role INTEGER NOT NULL,"
            "state INTEGER NOT NULL,"
            "chain INTEGER NOT NULL DEFAULT 0,"
            "secret_hash BLOB NOT NULL,"
            "secret BLOB,"
            "amount INTEGER NOT NULL,"
            "locktime INTEGER NOT NULL,"
            "my_address TEXT NOT NULL,"
            "counter_address TEXT NOT NULL,"
            "funding_txid BLOB,"
            "funding_vout INTEGER,"
            "redeem_script BLOB NOT NULL,"
            "redeem_script_len INTEGER NOT NULL,"
            "p2sh_address TEXT NOT NULL,"
            "created_at INTEGER NOT NULL)");

        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_zswp_state "
            "ON zswp_contracts(state)");

        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('018')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 18);
        current_ver = 18;
        applied++;
    }

    if (current_ver < 19) {
        /* v19: HODL wave history — one row per sample height, populated
         * lazily by app/services/src/hodl_history_service.c. Each row
         * is the reconstructed UTXO snapshot as of that height:
         *   total_zat = sum of unspent output value at H
         *   older_1y_zat = subset that was older than 1 year at H
         *   older_1y_pct = older_1y_zat / total_zat * 100
         * Source: tx_outputs + tx_inputs, no current-UTXO-table dep. */
        node_db_exec(ndb,
            "CREATE TABLE IF NOT EXISTS hodl_history ("
            "height INTEGER PRIMARY KEY,"
            "time INTEGER NOT NULL,"
            "total_zat INTEGER NOT NULL DEFAULT 0 CHECK(total_zat >= 0),"
            "older_1y_zat INTEGER NOT NULL DEFAULT 0 "
            "  CHECK(older_1y_zat >= 0 AND older_1y_zat <= total_zat),"
            "older_1y_pct REAL NOT NULL DEFAULT 0)");
        node_db_exec(ndb,
            "CREATE INDEX IF NOT EXISTS idx_hodl_history_time "
            "ON hodl_history(time)");
        node_db_exec(ndb,
            "INSERT OR IGNORE INTO schema_migrations(version) VALUES('019')");
        DB_MIGRATE_PERSIST_VERSION(ndb, 19);
        current_ver = 19;
        applied++;
    }

    if (applied > 0)
        printf("db: applied %d migration(s), now at version %d\n",
               applied, node_db_schema_version(ndb));

    return applied;
}
