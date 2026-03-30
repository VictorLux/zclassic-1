/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Internal header shared between boot.c, boot_index.c, boot_services.c.
 * Not for use outside config/src/. */

#ifndef ZCL_BOOT_INTERNAL_H
#define ZCL_BOOT_INTERNAL_H

#include "config/boot.h"
#include "validation/main_state.h"
#include "storage/coins_view_sqlite.h"
#include "coins/coins_view.h"
#include "validation/txmempool.h"
#include "net/connman.h"
#include "net/msgprocessor.h"
#include "rpc/server.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"
#include "mining/gen.h"
#include "metrics/metrics.h"
#include "storage/block_index_db.h"
#include "models/database.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <sqlite3.h>

/* All boot globals remain static in boot.c to avoid LTO conflicts
 * with pointer aliases in wallet_helpers.c. Functions in boot_index.c
 * and boot_services.c receive what they need via parameters. */

/* ── boot_index.c ───────────────────────────────────────────── */

void save_block_index_flat(const char *datadir, struct main_state *ms);
bool load_block_index_flat(const char *datadir, struct main_state *ms);
void save_block_index_recent(struct node_db *ndb, struct main_state *ms);
bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms);
bool load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open);
bool fast_rebuild_chainstate(struct coins_view_sqlite *cvs,
                              struct coins_view_cache *cvtip,
                              const char *datadir);
bool reindex_chainstate(struct main_state *ms,
                          struct coins_view_sqlite *cvs,
                          struct coins_view_cache *cvtip,
                          struct node_db *ndb,
                          const char *datadir);
void *backfill_addresses_thread(void *arg);

/* Scan block files (blk*.dat), parse ZClassic block headers,
 * create block_index entries for unknown blocks, mark BLOCK_HAVE_DATA,
 * set nTx, and propagate nChainTx. If params is NULL, only marks
 * blocks already in the index (no creation). */
int scan_block_files_mark_data(struct main_state *ms, const char *datadir,
                                const struct chain_params *params);

/* ── boot_services.c ────────────────────────────────────────── */

struct boot_svc_ctx {
    struct main_state *state;
    struct coins_view_sqlite *coins_sqlite;
    struct coins_view_cache *coins_tip;
    struct tx_mempool *mempool;
    struct rpc_table *rpc_table;
    struct msg_processor *msg_processor;
    struct connman *connman;
    struct wallet *wallet;
    struct gen_context *gen;
    struct wallet_sqlite *wallet_sqlite;
    struct node_db *node_db;
    struct metrics_context *metrics;
    _Atomic bool *running;
    const char *datadir;
    pthread_t params_thread;
    _Atomic bool *params_loaded;
    bool block_tree_open;
    struct block_tree_db *block_tree;
};

bool app_init_services(struct app_context *ctx,
                        const struct chain_params *params,
                        struct boot_svc_ctx *svc);

void app_shutdown_svc(struct boot_svc_ctx *svc);

#endif
