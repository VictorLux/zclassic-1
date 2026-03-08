/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "init/init.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
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
#include "zcash/params_init.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct main_state g_state;
static struct coins_view_db g_coins_db;
static struct coins_view_cache g_coins_tip;
static struct block_tree_db g_block_tree;
static bool g_block_tree_open = false;
static struct tx_mempool g_mempool;
static struct rpc_table g_rpc_table;
static struct msg_processor g_msg_processor;
static struct connman g_connman;
static struct wallet g_wallet;
static struct gen_context g_gen;
static _Atomic bool g_running = false;

void app_context_defaults(struct app_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->datadir = NULL;
    ctx->params_dir = NULL;
    ctx->rpc_port = 8232;
    ctx->p2p_port = 8233;
    ctx->listen = false;
    ctx->checkpoints_enabled = true;
}

static struct block_index *insert_block_index_cb(void *ctx_ptr,
                                                  const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index(
        (struct chainstate *)&ms->map_block_index, hash);
}

static bool load_block_index(struct main_state *ms,
                              const struct chain_params *params)
{
    if (!g_block_tree_open)
        return false;

    if (!block_tree_db_load_block_index_guts(&g_block_tree,
                                              insert_block_index_cb, ms))
        return false;

    if (ms->map_block_index.size == 0) {
        struct block_index *genesis = chainstate_insert_block_index(
            (struct chainstate *)&ms->map_block_index,
            &params->consensus.hashGenesisBlock);
        if (genesis) {
            genesis->nHeight = 0;
            genesis->nStatus = BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA;
            genesis->nTx = 1;
            genesis->nChainTx = 1;
        }
    }

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

    main_state_init(&g_state);
    g_state.fTxIndex = ctx->tx_index;
    g_state.fCheckpointsEnabled = ctx->checkpoints_enabled;

    /* Load ZK verification keys */
    if (ctx->params_dir) {
        printf("Loading verification keys...\n");
        if (!zcash_init_params(ctx->params_dir)) {
            fprintf(stderr, "Error: Failed to load verification keys from %s\n",
                    ctx->params_dir);
            return false;
        }
        printf("Verification keys loaded.\n");
    }

    /* Open block index database */
    char blocktree_path[1024];
    snprintf(blocktree_path, sizeof(blocktree_path), "%s/blocks/index",
             ctx->datadir);
    if (block_tree_db_open(&g_block_tree, blocktree_path,
                           256 << 20, false, false)) {
        g_block_tree_open = true;
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

    /* Activate best chain */
    struct validation_state vs;
    validation_state_init(&vs);
    if (!activate_best_chain(&vs, &g_state, &g_coins_tip, params, NULL,
                             ctx->datadir)) {
        fprintf(stderr, "Warning: Failed to activate best chain\n");
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

    /* Initialize wallet */
    wallet_init(&g_wallet);
    wallet_top_up_key_pool(&g_wallet, DEFAULT_KEYPOOL_SIZE);
    printf("Wallet initialized with %zu keys.\n", g_wallet.keystore.num_keys);

    /* Initialize message processor */
    msg_processor_init(&g_msg_processor, &g_state, &g_mempool,
                       &g_coins_tip, params, ctx->datadir);

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

    if (ctx->listen) {
        struct net_service bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.port = (uint16_t)ctx->p2p_port;
        bind_listen_port(&g_connman.manager, &bind_addr, false);
        printf("P2P listening on port %d\n", ctx->p2p_port);
    }

    connman_start(&g_connman);

    /* Initialize RPC */
    rpc_table_init(&g_rpc_table);
    rpc_blockchain_set_state(&g_state, &g_mempool, ctx->datadir);
    register_blockchain_rpc_commands(&g_rpc_table);

    rpc_rawtx_set_state(&g_state, &g_mempool, &g_coins_tip, ctx->datadir);
    register_rawtransaction_rpc_commands(&g_rpc_table);

    rpc_mining_set_state(&g_state, &g_mempool, &g_coins_tip, ctx->datadir);
    register_mining_rpc_commands(&g_rpc_table);

    rpc_misc_set_state(&g_state);
    register_misc_rpc_commands(&g_rpc_table);
    register_net_rpc_commands(&g_rpc_table);

    rpc_wallet_set_state(&g_wallet);
    register_wallet_rpc_commands(&g_rpc_table);

    /* Start RPC HTTP server */
    set_rpc_warmup_finished();
    rpc_http_start(&g_rpc_table, (uint16_t)ctx->rpc_port, NULL, NULL);

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
    return true;
}

void app_shutdown(void)
{
    atomic_store(&g_running, false);

    printf("Shutting down...\n");

    if (g_gen.running)
        gen_stop(&g_gen);

    rpc_http_stop();
    connman_stop(&g_connman);
    connman_free(&g_connman);

    coins_view_cache_flush(&g_coins_tip);
    coins_view_cache_free(&g_coins_tip);
    coins_view_db_close(&g_coins_db);

    if (g_block_tree_open) {
        block_tree_db_close(&g_block_tree);
        g_block_tree_open = false;
    }

    wallet_free(&g_wallet);
    tx_mempool_free(&g_mempool);
    main_state_free(&g_state);
    zcash_free_params();

    printf("Shutdown complete.\n");
}

bool app_is_running(void)
{
    return atomic_load(&g_running);
}

void app_add_node(const char *host, int port)
{
    connman_add_seed_node(&g_connman, host,
                           port > 0 ? (uint16_t)port
                                    : g_connman.manager.default_port);
}
