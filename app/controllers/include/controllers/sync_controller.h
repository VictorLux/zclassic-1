/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Bridge between the validation pipeline and SQLite.
 *
 * Called from connect_block/disconnect_block to keep
 * SQLite in sync with the UTXO set and block index.
 * Called from wallet_sync_transaction to track wallet UTXOs.
 *
 * This is the "controller" layer — it orchestrates model
 * writes in response to consensus events. */

#ifndef ZCL_DB_NODE_DB_SYNC_H
#define ZCL_DB_NODE_DB_SYNC_H

#include "models/database.h"
#include "models/block.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "models/wallet_tx.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include <stdbool.h>
#include <stdint.h>

struct block;
struct block_index;
struct transaction;
struct wallet;

/* Initialize the sync layer. Opens SQLite at datadir/node.db. */
bool node_db_sync_init(struct node_db *ndb, const char *datadir);

/* Global flag: set to true while rescanwitnesses is running.
 * Prevents connect_block from overwriting the Sapling tree. */
extern _Atomic bool g_sapling_rescan_active;

/* Called after a block is successfully connected to the active chain.
 * Indexes the block header, all transactions, and updates the UTXO set.
 * Runs inside a SQLite transaction for atomicity. */
bool node_db_sync_connect_block(struct node_db *ndb,
                                const struct block *blk,
                                const struct block_index *pindex);

/* Called when a block is disconnected during a reorg.
 * Removes the block's transaction index entries and
 * restores the UTXO set to pre-block state. */
bool node_db_sync_disconnect_block(struct node_db *ndb,
                                   const struct block *blk,
                                   const struct block_index *pindex);

/* Called when a transaction is added to the wallet.
 * Tracks wallet-owned UTXOs and marks spent inputs. */
bool node_db_sync_wallet_tx(struct node_db *ndb,
                            const struct transaction *tx,
                            const struct wallet *w,
                            int block_height);

/* Called when a transaction enters the mempool. */
bool node_db_sync_mempool_add(struct node_db *ndb,
                              const struct transaction *tx,
                              int64_t fee, int height);

/* Called when a transaction is removed from the mempool
 * (confirmed in a block or evicted). */
bool node_db_sync_mempool_remove(struct node_db *ndb,
                                 const uint8_t txid[32]);

/* Called when a Sapling note is decrypted (trial decryption
 * found a note belonging to our wallet). */
bool node_db_sync_sapling_note(struct node_db *ndb,
                               const uint8_t txid[32],
                               uint32_t output_index,
                               int64_t value,
                               const uint8_t rcm[32],
                               const uint8_t memo[512],
                               size_t memo_len,
                               const uint8_t ivk[32],
                               const uint8_t diversifier[11],
                               const uint8_t pk_d[32],
                               const uint8_t cm[32],
                               const uint8_t nullifier[32],
                               int block_height);

/* Mark Sapling nullifiers as spent (from a confirmed tx). */
bool node_db_sync_sapling_spend(struct node_db *ndb,
                                const uint8_t nullifier[32],
                                const uint8_t spending_txid[32]);

/* Persist a peer address we learned about. */
bool node_db_sync_peer(struct node_db *ndb,
                       const uint8_t ip[16], uint16_t port,
                       uint64_t services, int64_t last_seen);

/* Load persisted state on startup:
 * Returns the chain tip height stored in SQLite, or -1. */
int node_db_sync_get_tip_height(struct node_db *ndb);
bool node_db_sync_get_tip_hash(struct node_db *ndb, uint8_t hash_out[32]);

/* Store the current chain tip. */
bool node_db_sync_set_tip(struct node_db *ndb,
                          const uint8_t hash[32], int height);

/* Catch up SQLite from existing chain data on disk.
 * Reads blocks from (sqlite_tip+1) to chain_tip and indexes them.
 * Also scans for wallet transactions if wallet is provided.
 * Called once at startup after chain is loaded. */
struct active_chain;
int node_db_sync_catchup(struct node_db *ndb,
                         const struct active_chain *chain,
                         const struct wallet *w,
                         const char *datadir);

/* Copy wallet keys (transparent + Sapling) to SQLite.
 * Idempotent — skips keys that already exist. */
int node_db_sync_wallet_keys(struct node_db *ndb,
                             const struct wallet *w);

/* Background thread entry point for catchup.
 * Arg is pointer to struct with ndb, chain, w, datadir fields. */
void *node_db_sync_catchup_thread(void *arg);

/* Import the full UTXO set from chainstate LevelDB into SQLite.
 * Iterates all 'c'-prefixed entries, decodes compressed outputs,
 * and bulk-inserts into the utxos table with address indexing.
 * Returns the number of UTXO outputs imported, or -1 on error. */
struct coins_view_db;
int node_db_sync_import_utxos(struct node_db *ndb,
                               struct coins_view_db *cvdb);

/* Save current in-memory mempool to SQLite. Called on shutdown. */
struct tx_mempool;
int node_db_sync_mempool_save(struct node_db *ndb,
                              const struct tx_mempool *mempool);

/* Load persisted mempool from SQLite into in-memory pool.
 * Called on startup. Returns count loaded. */
int node_db_sync_mempool_load(struct node_db *ndb,
                              struct tx_mempool *mempool);

#endif
