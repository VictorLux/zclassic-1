/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "init/init.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "coins/coins_view.h"
#include "storage/coins_db.h"
#include "consensus/validation.h"
#include "rpc/blockchain.h"
#include "rpc/misc.h"
#include "rpc/net_rpc.h"
#include "rpc/httpserver.h"
#include "rpc/mining_rpc.h"
#include "rpc/rawtransaction.h"
#include "rpc/server.h"
#include "storage/block_index_db.h"
#include "storage/coins_db.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "net/connman.h"
#include "net/msgprocessor.h"
#include "keys/key_io.h"
#include "mining/gen.h"
#include "script/standard.h"
#include "rpc/wallet_rpc.h"
#include "wallet/wallet.h"
#include "wallet/wallet_db.h"
#include "zcash/params_init.h"
#include "metrics/metrics.h"
#include "chain/pow.h"
#include "db/node_db_sync.h"
#include "db/legacy_import.h"
#include <netdb.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

static struct main_state g_state;
static struct coins_view_db g_coins_db;
static struct coins_view_cache g_coins_tip;
static struct block_tree_db g_block_tree;
struct block_tree_db *g_active_block_tree = NULL;
static bool g_block_tree_open = false;
static struct tx_mempool g_mempool;
static struct rpc_table g_rpc_table;
static struct msg_processor g_msg_processor;
static struct connman g_connman;
static struct wallet g_wallet;
struct wallet *g_active_wallet = NULL;
static struct gen_context g_gen;
static struct wallet_db g_wallet_db;
static struct node_db g_node_db;
struct node_db *g_active_node_db = NULL;
static const char *g_datadir = NULL;
static _Atomic bool g_running = false;
static struct metrics_context g_metrics;

void app_context_defaults(struct app_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->datadir = NULL;
    ctx->params_dir = NULL;
    ctx->rpc_port = 8232;
    ctx->p2p_port = 8033;
    ctx->listen = false;
    ctx->checkpoints_enabled = true;
}

static struct block_index *insert_block_index_cb(void *ctx_ptr,
                                                  const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index(
        (struct chainstate *)ms, hash);
}

static int cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1;
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

static bool load_block_index(struct main_state *ms,
                              const struct chain_params *params)
{
    if (g_block_tree_open) {
        if (!block_tree_db_load_block_index_guts(&g_block_tree,
                                                  insert_block_index_cb, ms))
            return false;
    }

    /* Fix phashBlock pointers — block_map rehashing invalidates them */
    {
        size_t iter = 0;
        struct block_index *pi;
        const struct uint256 *hash;
        while (block_map_next(&ms->map_block_index, &iter, &hash, &pi)) {
            if (pi)
                pi->phashBlock = hash;
        }
    }

    if (ms->map_block_index.size == 0) {
        struct block_index *genesis = chainstate_insert_block_index(
            (struct chainstate *)ms,
            &params->consensus.hashGenesisBlock);
        if (genesis) {
            genesis->nHeight = 0;
            genesis->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            genesis->nTx = 1;
            genesis->nChainTx = 1;
            genesis->nBits = 0x1f07ffff;
            genesis->nChainWork = GetBlockProof(genesis);
            /* Set genesis as active chain tip directly */
            active_chain_set_tip(&ms->chain_active, genesis);
            ms->pindex_best_header = genesis;
        }
        return true;
    }

    /* Post-load: compute nChainWork, nChainTx, skip links (like C++ LoadBlockIndexDB) */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = malloc(count * sizeof(struct block_index *));
    if (!sorted)
        return false;

    size_t idx = 0;
    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (pindex && idx < count)
            sorted[idx++] = pindex;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    for (size_t i = 0; i < count; i++) {
        pindex = sorted[i];

        /* Compute chain work */
        struct arith_uint256 proof = GetBlockProof(pindex);
        if (pindex->pprev)
            arith_uint256_add(&pindex->nChainWork,
                              &pindex->pprev->nChainWork, &proof);
        else
            pindex->nChainWork = proof;

        /* Compute chain tx count */
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx)
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                else
                    pindex->nChainTx = 0;
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }

        /* Build skip list */
        block_index_build_skip(pindex);

        /* Propagate cached branch ID */
        if (pindex->pprev) {
            if (block_index_is_valid(pindex, BLOCK_VALID_CONSENSUS) &&
                !pindex->nCachedBranchId)
                pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
        }

        /* Mark children of failed blocks */
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev &&
            (pindex->pprev->nStatus & BLOCK_FAILED_MASK))
            pindex->nStatus |= BLOCK_FAILED_CHILD;
    }

    free(sorted);
    return true;
}

bool app_init(struct app_context *ctx)
{
    if (ctx->regtest)
        chain_params_select(CHAIN_REGTEST);
    else if (ctx->testnet)
        chain_params_select(CHAIN_TESTNET);
    else
        chain_params_select(CHAIN_MAIN);

    const struct chain_params *params = chain_params_get();
    g_datadir = ctx->datadir;

    ecc_start();
    ecc_verify_init();

    main_state_init(&g_state);
    g_state.fTxIndex = ctx->tx_index;
    g_state.fCheckpointsEnabled = ctx->checkpoints_enabled;

    /* Load ZK verification keys */
    if (ctx->params_dir) {
        printf("Loading verification keys...\n");
        fflush(stdout);
        if (!zcash_init_params(ctx->params_dir)) {
            fprintf(stderr, "Error: Failed to load verification keys from %s\n",
                    ctx->params_dir);
            return false;
        }
        printf("Verification keys loaded.\n");
    }

    /* Initialize wallet (before block index — needed for -importlegacy) */
    wallet_init(&g_wallet);

    char wallet_path[1024];
    snprintf(wallet_path, sizeof(wallet_path), "%s/wallet", ctx->datadir);
    if (wallet_db_open(&g_wallet_db, wallet_path)) {
        wallet_db_read_keys(&g_wallet_db, &g_wallet);
        wallet_db_read_txs(&g_wallet_db, &g_wallet);
        wallet_rebuild_spent_set(&g_wallet);
        wallet_db_read_sapling_keys(&g_wallet_db, &g_wallet);
        wallet_db_read_scripts(&g_wallet_db, &g_wallet);
        int saved_height = 0;
        if (wallet_db_read_scan_height(&g_wallet_db, &saved_height))
            g_wallet.best_block_height = saved_height;
        printf("Wallet loaded: %zu keys, %zu sapling keys, %zu scripts, "
               "%zu txs, scan height %d.\n",
               g_wallet.keystore.num_keys,
               g_wallet.sapling_keys.num_keys,
               g_wallet.keystore.num_scripts,
               g_wallet.num_wallet_tx,
               g_wallet.best_block_height);
    } else {
        printf("New wallet created.\n");
    }

    if (g_wallet.keystore.num_keys == 0)
        wallet_top_up_key_pool(&g_wallet, DEFAULT_KEYPOOL_SIZE);
    printf("Wallet has %zu keys.\n", g_wallet.keystore.num_keys);
    g_active_wallet = &g_wallet;

    /* Open SQLite node database */
    if (node_db_sync_init(&g_node_db, ctx->datadir)) {
        g_active_node_db = &g_node_db;
        int db_tip = node_db_sync_get_tip_height(&g_node_db);
        if (db_tip >= 0)
            printf("SQLite tip: height=%d\n", db_tip);
    } else {
        fprintf(stderr, "Warning: SQLite database unavailable\n");
    }

    /* Fast path: -importlegacy imports wallet data from legacy block files
     * and exits. No block index, no P2P, no RPC needed. */
    if (ctx->import_legacy_dir) {
        if (!g_active_node_db) {
            fprintf(stderr, "Error: SQLite database required for import\n");
            return false;
        }
        int result = legacy_import(ctx->import_legacy_dir,
                                    g_active_node_db, &g_wallet,
                                    ctx->sapling_scan);
        if (result >= 0)
            printf("Import complete: %d wallet transactions found.\n", result);
        else
            fprintf(stderr, "Import failed.\n");
        return false; /* triggers exit in main() */
    }

    /* Open block index database */
    char blocktree_path[1024];
    snprintf(blocktree_path, sizeof(blocktree_path), "%s/blocks/index",
             ctx->datadir);
    if (block_tree_db_open(&g_block_tree, blocktree_path,
                           256 << 20, false, false)) {
        g_block_tree_open = true;
        g_active_block_tree = &g_block_tree;
    } else {
        fprintf(stderr, "Warning: Could not open block tree DB at %s\n",
                blocktree_path);
    }

    /* Open coins database */
    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", ctx->datadir);
    if (!coins_view_db_open(&g_coins_db, coins_path,
                            DEFAULT_DB_CACHE << 20, false, false)) {
        fprintf(stderr, "Warning: Could not open coins DB at %s\n", coins_path);
    }

    coins_view_cache_init(&g_coins_tip, &g_coins_db.view);

    /* Load block index from disk */
    printf("Loading block index...\n");
    if (!load_block_index(&g_state, params)) {
        fprintf(stderr, "Warning: Failed to load block index\n");
    }
    printf("Block index loaded: %zu entries\n", g_state.map_block_index.size);

    /* Restore chain tip from coins DB best block hash */
    bool skip_activate = false;
    if (g_state.map_block_index.size > 1) {
        struct uint256 best_hash;
        coins_view_cache_get_best_block(&g_coins_tip, &best_hash);
        if (!uint256_is_null(&best_hash)) {
            struct block_index *best = block_map_find(
                &g_state.map_block_index, &best_hash);
            if (best) {
                active_chain_set_tip(&g_state.chain_active, best);
                g_state.pindex_best_header = best;
                printf("Restored chain tip from coins DB: height=%d\n",
                       best->nHeight);
            } else {
                char hex[65];
                uint256_get_hex(&best_hash, hex);
                printf("Coins DB best block %s not in index.\n", hex);
                /* The chainstate is ahead of the block index. Find the
                 * highest block in the index that is at or below the
                 * chainstate height. Walk down from the most-work block. */
                struct block_index *fallback = NULL;
                size_t fi = 0;
                struct block_index *fp;
                while (block_map_next(&g_state.map_block_index, &fi,
                                       NULL, &fp)) {
                    if (fp && (fp->nStatus & BLOCK_HAVE_DATA) &&
                        fp->nChainTx > 0 &&
                        (!fallback || arith_uint256_compare(
                            &fp->nChainWork, &fallback->nChainWork) > 0))
                        fallback = fp;
                }
                if (fallback) {
                    /* Set this as tip but DON'T re-connect blocks.
                     * The chainstate is valid for a height >= fallback. */
                    active_chain_set_tip(&g_state.chain_active, fallback);
                    g_state.pindex_best_header = fallback;
                    printf("Fallback chain tip: height=%d\n",
                           fallback->nHeight);
                    skip_activate = true;
                }
            }
        }
        /* Find the best header (most chain work) */
        struct block_index *best_hdr = NULL;
        size_t iter = 0;
        struct block_index *pi;
        while (block_map_next(&g_state.map_block_index, &iter, NULL, &pi)) {
            if (pi && (!best_hdr ||
                arith_uint256_compare(&pi->nChainWork,
                                      &best_hdr->nChainWork) > 0))
                best_hdr = pi;
        }
        if (best_hdr)
            g_state.pindex_best_header = best_hdr;
    }

    /* Activate best chain (connects any new blocks beyond saved tip) */
    if (!skip_activate) {
        struct validation_state vs;
        validation_state_init(&vs);
        if (!activate_best_chain(&vs, &g_state, &g_coins_tip, params, NULL,
                                 ctx->datadir)) {
            fprintf(stderr, "Warning: Failed to activate best chain\n");
        }
    }

    struct block_index *tip = active_chain_tip(&g_state.chain_active);
    if (tip && tip->phashBlock) {
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        printf("Chain tip: height=%d hash=%s\n", tip->nHeight, hex);
    } else {
        printf("Chain tip: genesis\n");
    }

    /* Initialize mempool */
    tx_mempool_init(&g_mempool, 1000);

    /* Load persisted mempool from SQLite */
    if (g_active_node_db)
        node_db_sync_mempool_load(g_active_node_db, &g_mempool);

    /* Rescan blockchain for wallet transactions if wallet is behind chain tip.
     * Uses time_first_key with 2-hour safety margin to skip irrelevant blocks.
     * If no time_first_key is set (imported keys), scan from saved height only. */
    {
        struct block_index *chain_tip = active_chain_tip(&g_state.chain_active);
        int tip_height = active_chain_height(&g_state.chain_active);
        if (chain_tip && g_wallet.best_block_height < tip_height) {
            int scan_from = g_wallet.best_block_height > 0
                ? g_wallet.best_block_height + 1 : 0;
            if (g_wallet.time_first_key > 0 && scan_from == 0) {
                int64_t scan_time = g_wallet.time_first_key - 7200;
                for (int h = tip_height; h >= 0; h--) {
                    struct block_index *bi = active_chain_at(
                        &g_state.chain_active, h);
                    if (bi && (int64_t)bi->nTime < scan_time) {
                        scan_from = h + 1;
                        break;
                    }
                }
            }
            if (scan_from == 0 && g_wallet.best_block_height == 0 &&
                tip_height > 1000) {
                printf("Wallet scan height is 0 with %d blocks. "
                       "Use rescanblockchain RPC for targeted rescan.\n",
                       tip_height);
            } else {
                wallet_rescan(&g_wallet, &g_state.chain_active,
                              scan_from, tip_height, ctx->datadir);
            }
        }
    }

    /* Verify wallet UTXOs against actual UTXO set */
    wallet_verify_utxos(&g_wallet, &g_coins_tip);

    /* Sync wallet keys to SQLite */
    if (g_active_node_db)
        node_db_sync_wallet_keys(g_active_node_db, &g_wallet);

    /* Initialize message processor */
    msg_processor_init(&g_msg_processor, &g_state, &g_mempool,
                       &g_coins_tip, params, ctx->datadir,
                       &g_connman.manager);

    /* Initialize P2P connection manager */
    struct node_signals signals = {
        .get_height = msg_get_height,
        .process_messages = msg_process_messages,
        .send_messages = msg_send_messages,
        .initialize_node = NULL,
        .finalize_node = NULL,
        .ctx = &g_msg_processor,
    };
    connman_init(&g_connman, params, &signals);

    /* Load saved peer addresses */
    addr_db_read(&g_connman.manager, ctx->datadir);

    if (ctx->listen) {
        /* Bind IPv4 0.0.0.0 */
        struct net_service bind4;
        net_service_init(&bind4);
        unsigned char any4[4] = {0, 0, 0, 0};
        net_addr_set_ipv4(&bind4.addr, any4);
        bind4.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&g_connman.manager, &bind4, false))
            printf("P2P listening on 0.0.0.0:%d\n", ctx->p2p_port);
        /* Bind IPv6 [::] */
        struct net_service bind6;
        net_service_init(&bind6);
        bind6.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&g_connman.manager, &bind6, false))
            printf("P2P listening on [::]:%d\n", ctx->p2p_port);
    }

    connman_start(&g_connman);

    /* Initialize RPC */
    rpc_table_init(&g_rpc_table);
    rpc_blockchain_set_state(&g_state, &g_mempool, ctx->datadir);
    register_blockchain_rpc_commands(&g_rpc_table);

    rpc_rawtx_set_state(&g_state, &g_mempool, &g_coins_tip, ctx->datadir);
    rpc_rawtx_set_keystore(&g_wallet.keystore);
    register_rawtransaction_rpc_commands(&g_rpc_table);

    rpc_mining_set_state(&g_state, &g_mempool, &g_coins_tip, ctx->datadir);
    register_mining_rpc_commands(&g_rpc_table);

    rpc_misc_set_state(&g_state);
    rpc_misc_set_wallet(&g_wallet);
    register_misc_rpc_commands(&g_rpc_table);
    rpc_net_set_connman(&g_connman);
    register_net_rpc_commands(&g_rpc_table);

    rpc_wallet_set_state(&g_wallet, &g_state, ctx->datadir, &g_wallet_db,
                         &g_mempool, &g_connman);
    rpc_wallet_set_coins_tip(&g_coins_tip);
    rpc_wallet_set_node_db(g_active_node_db);
    register_wallet_rpc_commands(&g_rpc_table);

    /* Start RPC HTTP server */
    set_rpc_warmup_finished();
    rpc_http_start(&g_rpc_table, (uint16_t)ctx->rpc_port,
                    ctx->rpc_user, ctx->rpc_password, ctx->datadir);

    /* Start miner if -gen */
    if (ctx->gen) {
        g_gen.ms = &g_state;
        g_gen.coins_tip = &g_coins_tip;
        g_gen.mempool = &g_mempool;
        g_gen.params = params;
        g_gen.datadir = ctx->datadir;
        g_gen.num_threads = ctx->gen_threads > 0 ? ctx->gen_threads : 1;
        g_gen.coinbase_script.size = 0;

        if (ctx->miner_address) {
            size_t pk_pfx_len, sc_pfx_len;
            const unsigned char *pk_pfx = chain_params_base58_prefix(
                params, B58_PUBKEY_ADDRESS, &pk_pfx_len);
            const unsigned char *sc_pfx = chain_params_base58_prefix(
                params, B58_SCRIPT_ADDRESS, &sc_pfx_len);
            struct tx_destination dest;
            if (decode_destination(ctx->miner_address, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, &dest))
                script_for_destination(&g_gen.coinbase_script, &dest);
        }

        gen_start(&g_gen);
    }

    atomic_store(&g_running, true);
    printf("ZClassic C23 node initialized.\n");

    /* Run SQLite catchup in background thread */
    if (g_active_node_db) {
        static pthread_t catchup_thread;
        static struct {
            struct node_db *ndb;
            const struct active_chain *chain;
            const struct wallet *w;
            const char *datadir;
        } catchup_args;
        catchup_args.ndb = g_active_node_db;
        catchup_args.chain = &g_state.chain_active;
        catchup_args.w = &g_wallet;
        catchup_args.datadir = ctx->datadir;
        pthread_create(&catchup_thread, NULL, (void *(*)(void *))
            node_db_sync_catchup_thread, &catchup_args);
        pthread_detach(catchup_thread);
    }

    return true;
}

void app_shutdown(void)
{
    atomic_store(&g_running, false);

    printf("Shutting down...\n");

    if (g_gen.running)
        gen_stop(&g_gen);

    rpc_http_stop();

    /* Save peer addresses */
    addr_db_write(&g_connman.manager, g_datadir);

    connman_stop(&g_connman);
    connman_free(&g_connman);

    coins_view_cache_flush(&g_coins_tip);
    coins_view_cache_free(&g_coins_tip);
    coins_view_db_close(&g_coins_db);

    if (g_block_tree_open) {
        block_tree_db_close(&g_block_tree);
        g_block_tree_open = false;
    }

    if (g_wallet_db.open) {
        wallet_db_flush(&g_wallet_db, &g_wallet);
        wallet_db_close(&g_wallet_db);
    }
    if (g_node_db.open) {
        node_db_sync_mempool_save(&g_node_db, &g_mempool);
        node_db_close(&g_node_db);
    }
    g_active_node_db = NULL;
    wallet_free(&g_wallet);
    tx_mempool_free(&g_mempool);
    main_state_free(&g_state);
    zcash_free_params();

    ecc_verify_destroy();
    ecc_stop();

    printf("Shutdown complete.\n");
}

bool app_is_running(void)
{
    return atomic_load(&g_running);
}

void app_add_node(const char *host, int port)
{
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);

    /* Parse host:port if port embedded in string */
    if (port <= 0) {
        char *colon = strrchr(hostbuf, ':');
        if (colon && colon != hostbuf) {
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) {
                port = p;
                *colon = '\0';
            }
        }
    }

    uint16_t use_port = port > 0 ? (uint16_t)port
                                 : g_connman.manager.default_port;

    /* Resolve and connect directly (don't rely on addrman random selection) */
    struct net_address addr;
    net_address_init(&addr);
    addr.svc.port = use_port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(hostbuf, NULL, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
            memset(addr.svc.addr.ip, 0, 10);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            memcpy(addr.svc.addr.ip + 12, &s4->sin_addr, 4);
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        }
        freeaddrinfo(res);

        printf("Connecting to addnode %s:%u\n", hostbuf, use_port);
        connman_open_connection(&g_connman, &addr);
    } else {
        printf("Failed to resolve addnode %s\n", hostbuf);
    }
}

void app_start_metrics(bool mining)
{
    g_metrics.ms = &g_state;
    g_metrics.cm = &g_connman;
    g_metrics.params = chain_params_get();
    g_metrics.mining = mining;
    metrics_start(&g_metrics);
}

void app_stop_metrics(void)
{
    metrics_stop(&g_metrics);
}
