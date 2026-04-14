/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "views/format_helpers.h"
#include "controllers/blockchain_controller.h"
#include "config/runtime.h"
#include "controllers/strong_params.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/checkpoints.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "event/event.h"
#include "chain/pow.h"
#include "coins/utxo_commitment.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/undo.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "json/json.h"
#include "models/zslp.h"
#include "primitives/block.h"
#include "storage/coins_db.h"
#include "storage/dbwrapper.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "validation/update_coins.h"
#include "validation/connect_block.h"
/* png_writer.h moved to hodl_controller.c */
#include "storage/block_index_db.h"
#include "zslp/slp.h"
#include "znam/znam.h"
#include "encoding/base58.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "models/block.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "controllers/sync_controller.h"
#include "controllers/network_controller.h"
#include "net/connman.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

struct blockchain_context {
    struct main_state *main_state;
    struct tx_mempool *mempool;
    const char *datadir;
    struct coins_view_db *coins_db;
    struct coins_view_cache *coins_tip;
    struct node_db *node_db;
};

static struct blockchain_context g_blockchain_ctx = {0};

static struct blockchain_context *blockchain_ctx(void)
{
    return &g_blockchain_ctx;
}

void rpc_blockchain_set_state(struct main_state *ms, struct tx_mempool *mp,
                               const char *datadir)
{
    g_blockchain_ctx.main_state = ms;
    g_blockchain_ctx.mempool = mp;
    g_blockchain_ctx.datadir = datadir;
}

void rpc_blockchain_set_coins_db(struct coins_view_db *cvdb,
                                  struct coins_view_cache *coins_tip)
{
    g_blockchain_ctx.coins_db = cvdb;
    g_blockchain_ctx.coins_tip = coins_tip;
}

static double get_difficulty(const struct block_index *bi)
{
    return bi ? difficulty_from_bits(bi->nBits) : 1.0;
}

static bool rpc_getblockcount(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getblockcount\nReturns the number of blocks.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockcount: main_state not initialized");
    }
    json_set_int(result, active_chain_height(&ctx->main_state->chain_active));
    return true;
}

static bool rpc_getbestblockhash(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getbestblockhash\nReturns the hash of the best block.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getbestblockhash: main_state not initialized");
    }
    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);
    if (!tip || !tip->phashBlock) {
        json_set_str(result, "No tip");
        LOG_FAIL("blockchain", "getbestblockhash: chain tip or phashBlock is NULL");
    }
    char hex[65];
    uint256_get_hex(tip->phashBlock, hex);
    json_set_str(result, hex);
    return true;
}

static bool rpc_getdifficulty(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getdifficulty\nReturns proof-of-work difficulty.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getdifficulty: main_state not initialized");
    }
    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);
    json_set_real(result, get_difficulty(tip));
    return true;
}

static bool rpc_getblockhash(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result, "getblockhash height\nReturns hash of block at height.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    int height = (int)rpc_require_int(&p, 0, "height");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getblockhash: invalid params");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockhash: main_state not initialized");
    }
    struct block_index *bi = active_chain_at(&ctx->main_state->chain_active, height);
    if (!bi || !bi->phashBlock) {
        json_set_str(result, "Block height out of range");
        LOG_FAIL("blockchain", "getblockhash: height %d out of range", height);
    }
    char hex[65];
    uint256_get_hex(bi->phashBlock, hex);
    json_set_str(result, hex);
    return true;
}

static void block_header_to_json(const struct block_index *bi,
                                  struct json_value *result)
{
    json_set_object(result);
    if (!bi || !bi->phashBlock)
        return;

    char hex[65];
    uint256_get_hex(bi->phashBlock, hex);
    json_push_kv_str(result, "hash", hex);
    json_push_kv_int(result, "confirmations", 1);
    json_push_kv_int(result, "height", bi->nHeight);
    json_push_kv_int(result, "version", bi->nVersion);

    uint256_get_hex(&bi->hashMerkleRoot, hex);
    json_push_kv_str(result, "merkleroot", hex);

    json_push_kv_int(result, "time", (int64_t)bi->nTime);
    json_push_kv_int(result, "nonce", 0);

    char bits_hex[9];
    snprintf(bits_hex, sizeof(bits_hex), "%08x", bi->nBits);
    json_push_kv_str(result, "bits", bits_hex);

    json_push_kv_real(result, "difficulty", get_difficulty(bi));

    if (bi->pprev && bi->pprev->phashBlock) {
        uint256_get_hex(bi->pprev->phashBlock, hex);
        json_push_kv_str(result, "previousblockhash", hex);
    }
}

static bool rpc_getblockheader(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
             "getblockheader \"hash\" ( verbose )\nReturns block header.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hash_str = rpc_require_str(&p, 0, "hash");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getblockheader: invalid params");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockheader: main_state not initialized");
    }
    struct uint256 hash;
    uint256_set_hex(&hash, hash_str);

    struct block_index *bi = block_map_find(&ctx->main_state->map_block_index, &hash);
    if (!bi) {
        json_set_str(result, "Block not found");
        LOG_FAIL("blockchain", "getblockheader: block %s not found", hash_str);
    }

    block_header_to_json(bi, result);
    return true;
}

static bool rpc_getblock(const struct json_value *params, bool help,
                          struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
             "getblock \"hash\" ( verbose )\nReturns block data.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hash_str = rpc_require_str(&p, 0, "hash");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getblock: invalid params");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblock: main_state not initialized");
    }
    struct uint256 hash;
    uint256_set_hex(&hash, hash_str);

    struct block_index *bi = block_map_find(&ctx->main_state->map_block_index, &hash);
    if (!bi) {
        json_set_str(result, "Block not found");
        LOG_FAIL("blockchain", "getblock: block %s not found", hash_str);
    }

    block_header_to_json(bi, result);

    json_push_kv_int(result, "size", 0);
    json_push_kv_int(result, "tx", (int64_t)bi->nTx);

    return true;
}

static bool rpc_getblockchaininfo(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getblockchaininfo\nReturns blockchain state info.");
    if (!ctx->main_state) {
        json_set_str(result, "Not initialized");
        LOG_FAIL("blockchain", "getblockchaininfo: main_state not initialized");
    }

    const struct chain_params *cp = chain_params_get();

    json_set_object(result);
    json_push_kv_str(result, "chain", cp->strNetworkID);

    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);
    json_push_kv_int(result, "blocks", tip ? tip->nHeight : 0);
    struct block_index *best_hdr = ctx->main_state->pindex_best_header;
    int header_height = best_hdr ? best_hdr->nHeight : (tip ? tip->nHeight : 0);
    json_push_kv_int(result, "headers", header_height);
    json_push_kv_int(result, "best_header_height", header_height);

    if (tip && tip->phashBlock) {
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        json_push_kv_str(result, "bestblockhash", hex);
    }

    json_push_kv_real(result, "difficulty", get_difficulty(tip));
    struct connman *cm = rpc_net_get_connman();
    int max_peer_h = cm ? connman_max_peer_height(cm) : 0;
    int our_h = tip ? tip->nHeight : 0;
    double progress = 1.0;
    if (max_peer_h > 0 && our_h < max_peer_h)
        progress = (double)our_h / (double)max_peer_h;
    json_push_kv_real(result, "verificationprogress", progress);

    /* Upgrades */
    struct json_value upgrades = {0};
    json_set_object(&upgrades);
    json_push_kv(result, "upgrades", &upgrades);
    json_free(&upgrades);

    return true;
}

static bool rpc_getmempoolinfo(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result, "getmempoolinfo\nReturns mempool state.");

    json_set_object(result);
    json_push_kv_int(result, "size",
                     ctx->mempool ? (int64_t)ctx->mempool->num_entries : 0);
    json_push_kv_int(result, "bytes",
                     ctx->mempool ? (int64_t)ctx->mempool->total_tx_size : 0);
    return true;
}

/* gettxoutsetinfo: UTXO set statistics matching legacy node output. */
static bool rpc_gettxoutsetinfo(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "gettxoutsetinfo\n"
        "\nReturns statistics about the UTXO set.\n");

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Coins database not available");
        LOG_FAIL("blockchain", "gettxoutsetinfo: coins database not available");
    }
    if (!ctx->main_state || !active_chain_tip(&ctx->main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        LOG_FAIL("blockchain", "gettxoutsetinfo: chain not loaded or no tip");
    }

    /* Flush in-memory UTXO cache to SQLite for accurate totals */
    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    int tip_height = active_chain_height(&ctx->main_state->chain_active);
    struct block_index *tip = active_chain_tip(&ctx->main_state->chain_active);

    int64_t total_amount = 0;
    int64_t num_txs = 0;
    int64_t num_txouts = 0;

    /* Query UTXO set from SQLite */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT COUNT(DISTINCT txid), COUNT(*), COALESCE(SUM(value),0)"
        " FROM utxos", -1, &s, NULL);
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        num_txs = sqlite3_column_int64(s, 0);
        num_txouts = sqlite3_column_int64(s, 1);
        total_amount = sqlite3_column_int64(s, 2);
    }
    sqlite3_finalize(s);

    json_set_object(result);
    json_push_kv_int(result, "height", tip_height);
    if (tip && tip->phashBlock) {
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        json_push_kv_str(result, "bestblock", hex);
    }
    json_push_kv_int(result, "transactions", num_txs);
    json_push_kv_int(result, "txouts", num_txouts);

    char amt[32];
    snprintf(amt, sizeof(amt), "%lld.%08lld",
             (long long)(total_amount / ZATOSHI_PER_ZCL),
             (long long)(total_amount % ZATOSHI_PER_ZCL));
    json_push_kv_str(result, "total_amount", amt);

    return true;
}


/* reindexchainstate: Wipe and rebuild the UTXO set by replaying all blocks.
 * Fixes any corrupt coins entries from prior serialization bugs. */
static bool rpc_reindexchainstate(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "reindexchainstate\n"
        "\nWipes the chainstate (UTXO database) and rebuilds it by replaying\n"
        "all blocks from genesis to chain tip. This fixes any corrupt coins\n"
        "entries from prior serialization bugs.\n"
        "\nWARNING: This operation takes a long time (minutes to hours).\n"
        "The node will not process new blocks or transactions during reindex.\n");

    if (!ctx->coins_db || !ctx->coins_tip || !ctx->main_state || !ctx->datadir) {
        json_set_str(result, "Node not fully initialized");
        LOG_FAIL("blockchain", "reindexchainstate: node not fully initialized (coins_db=%p coins_tip=%p main_state=%p datadir=%p)",
                 (void *)ctx->coins_db, (void *)ctx->coins_tip, (void *)ctx->main_state, (void *)ctx->datadir);
    }

    int tip_height = active_chain_height(&ctx->main_state->chain_active);
    if (tip_height < 0) {
        json_set_str(result, "No active chain");
        LOG_FAIL("blockchain", "reindexchainstate: no active chain (tip_height=%d)", tip_height);
    }

    printf("reindexchainstate: rebuilding UTXO set for %d blocks...\n",
           tip_height + 1);
    fflush(stdout);

    /* Step 1: Flush and free the in-memory cache */
    coins_view_cache_flush(ctx->coins_tip);
    coins_view_cache_free(ctx->coins_tip);

    /* Step 2: Close and reopen coins DB with wipe=true */
    coins_view_db_close(ctx->coins_db);

    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", ctx->datadir);
    if (!coins_view_db_open(ctx->coins_db, coins_path,
                            450 << 20, false, true)) {
        json_set_str(result, "Failed to reopen coins database");
        LOG_FAIL("blockchain", "reindexchainstate: failed to reopen coins database at %s", coins_path);
    }

    /* Step 3: Reinitialize coins cache */
    coins_view_cache_init(ctx->coins_tip, &ctx->coins_db->view);

    /* Step 3.5: Reset sapling tree state — must replay from empty.
     * Use the global node_db (set later in file via rpc_blockchain_set_node_db). */
    {
        struct node_db *ndb = app_runtime_node_db();
        if (ndb && ndb->open) {
            node_db_state_set(ndb, "sapling_tree", NULL, 0);
            node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);
        }
    }

    int64_t t_start = (int64_t)time(NULL);
    int errors = 0;

    /* Step 4: Replay all blocks */
    for (int h = 0; h <= tip_height; h++) {
        struct block_index *pindex = active_chain_at(
            &ctx->main_state->chain_active, h);
        if (!pindex) {
            printf("reindexchainstate: missing block_index at height %d\n", h);
            errors++;
            continue;
        }

        struct block blk;
        if (!read_block_from_disk_index(&blk, pindex, ctx->datadir)) {
            printf("reindexchainstate: failed to read block at height %d\n", h);
            errors++;
            continue;
        }

        /* Genesis block: just set best block */
        if (h == 0) {
            struct uint256 block_hash;
            block_header_get_hash(&blk.header, &block_hash);
            /* Low-level: bypasses csr — reindexchainstate is an
             * operator-invoked UTXO replay across every block from
             * genesis to the current tip. active_chain never moves
             * during the replay; only coins_tip's hash_block is being
             * resynced one block at a time. Routing each iteration
             * through csr_commit_tip would require O(N) full
             * cross-checks and would report spurious backward moves. */
            coins_view_cache_set_best_block(ctx->coins_tip, &block_hash);
            block_free(&blk);
            if (h % 10000 == 0) {
                printf("  height %d/%d\n", h, tip_height);
                fflush(stdout);
            }
            continue;
        }

        /* Apply each transaction to the UTXO set */
        for (size_t i = 0; i < blk.num_vtx; i++) {
            update_coins(&blk.vtx[i], ctx->coins_tip, pindex->nHeight);
        }

        /* Set best block hash.
         * Low-level: bypasses csr (see genesis branch above for the
         * rationale) — this is inside the reindexchainstate replay
         * loop, not a tip-commit operation. */
        struct uint256 block_hash;
        block_header_get_hash(&blk.header, &block_hash);
        coins_view_cache_set_best_block(ctx->coins_tip, &block_hash);

        block_free(&blk);

        /* Periodic flush every 10000 blocks */
        if (h % 10000 == 0) {
            coins_view_cache_flush(ctx->coins_tip);
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            double rate = elapsed > 0 ? (double)h / (double)elapsed : 0;
            int eta = rate > 0 ? (int)((tip_height - h) / rate) : 0;
            printf("  height %d/%d (%.0f blk/s, ETA %dm%ds)\n",
                   h, tip_height, rate, eta / 60, eta % 60);
            fflush(stdout);
        }
    }

    /* Step 5: Final flush */
    coins_view_cache_flush(ctx->coins_tip);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("reindexchainstate: complete in %lldm%llds (%d errors)\n",
           (long long)(elapsed / 60), (long long)(elapsed % 60), errors);
    fflush(stdout);

    /* Report results */
    json_set_object(result);
    json_push_kv_int(result, "height", tip_height);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    json_push_kv_int(result, "errors", errors);
    json_push_kv_str(result, "status", errors == 0 ? "success" : "completed with errors");

    return true;
}

/* ── Phase B data structures for parallel block extraction ── */

#define N_INDEX_THREADS 4
#define IDX_BATCH_CAP   4000000

struct idx_tx_input {
    uint8_t txid[32]; uint8_t prev_txid[32];
    uint32_t vin_index; uint32_t prev_vout; int height;
};
struct idx_tx_output {
    uint8_t txid[32]; int64_t value;
    uint8_t addr_hash[20]; bool has_addr;
    uint32_t vout; int script_type; int height;
};
struct idx_joinsplit {
    uint8_t txid[32]; uint8_t anchor[32];
    uint8_t nullifiers[2][32];
    int64_t vpub_old; int64_t vpub_new;
    uint32_t js_index; int height;
};
struct idx_sapling_spend {
    uint8_t txid[32]; uint8_t cv[32]; uint8_t anchor[32];
    uint8_t nullifier[32]; uint8_t rk[32];
    uint32_t spend_index; int height;
};
struct idx_sapling_output {
    uint8_t txid[32]; uint8_t cv[32]; uint8_t cm[32];
    uint8_t ephemeral_key[32];
    uint32_t output_index; int height;
};
struct idx_opret {
    uint8_t txid[32]; uint8_t script[256];
    size_t script_len; int is_slp; int height;
};
struct idx_block_shielded {
    int height; int64_t sprout_value; int64_t sapling_value;
    uint8_t block_hash[32];
    uint32_t num_js; uint32_t num_ss; uint32_t num_so;
    uint32_t num_tx;
};

struct blk_loc {
    int file;
    uint32_t offset;
    uint32_t size;
};

struct worker_ctx {
    int thread_id;
    int height_from, height_to;
    const struct blk_loc *locs;
    int max_height;
    const char *legacy_dir;

    /* Output arrays -- allocated by thread */
    struct idx_tx_input *inputs;      int num_inputs;      int cap_inputs;
    struct idx_tx_output *outputs;    int num_outputs;     int cap_outputs;
    struct idx_joinsplit *joinsplits; int num_joinsplits;  int cap_joinsplits;
    struct idx_sapling_spend *sspends;   int num_sspends;  int cap_sspends;
    struct idx_sapling_output *soutputs; int num_soutputs; int cap_soutputs;
    struct idx_opret *oprets;         int num_oprets;      int cap_oprets;
    struct idx_block_shielded *blocks_sh; int num_blocks_sh; int cap_blocks_sh;
};

/* Macro to grow a per-thread array if at capacity */
#define IDX_GROW(arr, num, cap, type) do {            \
    if ((num) >= (cap)) {                             \
        (cap) = (cap) < 1024 ? 1024 : (cap) * 2;     \
        void *_tmp = realloc((arr), (size_t)(cap) * sizeof(type)); /* raw-alloc-ok */ \
        if (!_tmp) { free(arr); (arr) = NULL; (cap) = 0; break; } \
        (arr) = _tmp;                                 \
    }                                                 \
} while (0)

static bool indexlegacy_exec_checked(sqlite3 *db, const char *sql,
                                   const char *label)
{
    if (!db || !sql)
        return false;

    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "indexlegacy: %s failed: %s\n",
                label, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool indexlegacy_prepare_checked(sqlite3 *db, const char *sql,
                                      sqlite3_stmt **stmt,
                                      const char *label)
{
    if (!db || !sql || !stmt)
        return false;

    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK ||
        !*stmt) {
        fprintf(stderr, "indexlegacy: %s failed: %s\n",
                label, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool indexlegacy_step_checked(sqlite3_stmt *stmt, sqlite3 *db,
                                   const char *label)
{
    int rc;

    if (!stmt || !db)
        return false;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "indexlegacy: %s failed: rc=%d err=%s\n",
                label, rc, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool indexlegacy_node_tx_begin_checked(struct node_db *ndb,
                                              const char *label)
{
    if (!ndb || !ndb->open || !node_db_begin(ndb)) {
        fprintf(stderr, "indexlegacy: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static bool indexlegacy_node_tx_commit_checked(struct node_db *ndb,
                                               const char *label)
{
    if (!ndb || !ndb->open || !node_db_commit(ndb)) {
        fprintf(stderr, "indexlegacy: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static bool indexlegacy_node_tx_rollback_checked(struct node_db *ndb,
                                                 const char *label)
{
    if (!ndb || !ndb->open || !node_db_rollback(ndb)) {
        fprintf(stderr, "indexlegacy: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static void *index_worker(void *arg) {
    struct worker_ctx *ctx = arg;

    /* Allocate output arrays */
    ctx->cap_inputs = IDX_BATCH_CAP;
    ctx->cap_outputs = IDX_BATCH_CAP;
    ctx->cap_joinsplits = IDX_BATCH_CAP / 4;
    ctx->cap_sspends = IDX_BATCH_CAP / 4;
    ctx->cap_soutputs = IDX_BATCH_CAP / 4;
    ctx->cap_oprets = IDX_BATCH_CAP / 8;
    ctx->cap_blocks_sh = (ctx->height_to - ctx->height_from + 2);

    ctx->inputs    = zcl_malloc((size_t)ctx->cap_inputs    * sizeof(*ctx->inputs), "idx_inputs");
    ctx->outputs   = zcl_malloc((size_t)ctx->cap_outputs   * sizeof(*ctx->outputs), "idx_outputs");
    ctx->joinsplits= zcl_malloc((size_t)ctx->cap_joinsplits* sizeof(*ctx->joinsplits), "idx_joinsplits");
    ctx->sspends   = zcl_malloc((size_t)ctx->cap_sspends   * sizeof(*ctx->sspends), "idx_sspends");
    ctx->soutputs  = zcl_malloc((size_t)ctx->cap_soutputs  * sizeof(*ctx->soutputs), "idx_soutputs");
    ctx->oprets    = zcl_malloc((size_t)ctx->cap_oprets    * sizeof(*ctx->oprets), "idx_oprets");
    ctx->blocks_sh = zcl_malloc((size_t)ctx->cap_blocks_sh * sizeof(*ctx->blocks_sh), "idx_blocks_sh");

    ctx->num_inputs = ctx->num_outputs = ctx->num_joinsplits = 0;
    ctx->num_sspends = ctx->num_soutputs = ctx->num_oprets = 0;
    ctx->num_blocks_sh = 0;

    int cur_file = -1;
    uint8_t *mdata = NULL;
    size_t msize = 0;

    printf("  Phase B: thread %d processing heights %d-%d\n",
           ctx->thread_id, ctx->height_from, ctx->height_to);
    fflush(stdout);

    for (int h = ctx->height_from; h <= ctx->height_to; h++) {
        if (h > ctx->max_height) break;
        if (ctx->locs[h].size == 0) continue;

        /* mmap block file if needed */
        if (ctx->locs[h].file != cur_file) {
            if (mdata) munmap(mdata, msize);
            char path[1200];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     ctx->legacy_dir, ctx->locs[h].file);
            struct stat st2;
            if (stat(path, &st2) != 0) { mdata = NULL; continue; }
            int fd2 = open(path, O_RDONLY);
            if (fd2 < 0) { mdata = NULL; continue; }
            msize = (size_t)st2.st_size;
            mdata = mmap(NULL, msize, PROT_READ, MAP_PRIVATE, fd2, 0);
            close(fd2);
            if (mdata == MAP_FAILED) { mdata = NULL; continue; }
            cur_file = ctx->locs[h].file;
        }
        if (!mdata) continue;
        if (ctx->locs[h].offset + ctx->locs[h].size > msize) continue;

        /* Deserialize block */
        struct block blk2;
        block_init(&blk2);
        struct byte_stream bs2;
        stream_init_from_data(&bs2, mdata + ctx->locs[h].offset,
                              ctx->locs[h].size);
        if (!block_deserialize(&blk2, &bs2)) {
            stream_free(&bs2);
            block_free(&blk2);
            continue;
        }
        stream_free(&bs2);

        struct uint256 bh2;
        block_header_get_hash(&blk2.header, &bh2);

        /* Per-block shielded accumulators */
        int64_t bk_sprout = 0, bk_sapling = 0;
        uint32_t bk_njs = 0, bk_nss = 0, bk_nso = 0;

        for (size_t ti = 0; ti < blk2.num_vtx; ti++) {
            const struct transaction *tx2 = &blk2.vtx[ti];

            /* Transparent inputs (except coinbase) */
            if (ti > 0) {
                for (size_t j = 0; j < tx2->num_vin; j++) {
                    IDX_GROW(ctx->inputs, ctx->num_inputs,
                             ctx->cap_inputs, struct idx_tx_input);
                    struct idx_tx_input *inp = &ctx->inputs[ctx->num_inputs++];
                    memcpy(inp->txid, tx2->hash.data, 32);
                    memcpy(inp->prev_txid, tx2->vin[j].prevout.hash.data, 32);
                    inp->vin_index = (uint32_t)j;
                    inp->prev_vout = tx2->vin[j].prevout.n;
                    inp->height = h;
                }
            }

            /* JoinSplits + sprout nullifiers */
            for (size_t j = 0; j < tx2->num_joinsplit; j++) {
                struct js_description *js = &tx2->v_joinsplit[j];
                bk_sprout += js->vpub_old - js->vpub_new;

                IDX_GROW(ctx->joinsplits, ctx->num_joinsplits,
                         ctx->cap_joinsplits, struct idx_joinsplit);
                struct idx_joinsplit *ij = &ctx->joinsplits[ctx->num_joinsplits++];
                memcpy(ij->txid, tx2->hash.data, 32);
                memcpy(ij->anchor, js->anchor.data, 32);
                for (int nf = 0; nf < ZC_NUM_JS_INPUTS; nf++)
                    memcpy(ij->nullifiers[nf], js->nullifiers[nf].data, 32);
                ij->vpub_old = js->vpub_old;
                ij->vpub_new = js->vpub_new;
                ij->js_index = (uint32_t)j;
                ij->height = h;
                bk_njs++;
            }

            /* Sapling spends */
            for (size_t j = 0; j < tx2->num_shielded_spend; j++) {
                struct spend_description *sd = &tx2->v_shielded_spend[j];
                IDX_GROW(ctx->sspends, ctx->num_sspends,
                         ctx->cap_sspends, struct idx_sapling_spend);
                struct idx_sapling_spend *isp = &ctx->sspends[ctx->num_sspends++];
                memcpy(isp->txid, tx2->hash.data, 32);
                memcpy(isp->cv, sd->cv.data, 32);
                memcpy(isp->anchor, sd->anchor.data, 32);
                memcpy(isp->nullifier, sd->nullifier.data, 32);
                memcpy(isp->rk, sd->rk.data, 32);
                isp->spend_index = (uint32_t)j;
                isp->height = h;
                bk_nss++;
            }

            /* Sapling outputs */
            for (size_t j = 0; j < tx2->num_shielded_output; j++) {
                struct output_description *od = &tx2->v_shielded_output[j];
                IDX_GROW(ctx->soutputs, ctx->num_soutputs,
                         ctx->cap_soutputs, struct idx_sapling_output);
                struct idx_sapling_output *iso = &ctx->soutputs[ctx->num_soutputs++];
                memcpy(iso->txid, tx2->hash.data, 32);
                memcpy(iso->cv, od->cv.data, 32);
                memcpy(iso->cm, od->cm.data, 32);
                memcpy(iso->ephemeral_key, od->ephemeral_key.data, 32);
                iso->output_index = (uint32_t)j;
                iso->height = h;
                bk_nso++;
            }

            /* Sapling value balance */
            bk_sapling += tx2->value_balance;

            /* Transparent outputs */
            for (size_t j = 0; j < tx2->num_vout; j++) {
                const uint8_t *scr = tx2->vout[j].script_pub_key.data;
                size_t scr_len = tx2->vout[j].script_pub_key.size;

                IDX_GROW(ctx->outputs, ctx->num_outputs,
                         ctx->cap_outputs, struct idx_tx_output);
                struct idx_tx_output *ot = &ctx->outputs[ctx->num_outputs++];
                memcpy(ot->txid, tx2->hash.data, 32);
                ot->value = tx2->vout[j].value;
                ot->vout = (uint32_t)j;
                ot->height = h;
                ot->has_addr = false;
                ot->script_type = 0;

                if (scr_len == 25 && scr[0] == 0x76 && scr[1] == 0xa9 &&
                    scr[2] == 0x14 && scr[23] == 0x88 && scr[24] == 0xac) {
                    memcpy(ot->addr_hash, scr + 3, 20);
                    ot->has_addr = true;
                    ot->script_type = SCRIPT_P2PKH;
                } else if (scr_len == 23 && scr[0] == 0xa9 && scr[1] == 0x14 &&
                           scr[22] == 0x87) {
                    memcpy(ot->addr_hash, scr + 2, 20);
                    ot->has_addr = true;
                    ot->script_type = SCRIPT_P2SH;
                }
            }

            /* OP_RETURN (first per tx) */
            for (size_t j = 0; j < tx2->num_vout; j++) {
                if (tx2->vout[j].script_pub_key.size > 0 &&
                    tx2->vout[j].script_pub_key.data[0] == 0x6a) {
                    const uint8_t *scr = tx2->vout[j].script_pub_key.data;
                    size_t scr_len = tx2->vout[j].script_pub_key.size;
                    struct slp_message slp_chk;
                    int is_slp_val = slp_parse(scr, scr_len, &slp_chk) ? 1 : 0;

                    IDX_GROW(ctx->oprets, ctx->num_oprets,
                             ctx->cap_oprets, struct idx_opret);
                    struct idx_opret *op = &ctx->oprets[ctx->num_oprets++];
                    memcpy(op->txid, tx2->hash.data, 32);
                    op->script_len = scr_len > 256 ? 256 : scr_len;
                    memcpy(op->script, scr, op->script_len);
                    op->is_slp = is_slp_val;
                    op->height = h;
                    break; /* only first OP_RETURN per tx */
                }
            }
        }

        /* Record per-block shielded data */
        IDX_GROW(ctx->blocks_sh, ctx->num_blocks_sh,
                 ctx->cap_blocks_sh, struct idx_block_shielded);
        struct idx_block_shielded *bsh = &ctx->blocks_sh[ctx->num_blocks_sh++];
        bsh->height = h;
        bsh->sprout_value = bk_sprout;
        bsh->sapling_value = bk_sapling;
        memcpy(bsh->block_hash, bh2.data, 32);
        bsh->num_js = bk_njs;
        bsh->num_ss = bk_nss;
        bsh->num_so = bk_nso;
        bsh->num_tx = (uint32_t)blk2.num_vtx;

        block_free(&blk2);
    }

    if (mdata) munmap(mdata, msize);
    return NULL;
}

/* ── importchainstate: read UTXO set from external LevelDB chainstate ── */

static bool rpc_importchainstate(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
        "importchainstate \"chainstate_path\"\n"
        "\nRebuild the UTXO index from an external LevelDB chainstate directory.\n"
        "Use this to import the complete UTXO set from a zclassicd node:\n"
        "  importchainstate /home/user/.zclassic/chainstate\n"
        "\nThis replaces all UTXOs in SQLite with those from the given chainstate.\n"
        "The source node should be stopped to avoid LevelDB lock conflicts.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *cs_path = rpc_require_str(&p, 0, "chainstate_path");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("blockchain", "importchainstate: invalid params"); }

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Node database not open");
        LOG_FAIL("blockchain", "importchainstate: node database not open");
    }

    printf("importchainstate: opening %s...\n", cs_path);
    fflush(stdout);

    struct coins_view_db ext_db;
    memset(&ext_db, 0, sizeof(ext_db));
    if (!coins_view_db_open(&ext_db, cs_path, 256, false, false)) {
        json_set_str(result, "Cannot open chainstate LevelDB");
        LOG_FAIL("blockchain", "importchainstate: cannot open LevelDB at %s", cs_path);
    }

    /* Read best block hash from LevelDB before importing.
     * This is the height at which the UTXO set was snapshotted. */
    struct uint256 ldb_best_block;
    memset(&ldb_best_block, 0, sizeof(ldb_best_block));
    coins_view_db_get_best_block(&ext_db, &ldb_best_block);

    struct node_db import_db;
    struct node_db *import_target = ctx->node_db;
    if (node_db_sync_open_private_db_like(ctx->node_db, &import_db))
        import_target = &import_db;

    int count = node_db_sync_import_utxos(import_target, &ext_db);
    if (import_target == &import_db)
        node_db_close(&import_db);
    coins_view_db_close(&ext_db);

    if (count < 0) {
        json_set_str(result, "Import failed");
        LOG_FAIL("blockchain", "importchainstate: UTXO import failed (count=%d)", count);
    }

    /* Fix height=0 UTXOs from transaction index (LevelDB decoder can
     * fail to read the trailing height varint for some entries). */
    {
        sqlite3_stmt *h0 = NULL;
        sqlite3_prepare_v2(ctx->node_db->db,
            "SELECT COUNT(*) FROM utxos WHERE height = 0 AND value > 0",
            -1, &h0, NULL);
        int64_t h0_count = 0;
        if (h0 && sqlite3_step(h0) == SQLITE_ROW)
            h0_count = sqlite3_column_int64(h0, 0);
        sqlite3_finalize(h0);
        if (h0_count > 0) {
            printf("importchainstate: fixing %lld UTXOs with height=0...\n",
                   (long long)h0_count);
            sqlite3_exec(ctx->node_db->db,
                "UPDATE utxos SET height = ("
                "  SELECT t.block_height FROM transactions t"
                "  WHERE t.txid = utxos.txid"
                ") WHERE height = 0 AND EXISTS ("
                "  SELECT 1 FROM transactions t"
                "  WHERE t.txid = utxos.txid AND t.block_height IS NOT NULL"
                ")", NULL, NULL, NULL);
            printf("importchainstate: fixed %d UTXO heights\n",
                   sqlite3_changes(ctx->node_db->db));
        }
    }

    /* Rebuild wallet_utxos and addresses from new UTXO set */
    sqlite3_exec(ctx->node_db->db, "BEGIN", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "DELETE FROM wallet_utxos", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "INSERT INTO wallet_utxos "
        "(txid, vout, value, address_hash, script, height, is_coinbase) "
        "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
        "u.height, u.is_coinbase "
        "FROM utxos u INNER JOIN wallet_keys wk "
        "ON u.address_hash = wk.pubkey_hash",
        NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "DELETE FROM addresses", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash IS NOT NULL "
        "GROUP BY address_hash",
        NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "COMMIT", NULL, NULL, NULL);

    /* Set coins_best_block from the LevelDB source.
     * This ensures connect_block's view/prevblock check passes
     * when resuming from the imported UTXO set. */
    {
        static const uint8_t zero_hash[32] = {0};
        if (memcmp(ldb_best_block.data, zero_hash, 32) != 0) {
            node_db_state_set(ctx->node_db, "coins_best_block",
                              ldb_best_block.data, 32);
            char hex[65];
            for (int i = 0; i < 32; i++)
                sprintf(hex + i*2, "%02x", ldb_best_block.data[i]);
            printf("importchainstate: set coins_best_block=%s\n", hex);
        }
    }

    json_set_object(result);
    json_push_kv_int(result, "utxos_imported", count);

    /* Report balance */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT COALESCE(SUM(value),0) FROM utxos",
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        json_push_kv_int(result, "total_value_zatoshi",
                          sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    s = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_utxos WHERE spent_txid IS NULL",
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        json_push_kv_int(result, "wallet_balance_zatoshi",
                          sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    printf("importchainstate: done — %d UTXOs imported\n", count);
    fflush(stdout);
    return true;
}

/* ── indexlegacy: import full chain from zclassicd LevelDB → our SQLite ── */

void rpc_blockchain_set_node_db(struct node_db *ndb)
{
    g_blockchain_ctx.node_db = ndb;
}

static bool rpc_indexlegacy(const struct json_value *params, bool help,
                             struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
        "indexlegacy ( \"legacy_datadir\" )\n"
        "\nImport the ENTIRE blockchain from a legacy zclassicd node into SQLite.\n"
        "Reads LevelDB block index, walks block files, indexes all blocks,\n"
        "transactions, and UTXOs. The legacy node should be stopped first.\n"
        "\nArguments:\n"
        "1. legacy_datadir  (string, optional, default: ~/.zclassic)\n"
        "\nThis is a heavy operation — may take 30+ minutes for 3M blocks.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    const char *legacy_dir = rpc_permit_str(&p, 0, "legacy_datadir", NULL);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("blockchain", "indexlegacy: invalid params"); }

    char default_dir[512];
    if (!legacy_dir || legacy_dir[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(default_dir, sizeof(default_dir),
                 "%s/.zclassic", home ? home : "/root");
        legacy_dir = default_dir;
    }

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "SQLite database not available");
        LOG_FAIL("blockchain", "indexlegacy: SQLite database not available");
    }

    printf("indexlegacy: scanning block files from %s/blocks/\n", legacy_dir);
    fflush(stdout);

    /* Two-pass approach:
     * Pass 1: Scan all block files, record (height, file, offset, size)
     * Pass 2: Sort by height, process in order (so spends find UTXOs)
     *
     * Supports 100+ years of blocks (~21M blocks). Uses a sparse
     * height-indexed array that grows dynamically. */

    int locs_cap = 4000000; /* initial, grows as needed */
    struct blk_loc *locs = zcl_calloc((size_t)locs_cap, sizeof(struct blk_loc), "idx_blk_locs");
    if (!locs) { json_set_str(result, "Out of memory"); LOG_FAIL("blockchain", "indexlegacy: failed to allocate locs array (%d entries)", locs_cap); }

    int max_height = -1;
    int total_found = 0;

    /* ── Turbo mode: aggressive SQLite settings for bulk import ── */
    printf("indexlegacy: Entering turbo mode (synchronous=OFF, WAL)...\n");
    fflush(stdout);
    /* Stay in WAL mode — journal_mode=OFF loses data when concurrent
     * writes happen (sync_controller commits new blocks via P2P).
     * WAL mode handles concurrent readers/writers safely. */
    sqlite3_exec(ctx->node_db->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "PRAGMA wal_autocheckpoint=10000", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "PRAGMA cache_size=-524288", NULL, NULL, NULL); /* 512MB */
    sqlite3_exec(ctx->node_db->db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "PRAGMA mmap_size=1073741824", NULL, NULL, NULL); /* 1GB */

    /* Drop all indexes before bulk insert — recreated after */
    printf("indexlegacy: Dropping indexes for fast bulk insert...\n");
    fflush(stdout);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_utxo_address", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_utxo_value", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_utxo_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_utxo_height_value", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_tx_block", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_tx_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_height_all", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_prev", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_chainwork", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_time", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_sprout_value", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_sapling_value", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_time_sprout", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_time_sapling", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_blocks_num_tx", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_txo_addr", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_txo_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_txi_prev", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_txi_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_ss_nf", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_ss_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_so_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_js_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_spnf_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_opret_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_opret_slp", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_zslp_xfer_token", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_zslp_xfer_height", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_zslp_xfer_addr", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "DROP INDEX IF EXISTS idx_zslp_ticker", NULL, NULL, NULL);

    /* Additive index: INSERT OR IGNORE for blocks/transactions (immutable chain
     * data that never changes). Phase B uses INSERT OR IGNORE for tx_outputs,
     * tx_inputs, joinsplits, etc. We DO NOT wipe — this makes indexlegacy
     * idempotent and crash-safe. Only addresses are recomputed from UTXOs. */
    printf("indexlegacy: Additive index mode (no wipe, INSERT OR IGNORE)...\n");
    fflush(stdout);
    node_db_exec(ctx->node_db, "DELETE FROM addresses");
    node_db_exec(ctx->node_db, "DELETE FROM view_integrity");

    /* ── Pass 1: Scan all blocks, build hash→(file,offset,size) map.
     * Then chain-walk from genesis using prev_hash to assign heights.
     * This works for ALL blocks including pre-BIP34 genesis era. ── */
    static const uint8_t ZCL_MAGIC[4] = {0x24, 0xe9, 0x27, 0x64};
    int64_t t_start = (int64_t)time(NULL);

    printf("indexlegacy: Pass 1 — scanning all blocks + building hash chain...\n");
    fflush(stdout);

    /* Hash→index map: store block hash, prev_hash, file pos for each block */
    struct raw_blk {
        uint8_t hash[32];
        uint8_t prev_hash[32];
        int file;
        uint32_t offset;
        uint32_t size;
    };
    int raw_cap = 4000000;
    struct raw_blk *raw = zcl_calloc((size_t)raw_cap, sizeof(struct raw_blk), "idx_raw_blks");
    if (!raw) { free(locs); json_set_str(result, "OOM"); LOG_FAIL("blockchain", "indexlegacy: failed to allocate raw block array (%d entries)", raw_cap); }
    int raw_count = 0;

    for (int file_num = 0; file_num < 1000; file_num++) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat", legacy_dir, file_num);

        struct stat st;
        if (stat(path, &st) != 0) break;

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        size_t fsize = (size_t)st.st_size;
        uint8_t *data = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (data == MAP_FAILED) continue;

        size_t pos = 0;
        while (pos + 8 < fsize) {
            if (memcmp(data + pos, ZCL_MAGIC, 4) != 0) { pos++; continue; }
            uint32_t block_size;
            memcpy(&block_size, data + pos + 4, 4);
            if (block_size < 80 || block_size > 8 * 1024 * 1024 ||
                pos + 8 + block_size > fsize) { pos++; continue; }

            /* Parse just the block header (first ~1487 bytes) to get hash + prev_hash.
             * We need to deserialize to compute the block hash (double-SHA256 of header). */
            struct block blk;
            block_init(&blk);
            struct byte_stream bs;
            stream_init_from_data(&bs, data + pos + 8, block_size);
            bool ok = block_deserialize(&blk, &bs);
            stream_free(&bs);

            if (ok) {
                if (raw_count >= raw_cap) {
                    raw_cap *= 2;
                    raw = zcl_realloc(raw, (size_t)raw_cap * sizeof(struct raw_blk), "idx_raw_blks");
                }
                struct uint256 bh;
                block_header_get_hash(&blk.header, &bh);
                memcpy(raw[raw_count].hash, bh.data, 32);
                memcpy(raw[raw_count].prev_hash, blk.header.hashPrevBlock.data, 32);
                raw[raw_count].file = file_num;
                raw[raw_count].offset = (uint32_t)(pos + 8);
                raw[raw_count].size = block_size;
                raw_count++;
            }
            block_free(&blk);
            pos += 8 + block_size;
        }
        munmap(data, fsize);

        if (file_num % 10 == 0) {
            printf("  blk%05d.dat — %d blocks total\n", file_num, raw_count);
            fflush(stdout);
        }
    }

    printf("indexlegacy: Pass 1 found %d raw blocks. Building height chain...\n",
           raw_count);
    fflush(stdout);

    /* Build hash→index lookup (simple hash table) */
    #define HASH_BUCKETS 4194304  /* 4M buckets */
    struct hash_entry { int idx; int next; };
    int *hash_heads = zcl_malloc((size_t)HASH_BUCKETS * sizeof(int), "idx_hash_heads");
    struct hash_entry *hash_nodes = zcl_malloc((size_t)raw_count * sizeof(struct hash_entry), "idx_hash_nodes");
    if (!hash_heads || !hash_nodes) {
        free(raw); free(locs); free(hash_heads); free(hash_nodes);
        json_set_str(result, "OOM"); LOG_FAIL("blockchain", "indexlegacy: failed to allocate hash table (%d buckets)", HASH_BUCKETS);
    }
    memset(hash_heads, -1, (size_t)HASH_BUCKETS * sizeof(int));

    for (int i = 0; i < raw_count; i++) {
        uint32_t bucket;
        memcpy(&bucket, raw[i].hash, 4);
        bucket %= HASH_BUCKETS;
        hash_nodes[i].idx = i;
        hash_nodes[i].next = hash_heads[bucket];
        hash_heads[bucket] = i;
    }

    /* Find genesis block (prev_hash = all zeros) */
    int genesis_idx = -1;
    uint8_t zero32[32] = {0};
    for (int i = 0; i < raw_count; i++) {
        if (memcmp(raw[i].prev_hash, zero32, 32) == 0) {
            genesis_idx = i;
            break;
        }
    }

    if (genesis_idx < 0) {
        free(raw); free(locs); free(hash_heads); free(hash_nodes);
        json_set_str(result, "Genesis block not found");
        LOG_FAIL("blockchain", "indexlegacy: genesis block not found in %d raw blocks", raw_count);
    }

    /* Walk the chain from genesis, assigning heights.
     * For each block, find the next block whose prev_hash matches our hash. */
    int *height_map = zcl_calloc((size_t)raw_count, sizeof(int), "idx_height_map");
    for (int i = 0; i < raw_count; i++) height_map[i] = -1;
    height_map[genesis_idx] = 0;

    /* Build child→parent index (reverse: for each hash, find blocks pointing to it) */
    /* Actually simpler: walk forward. Start at genesis, find who points to us. */
    /* Better approach: build prev_hash→index map, then walk from genesis forward */
    int *prev_heads = zcl_malloc((size_t)HASH_BUCKETS * sizeof(int), "idx_prev_heads");
    struct hash_entry *prev_nodes = zcl_malloc((size_t)raw_count * sizeof(struct hash_entry), "idx_prev_nodes");
    memset(prev_heads, -1, (size_t)HASH_BUCKETS * sizeof(int));
    for (int i = 0; i < raw_count; i++) {
        uint32_t bucket;
        memcpy(&bucket, raw[i].prev_hash, 4);
        bucket %= HASH_BUCKETS;
        prev_nodes[i].idx = i;
        prev_nodes[i].next = prev_heads[bucket];
        prev_heads[bucket] = i;
    }

    /* BFS from genesis: find children (blocks whose prev_hash = our hash) */
    int *queue = zcl_malloc((size_t)raw_count * sizeof(int), "idx_bfs_queue");
    int q_head = 0, q_tail = 0;
    queue[q_tail++] = genesis_idx;
    int assigned = 0;

    while (q_head < q_tail) {
        int cur = queue[q_head++];
        int cur_height = height_map[cur];

        /* Store in height→location array */
        while (cur_height >= locs_cap) {
            int new_cap = locs_cap * 2;
            struct blk_loc *tmp = zcl_realloc(locs,
                (size_t)new_cap * sizeof(struct blk_loc), "idx_blk_locs");
            if (!tmp) break;
            memset(tmp + locs_cap, 0,
                   (size_t)(new_cap - locs_cap) * sizeof(struct blk_loc));
            locs = tmp;
            locs_cap = new_cap;
        }
        if (cur_height < locs_cap) {
            locs[cur_height].file = raw[cur].file;
            locs[cur_height].offset = raw[cur].offset;
            locs[cur_height].size = raw[cur].size;
            if (cur_height > max_height) max_height = cur_height;
            total_found++;
        }
        assigned++;

        /* Find children: blocks whose prev_hash == our hash */
        uint32_t bucket;
        memcpy(&bucket, raw[cur].hash, 4);
        bucket %= HASH_BUCKETS;
        for (int e = prev_heads[bucket]; e >= 0; e = prev_nodes[e].next) {
            int child = prev_nodes[e].idx;
            if (memcmp(raw[child].prev_hash, raw[cur].hash, 32) == 0 &&
                height_map[child] < 0) {
                height_map[child] = cur_height + 1;
                queue[q_tail++] = child;
            }
        }
    }

    free(queue);
    free(prev_heads);
    free(prev_nodes);
    free(hash_heads);
    free(hash_nodes);
    free(height_map);
    free(raw);

    int64_t pass1_time = (int64_t)time(NULL) - t_start;
    printf("indexlegacy: Pass 1 complete — %d blocks chained, max height %d, "
           "%d assigned (%" PRId64 "s)\n",
           raw_count, max_height, assigned, pass1_time);
    fflush(stdout);

    if (max_height < 0) {
        free(locs);
        json_set_str(result, "No blocks found");
        LOG_FAIL("blockchain", "indexlegacy: no blocks found after chain walk");
    }

    /* ================================================================
     * Phase A: Sequential core chain data (blocks, txs, UTXOs, ZSLP)
     * Must be sequential because UTXO spends depend on height order.
     * ================================================================ */
    printf("indexlegacy: Phase A — core chain data (%d blocks)...\n",
           total_found);
    fflush(stdout);

    int64_t t_pass2 = (int64_t)time(NULL);
    int blocks_indexed = 0;
    int txs_indexed = 0;
    int utxos_created = 0;
    int utxos_spent = 0;
    int last_file = -1;
    uint8_t *mmap_data = NULL;
    size_t mmap_size = 0;
    bool phase_a_ok = true;
    bool phase_a_tx_open = false;
    const char *phase_a_error = "Phase A failed";

    if (!indexlegacy_node_tx_begin_checked(ctx->node_db, "phase A begin")) {
        free(locs);
        json_set_str(result, "Phase A failed to open transaction");
        LOG_FAIL("blockchain", "indexlegacy: phase A failed to begin transaction");
    }
    phase_a_tx_open = true;

    for (int h = 0; h <= max_height; h++) {
        if (locs[h].size == 0) continue;

        /* mmap the block file if not already mapped */
        if (locs[h].file != last_file) {
            if (mmap_data) munmap(mmap_data, mmap_size);
            char path[1200];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     legacy_dir, locs[h].file);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            mmap_size = (size_t)st.st_size;
            mmap_data = mmap(NULL, mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (mmap_data == MAP_FAILED) { mmap_data = NULL; continue; }
            last_file = locs[h].file;
        }
        if (!mmap_data) continue;
        if (locs[h].offset + locs[h].size > mmap_size) continue;

        /* Deserialize block */
        struct block blk;
        block_init(&blk);
        struct byte_stream bs;
        stream_init_from_data(&bs, mmap_data + locs[h].offset, locs[h].size);
        if (!block_deserialize(&blk, &bs)) {
            stream_free(&bs);
            block_free(&blk);
            continue;
        }
        stream_free(&bs);

        /* Compute block hash */
        struct uint256 block_hash;
        block_header_get_hash(&blk.header, &block_hash);

        /* Index block */
        struct db_block db_blk;
        memset(&db_blk, 0, sizeof(db_blk));
        memcpy(db_blk.hash, block_hash.data, 32);
        db_blk.height = h;
        memcpy(db_blk.merkle_root, blk.header.hashMerkleRoot.data, 32);
        memcpy(db_blk.sapling_root, blk.header.hashFinalSaplingRoot.data, 32);
        memcpy(db_blk.nonce, blk.header.nNonce.data, 32);
        memcpy(db_blk.prev_hash, blk.header.hashPrevBlock.data, 32);
        db_blk.version = blk.header.nVersion;
        db_blk.time = blk.header.nTime;
        db_blk.bits = blk.header.nBits;
        db_blk.num_tx = (int)blk.num_vtx;
        db_blk.file_num = locs[h].file;
        db_blk.data_pos = (int)locs[h].offset;
        db_blk.solution = blk.header.nSolution;
        db_blk.solution_len = blk.header.nSolutionSize;
        db_blk.status = 29; /* BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA */

        if (!db_block_save(ctx->node_db, &db_blk)) {
            phase_a_error = "Phase A block save failed";
            block_free(&blk);
            phase_a_ok = false;
            break;
        }
        blocks_indexed++;

        /* Index transactions + UTXOs */
        for (size_t i = 0; i < blk.num_vtx; i++) {
            const struct transaction *tx = &blk.vtx[i];

            struct db_tx_index db_tx;
            memset(&db_tx, 0, sizeof(db_tx));
            memcpy(db_tx.txid, tx->hash.data, 32);
            memcpy(db_tx.block_hash, block_hash.data, 32);
            db_tx.block_height = h;
            db_tx.tx_index = (int)i;
            db_tx.file_num = locs[h].file;
            db_tx.file_pos = (int)locs[h].offset;
            db_tx.is_coinbase = (i == 0);
            if (!db_tx_save(ctx->node_db, &db_tx)) {
                phase_a_error = "Phase A transaction index save failed";
                block_free(&blk);
                phase_a_ok = false;
                break;
            }
            txs_indexed++;

            /* Count spent inputs (but don't modify utxos table —
             * that's the canonical UTXO store managed by coins_view_sqlite) */
            if (i > 0) {
                for (size_t j = 0; j < tx->num_vin; j++) {
                    /* db_utxo_delete removed: utxos table is canonical */
                    utxos_spent++;
                }
            }

            /* Index OP_RETURN outputs for ZSLP token tracking (rare) */
            if (tx->num_vout > 0 &&
                tx->vout[0].script_pub_key.size > 0 &&
                tx->vout[0].script_pub_key.data[0] == 0x6a) {
                const uint8_t *script = tx->vout[0].script_pub_key.data;
                size_t script_len = tx->vout[0].script_pub_key.size;
                struct slp_message slp;
                bool is_slp = slp_parse(script, script_len, &slp);

                if (is_slp) {
                    uint8_t tok_id[32];
                    if (slp.type == SLP_TX_GENESIS) {
                        memcpy(tok_id, tx->hash.data, 32);
                    } else {
                        for (int b = 0; b < 32; b++)
                            tok_id[b] = slp.token_id.data[31 - b];
                    }

                    if (slp.type == SLP_TX_GENESIS &&
                        !db_zslp_token_save(ctx->node_db, tok_id,
                            slp.ticker, slp.name, slp.decimals,
                            slp.document_url, h,
                            (int64_t)slp.initial_quantity)) {
                        phase_a_error = "Phase A ZSLP token save failed";
                        block_free(&blk);
                        phase_a_ok = false;
                        break;
                    }

                    int num_outputs_slp = (slp.type == SLP_TX_SEND)
                        ? slp.num_outputs : 1;
                    if (num_outputs_slp < 1) num_outputs_slp = 1;

                    for (int q = 0; q < num_outputs_slp; q++) {
                        int64_t amount = 0;
                        if (slp.type == SLP_TX_GENESIS)
                            amount = (int64_t)slp.initial_quantity;
                        else if (slp.type == SLP_TX_MINT)
                            amount = (int64_t)slp.additional_quantity;
                        else if (q < slp.num_outputs)
                            amount = (int64_t)slp.output_quantities[q];

                        uint8_t to_addr[20];
                        const uint8_t *to = NULL;
                        int out_idx = (slp.type == SLP_TX_GENESIS) ? 1 : q + 1;
                        if (out_idx < (int)tx->num_vout) {
                            const uint8_t *sd2 = tx->vout[out_idx].script_pub_key.data;
                            size_t sl2 = tx->vout[out_idx].script_pub_key.size;
                            if (sl2 == 25 && sd2[0] == 0x76 && sd2[1] == 0xa9 &&
                                sd2[2] == 0x14 && sd2[23] == 0x88 && sd2[24] == 0xac) {
                                memcpy(to_addr, sd2 + 3, 20);
                                to = to_addr;
                            }
                        }

                        if (!db_zslp_transfer_save(ctx->node_db, tx->hash.data,
                                h, tok_id, (int)slp.type, amount, q, to)) {
                            phase_a_error = "Phase A ZSLP transfer save failed";
                            block_free(&blk);
                            phase_a_ok = false;
                            break;
                        }
                    }
                }

                /* ZNAM: index name registrations from OP_RETURN */
                struct znam_message znam;
                if (znam_parse(script, script_len, &znam)) {
                    struct znam_entry entry;
                    memset(&entry, 0, sizeof(entry));
                    snprintf(entry.name, sizeof(entry.name), "%s", znam.name);
                    entry.target_type = znam.target_type;
                    snprintf(entry.target_value, sizeof(entry.target_value),
                             "%s", znam.target_value);
                    memcpy(entry.reg_txid, tx->hash.data, 32);
                    entry.reg_height = h;
                    memcpy(entry.last_update_txid, tx->hash.data, 32);

                    /* Owner = P2PKH address from vout[1] (change output) */
                    if (tx->num_vout > 1) {
                        const uint8_t *sd2 = tx->vout[1].script_pub_key.data;
                        size_t sl2 = tx->vout[1].script_pub_key.size;
                        if (sl2 == 25 && sd2[0] == 0x76 && sd2[1] == 0xa9 &&
                            sd2[2] == 0x14 && sd2[23] == 0x88 && sd2[24] == 0xac) {
                            /* Base58Check encode the pubkey hash */
                            char addr[64];
                            uint8_t payload[22];
                            payload[0] = 0x1c; payload[1] = 0xb8; /* ZClassic t1 prefix */
                            memcpy(payload + 2, sd2 + 3, 20);
                            size_t addr_len = 0;
                            base58check_encode(payload, 22, addr, sizeof(addr), &addr_len);
                            snprintf(entry.owner_address, sizeof(entry.owner_address),
                                     "%s", addr);
                        }
                    }

                    if (znam.command == ZNAM_CMD_REGISTER) {
                        /* Only register if name doesn't exist yet */
                        struct znam_entry existing;
                        if (!db_znam_find(ctx->node_db, znam.name, &existing)) {
                            db_znam_save(ctx->node_db, &entry);
                            printf("znam: registered '%s' -> %s (height %d)\n",
                                   entry.name, entry.target_value, h);
                        }
                    } else if (znam.command == ZNAM_CMD_UPDATE) {
                        struct znam_entry existing;
                        if (db_znam_find(ctx->node_db, znam.name, &existing) &&
                            strcmp(existing.owner_address, entry.owner_address) == 0) {
                            existing.target_type = znam.target_type;
                            snprintf(existing.target_value, sizeof(existing.target_value),
                                     "%s", znam.target_value);
                            memcpy(existing.last_update_txid, tx->hash.data, 32);
                            db_znam_save(ctx->node_db, &existing);
                        }
                    } else if (znam.command == ZNAM_CMD_TRANSFER) {
                        struct znam_entry existing;
                        if (db_znam_find(ctx->node_db, znam.name, &existing) &&
                            strcmp(existing.owner_address, entry.owner_address) == 0) {
                            snprintf(existing.owner_address, sizeof(existing.owner_address),
                                     "%s", znam.new_owner);
                            memcpy(existing.last_update_txid, tx->hash.data, 32);
                            db_znam_save(ctx->node_db, &existing);
                        }
                    } else if (znam.command == ZNAM_CMD_RENEW) {
                        struct znam_entry existing;
                        if (db_znam_find(ctx->node_db, znam.name, &existing) &&
                            strcmp(existing.owner_address, entry.owner_address) == 0) {
                            memcpy(existing.last_update_txid, tx->hash.data, 32);
                            db_znam_save(ctx->node_db, &existing);
                        }
                    } else if (znam.command == ZNAM_CMD_SET_RECORD) {
                        struct znam_entry existing;
                        if (db_znam_find(ctx->node_db, znam.name, &existing) &&
                            strcmp(existing.owner_address, entry.owner_address) == 0) {
                            db_znam_addr_save(ctx->node_db, znam.name,
                                              znam.target_type, znam.target_value);
                        }
                    } else if (znam.command == ZNAM_CMD_SET_TEXT) {
                        struct znam_entry existing;
                        if (db_znam_find(ctx->node_db, znam.name, &existing) &&
                            strcmp(existing.owner_address, entry.owner_address) == 0) {
                            db_znam_text_save(ctx->node_db, znam.name,
                                              znam.text_key, znam.text_value);
                        }
                    }
                }
            }

            /* Create output UTXOs */
            for (size_t j = 0; j < tx->num_vout; j++) {
                if (tx->vout[j].value == 0 &&
                    tx->vout[j].script_pub_key.size > 0 &&
                    tx->vout[j].script_pub_key.data[0] == 0x6a)
                    continue;

                struct db_utxo u;
                memset(&u, 0, sizeof(u));
                memcpy(u.txid, tx->hash.data, 32);
                u.vout = (uint32_t)j;
                u.value = tx->vout[j].value;
                u.script = (uint8_t *)tx->vout[j].script_pub_key.data;
                u.script_len = tx->vout[j].script_pub_key.size;
                u.height = h;
                u.is_coinbase = (i == 0);

                const uint8_t *sd = tx->vout[j].script_pub_key.data;
                size_t sl = tx->vout[j].script_pub_key.size;
                if (sl == 25 && sd[0] == 0x76 && sd[1] == 0xa9 &&
                    sd[2] == 0x14 && sd[23] == 0x88 && sd[24] == 0xac) {
                    memcpy(u.address_hash, sd + 3, 20);
                    u.has_address = true;
                    u.script_type = SCRIPT_P2PKH;
                } else if (sl == 23 && sd[0] == 0xa9 && sd[1] == 0x14 &&
                           sd[22] == 0x87) {
                    memcpy(u.address_hash, sd + 2, 20);
                    u.has_address = true;
                    u.script_type = SCRIPT_P2SH;
                }

                /* db_utxo_save removed: utxos table is canonical */
                utxos_created++;
            }

            if (!phase_a_ok)
                break;
        }

        if (!phase_a_ok)
            break;

        block_free(&blk);

        if (blocks_indexed % 100000 == 0 && blocks_indexed > 0) {
            if (!indexlegacy_node_tx_commit_checked(ctx->node_db,
                    "phase A batch commit")) {
                phase_a_error = "Phase A batch commit failed";
                phase_a_ok = false;
                phase_a_tx_open = false;
                break;
            }
            phase_a_tx_open = false;
            int64_t elapsed = (int64_t)time(NULL) - t_pass2;
            double rate = elapsed > 0 ?
                (double)blocks_indexed / (double)elapsed : 0;
            int remaining = total_found - blocks_indexed;
            int eta = rate > 0 ? (int)((double)remaining / rate) : 0;
            printf("  Phase A: height %d/%d — %d txs, %d utxos (+%d -%d) "
                   "(%.0f blk/s, ETA %dm%ds)\n",
                   h, max_height, txs_indexed, utxos_created - utxos_spent,
                   utxos_created, utxos_spent,
                   rate, eta / 60, eta % 60);
            fflush(stdout);
            if (!indexlegacy_node_tx_begin_checked(ctx->node_db,
                    "phase A batch reopen")) {
                phase_a_error = "Phase A failed to reopen transaction";
                phase_a_ok = false;
                break;
            }
            phase_a_tx_open = true;
        }
    }

    if (mmap_data) munmap(mmap_data, mmap_size);
    if (!phase_a_ok) {
        if (phase_a_tx_open &&
            !indexlegacy_node_tx_rollback_checked(ctx->node_db,
                "phase A rollback")) {
            fprintf(stderr,
                    "indexlegacy: phase A rollback failed after error\n");
        }
        free(locs);
        json_set_str(result, phase_a_error);
        LOG_FAIL("blockchain", "indexlegacy: %s", phase_a_error);
    }
    if (phase_a_tx_open &&
        !indexlegacy_node_tx_commit_checked(ctx->node_db,
            "phase A final commit")) {
        free(locs);
        json_set_str(result, "Phase A final commit failed");
        LOG_FAIL("blockchain", "indexlegacy: phase A final commit failed");
    }
    phase_a_tx_open = false;

    int64_t phase_a_time = (int64_t)time(NULL) - t_pass2;
    printf("indexlegacy: Phase A complete — %d blocks, %d txs, %d net UTXOs "
           "in %" PRId64 "s\n",
           blocks_indexed, txs_indexed, utxos_created - utxos_spent,
           phase_a_time);
    fflush(stdout);

    /* ================================================================
     * Phase B: Parallel extraction of detailed chain data
     * Worker threads re-read blocks and extract tx_inputs, tx_outputs,
     * joinsplits, sapling_spends, sapling_outputs, sprout_nullifiers,
     * op_returns, and per-block shielded values into memory arrays.
     * Then the main thread writes everything sequentially to SQLite.
     * ================================================================ */

    printf("indexlegacy: Phase B — parallel extraction with %d threads...\n",
           N_INDEX_THREADS);
    fflush(stdout);
    int64_t t_phase_b = (int64_t)time(NULL);

    /* Divide height range among threads */
    struct worker_ctx workers[N_INDEX_THREADS];
    pthread_t threads[N_INDEX_THREADS];
    int heights_per_thread = (max_height + 1 + N_INDEX_THREADS - 1) / N_INDEX_THREADS;
    int workers_started = 0;

    for (int t = 0; t < N_INDEX_THREADS; t++) {
        memset(&workers[t], 0, sizeof(workers[t]));
        workers[t].thread_id = t;
        workers[t].height_from = t * heights_per_thread;
        workers[t].height_to = (t + 1) * heights_per_thread - 1;
        if (workers[t].height_to > max_height)
            workers[t].height_to = max_height;
        workers[t].locs = locs;
        workers[t].max_height = max_height;
        workers[t].legacy_dir = legacy_dir;
        if (pthread_create(&threads[t], NULL, index_worker, &workers[t]) != 0) {
            fprintf(stderr,
                    "indexlegacy: Phase B failed to start extraction worker %d\n",
                    t);
            for (int j = 0; j < workers_started; j++)
                pthread_join(threads[j], NULL);
            free(locs);
            json_set_str(result,
                         "Failed to start Phase B extraction workers");
            LOG_FAIL("blockchain", "indexlegacy: failed to start Phase B worker thread %d", t);
        }
        workers_started++;
    }

    for (int t = 0; t < N_INDEX_THREADS; t++)
        pthread_join(threads[t], NULL);

    int64_t extract_time = (int64_t)time(NULL) - t_phase_b;
    printf("indexlegacy: Phase B extraction complete in %" PRId64 "s\n",
           extract_time);
    fflush(stdout);

    /* ── Phase B write: sequential INSERT into SQLite ── */
    printf("indexlegacy: Phase B — writing extracted data to SQLite...\n");
    fflush(stdout);
    int64_t t_write = (int64_t)time(NULL);

    int64_t joinsplits_indexed = 0, sapling_spends_indexed = 0;
    int64_t sapling_outputs_indexed = 0, sprout_nullifiers_indexed = 0;
    int64_t op_returns_indexed = 0;
    int64_t total_inputs = 0, total_outputs = 0;
    int64_t batch_rows = 0;
    struct idx_block_shielded *all_bsh = NULL;
    

    /* Phase B MUST use a separate sqlite3 connection. The main handle
     * has sync_controller's batch transaction which gets rolled back
     * on any block validation error — destroying all Phase B inserts.
     * WAL mode allows concurrent readers but only ONE writer. We set
     * a 60-second busy timeout so Phase B waits for the write lock. */
    const char *phase_b_path = sqlite3_db_filename(ctx->node_db->db, "main");
    sqlite3 *phase_b_db = NULL;
    bool phase_b_db_opened = false;
    bool phase_b_ok = true;
    bool phase_b_own_txn = false;
    if (sqlite3_open(phase_b_path, &phase_b_db) != SQLITE_OK || !phase_b_db) {
        fprintf(stderr, "indexlegacy: Phase B FATAL: cannot open DB: %s\n",
                phase_b_db ? sqlite3_errmsg(phase_b_db) : "null");
        phase_b_ok = false;
        phase_b_db = NULL;
        goto phase_b_cleanup;
    }
    phase_b_db_opened = true;
    if (!indexlegacy_exec_checked(phase_b_db, "PRAGMA journal_mode=WAL",
                                 "phase B pragma journal_mode=WAL") ||
        !indexlegacy_exec_checked(phase_b_db, "PRAGMA synchronous=OFF",
                                 "phase B pragma synchronous") ||
        !indexlegacy_exec_checked(phase_b_db, "PRAGMA wal_autocheckpoint=10000",
                                 "phase B pragma wal_autocheckpoint") ||
        !indexlegacy_exec_checked(phase_b_db, "PRAGMA cache_size=-524288",
                                 "phase B pragma cache_size") ||
        !indexlegacy_exec_checked(phase_b_db, "PRAGMA temp_store=MEMORY",
                                 "phase B pragma temp_store") ||
        !indexlegacy_exec_checked(phase_b_db, "PRAGMA mmap_size=1073741824",
                                 "phase B pragma mmap_size")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    sqlite3_busy_timeout(phase_b_db, 60000); /* 60s wait for write lock */
    if (!indexlegacy_exec_checked(phase_b_db, "BEGIN IMMEDIATE",
                                 "phase B begin transaction")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    phase_b_own_txn = true;

    sqlite3_stmt *stmt_txo = NULL, *stmt_txi = NULL, *stmt_js = NULL;
    sqlite3_stmt *stmt_ss = NULL, *stmt_so = NULL, *stmt_spnf = NULL;
    sqlite3_stmt *stmt_opret = NULL, *stmt_integrity = NULL;
    sqlite3_stmt *stmt_update_shielded = NULL;

    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO tx_outputs"
            "(txid,vout,value,script_type,address_hash,block_height)"
            " VALUES(?,?,?,?,?,?)",
            &stmt_txo, "phase B prepare tx_outputs")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO tx_inputs"
            "(txid,vin_index,prev_txid,prev_vout,block_height)"
            " VALUES(?,?,?,?,?)",
            &stmt_txi, "phase B prepare tx_inputs")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO joinsplits"
            "(txid,js_index,vpub_old,vpub_new,anchor,block_height)"
            " VALUES(?,?,?,?,?,?)",
            &stmt_js, "phase B prepare joinsplits")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO sapling_spends"
            "(txid,spend_index,cv,anchor,nullifier,rk,block_height)"
            " VALUES(?,?,?,?,?,?,?)",
            &stmt_ss, "phase B prepare sapling_spends")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO sapling_outputs"
            "(txid,output_index,cv,cm,ephemeral_key,block_height)"
            " VALUES(?,?,?,?,?,?)",
            &stmt_so, "phase B prepare sapling_outputs")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO sprout_nullifiers"
            "(nullifier,txid,block_height)"
            " VALUES(?,?,?)",
            &stmt_spnf, "phase B prepare sprout_nullifiers")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR IGNORE INTO op_returns"
            "(txid,block_height,script,is_slp)"
            " VALUES(?,?,?,?)",
            &stmt_opret, "phase B prepare op_returns")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "INSERT OR REPLACE INTO view_integrity"
            "(height,sha3_hash) VALUES(?,?)",
            &stmt_integrity, "phase B prepare view_integrity")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    if (!indexlegacy_prepare_checked(
            phase_b_db,
            "UPDATE blocks SET sprout_value=?,sapling_value=?"
            " WHERE height=?",
            &stmt_update_shielded, "phase B prepare update shielded")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }

    /* Phase B BEGIN on separate connection */

    /* Write tx_inputs from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_inputs; k++) {
            struct idx_tx_input *inp = &workers[t].inputs[k];
            sqlite3_reset(stmt_txi);
            if (sqlite3_bind_blob(stmt_txi, 1, inp->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_txi, 2, (int)inp->vin_index) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_txi, 3, inp->prev_txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_txi, 4, (int)inp->prev_vout) != SQLITE_OK ||
                sqlite3_bind_int(stmt_txi, 5, inp->height) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_txi, phase_b_db, "phase B insert tx_inputs")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
            sqlite3_reset(stmt_txi);
            total_inputs++;
            if (++batch_rows % 500000 == 0) {
                if (phase_b_own_txn) {
                    if (!indexlegacy_exec_checked(phase_b_db, "COMMIT",
                        "phase B commit batch")) {
                        phase_b_ok = false;
                        goto phase_b_cleanup;
                    }
                    if (!indexlegacy_exec_checked(phase_b_db, "BEGIN",
                        "phase B begin batch")) {
                        phase_b_ok = false;
                        goto phase_b_cleanup;
                    }
                }
                printf("  Phase B write: %lld rows...\n", (long long)batch_rows);
                fflush(stdout);
            }
        }
    }

    /* Write tx_outputs from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_outputs; k++) {
            struct idx_tx_output *ot = &workers[t].outputs[k];
            sqlite3_reset(stmt_txo);
            if (sqlite3_bind_blob(stmt_txo, 1, ot->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_txo, 2, (int)ot->vout) != SQLITE_OK ||
                sqlite3_bind_int64(stmt_txo, 3, ot->value) != SQLITE_OK ||
                sqlite3_bind_int(stmt_txo, 4, ot->script_type) != SQLITE_OK ||
                (ot->has_addr ?
                sqlite3_bind_blob(stmt_txo, 5, ot->addr_hash, 20, SQLITE_STATIC) :
                 sqlite3_bind_null(stmt_txo, 5)) != SQLITE_OK ||
                sqlite3_bind_int(stmt_txo, 6, ot->height) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_txo, phase_b_db, "phase B insert tx_outputs")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
            sqlite3_reset(stmt_txo);
            total_outputs++;
        }
    }

    /* Write joinsplits + sprout nullifiers from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_joinsplits; k++) {
            struct idx_joinsplit *ij = &workers[t].joinsplits[k];
            sqlite3_reset(stmt_js);
            if (sqlite3_bind_blob(stmt_js, 1, ij->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_js, 2, (int)ij->js_index) != SQLITE_OK ||
                sqlite3_bind_int64(stmt_js, 3, ij->vpub_old) != SQLITE_OK ||
                sqlite3_bind_int64(stmt_js, 4, ij->vpub_new) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_js, 5, ij->anchor, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_js, 6, ij->height) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_js, phase_b_db, "phase B insert joinsplits")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
            joinsplits_indexed++;

            /* Write both sprout nullifiers */
            for (int nf = 0; nf < 2; nf++) {
                sqlite3_reset(stmt_spnf);
                if (sqlite3_bind_blob(stmt_spnf, 1, ij->nullifiers[nf], 32, SQLITE_STATIC) != SQLITE_OK ||
                    sqlite3_bind_blob(stmt_spnf, 2, ij->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                    sqlite3_bind_int(stmt_spnf, 3, ij->height) != SQLITE_OK ||
                    !indexlegacy_step_checked(stmt_spnf, phase_b_db,
                        "phase B insert sprout nullifier")) {
                    phase_b_ok = false;
                    goto phase_b_cleanup;
                }
                sprout_nullifiers_indexed++;
            }
        }
    }

    /* Write sapling spends from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_sspends; k++) {
            struct idx_sapling_spend *is2 = &workers[t].sspends[k];
            sqlite3_reset(stmt_ss);
            if (sqlite3_bind_blob(stmt_ss, 1, is2->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_ss, 2, (int)is2->spend_index) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_ss, 3, is2->cv, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_ss, 4, is2->anchor, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_ss, 5, is2->nullifier, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_ss, 6, is2->rk, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_ss, 7, is2->height) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_ss, phase_b_db,
                    "phase B insert sapling_spend")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
            sapling_spends_indexed++;
        }
    }

    /* Write sapling outputs from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_soutputs; k++) {
            struct idx_sapling_output *io = &workers[t].soutputs[k];
            sqlite3_reset(stmt_so);
            if (sqlite3_bind_blob(stmt_so, 1, io->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_so, 2, (int)io->output_index) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_so, 3, io->cv, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_so, 4, io->cm, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_so, 5, io->ephemeral_key, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_so, 6, io->height) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_so, phase_b_db,
                    "phase B insert sapling_output")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
            sapling_outputs_indexed++;
        }
    }

    /* Write op_returns from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_oprets; k++) {
            struct idx_opret *op = &workers[t].oprets[k];
            sqlite3_reset(stmt_opret);
            if (sqlite3_bind_blob(stmt_opret, 1, op->txid, 32, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_opret, 2, op->height) != SQLITE_OK ||
                sqlite3_bind_blob(stmt_opret, 3, op->script, (int)op->script_len,
                                 SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_int(stmt_opret, 4, op->is_slp) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_opret, phase_b_db,
                    "phase B insert op_return")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
            op_returns_indexed++;
        }
    }

    /* Collect all block_shielded entries, sort by height for SHA3 chain */
    int total_bsh = 0;
    for (int t = 0; t < N_INDEX_THREADS; t++)
        total_bsh += workers[t].num_blocks_sh;

    all_bsh = zcl_malloc((size_t)total_bsh * sizeof(*all_bsh), "idx_all_bsh");
    if (!all_bsh && total_bsh > 0) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }
    int bsh_idx = 0;
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        if (workers[t].num_blocks_sh > 0) {
            memcpy(all_bsh + bsh_idx, workers[t].blocks_sh,
                   (size_t)workers[t].num_blocks_sh * sizeof(*all_bsh));
            bsh_idx += workers[t].num_blocks_sh;
        }
    }

    /* Sort by height (threads are already sorted within, but merge all) */
    /* Simple: threads process contiguous ranges so concatenation is sorted.
     * But verify by sorting anyway for safety. */
    for (int a = 1; a < total_bsh; a++) {
        if (all_bsh[a].height < all_bsh[a - 1].height) {
            /* Need to sort — use insertion sort on nearly-sorted data */
            for (int b = a; b > 0 && all_bsh[b].height < all_bsh[b-1].height; b--) {
                struct idx_block_shielded tmp_bsh = all_bsh[b];
                all_bsh[b] = all_bsh[b-1];
                all_bsh[b-1] = tmp_bsh;
            }
        }
    }

    /* Update sprout_value/sapling_value + compute SHA3 hash chain */
    uint8_t sha3_prev[32];
    memset(sha3_prev, 0, 32);

    for (int k = 0; k < total_bsh; k++) {
        struct idx_block_shielded *b = &all_bsh[k];

        if (b->sprout_value != 0 || b->sapling_value != 0) {
            sqlite3_reset(stmt_update_shielded);
            if (sqlite3_bind_int64(stmt_update_shielded, 1, b->sprout_value) != SQLITE_OK ||
                sqlite3_bind_int64(stmt_update_shielded, 2, b->sapling_value) != SQLITE_OK ||
                sqlite3_bind_int(stmt_update_shielded, 3, b->height) != SQLITE_OK ||
                !indexlegacy_step_checked(stmt_update_shielded, phase_b_db,
                    "phase B update shielded values")) {
                phase_b_ok = false;
                goto phase_b_cleanup;
            }
        }

        /* SHA3-256 integrity hash chain */
        struct sha3_256_ctx sha3;
        sha3_256_init(&sha3);
        sha3_256_write(&sha3, sha3_prev, 32);

        uint32_t h_le = (uint32_t)b->height;
        sha3_256_write(&sha3, (const unsigned char *)&h_le, 4);
        sha3_256_write(&sha3, b->block_hash, 32);

        int64_t sv_le = b->sprout_value;
        sha3_256_write(&sha3, (const unsigned char *)&sv_le, 8);
        int64_t sapv_le = b->sapling_value;
        sha3_256_write(&sha3, (const unsigned char *)&sapv_le, 8);

        sha3_256_write(&sha3, (const unsigned char *)&b->num_tx, 4);
        sha3_256_write(&sha3, (const unsigned char *)&b->num_js, 4);
        sha3_256_write(&sha3, (const unsigned char *)&b->num_ss, 4);
        sha3_256_write(&sha3, (const unsigned char *)&b->num_so, 4);

        uint8_t sha3_out[32];
        sha3_256_finalize(&sha3, sha3_out);
        memcpy(sha3_prev, sha3_out, 32);

        sqlite3_reset(stmt_integrity);
        if (sqlite3_bind_int(stmt_integrity, 1, b->height) != SQLITE_OK ||
            sqlite3_bind_blob(stmt_integrity, 2, sha3_out, 32, SQLITE_STATIC) != SQLITE_OK) {
            phase_b_ok = false;
            goto phase_b_cleanup;
        }
        if (!indexlegacy_step_checked(stmt_integrity, phase_b_db,
                "phase B insert integrity")) {
            phase_b_ok = false;
            goto phase_b_cleanup;
        }
    }

    if (phase_b_own_txn &&
        !indexlegacy_exec_checked(phase_b_db, "COMMIT", "phase B commit")) {
        phase_b_ok = false;
        goto phase_b_cleanup;
    }

phase_b_cleanup:
    if (!phase_b_ok) {
        json_set_str(result, "Phase B failed and rolled back");
        if (phase_b_own_txn &&
            phase_b_db &&
            !sqlite3_get_autocommit(phase_b_db) &&
            !indexlegacy_exec_checked(phase_b_db, "ROLLBACK", "phase B rollback")) {
            /* rollback failure is already logged by helper */
        }
    }
    if (phase_b_db_opened && phase_b_db &&
        !sqlite3_get_autocommit(phase_b_db) &&
        !indexlegacy_exec_checked(phase_b_db, "ROLLBACK", "phase B pre-close rollback")) {
        /* rollback failure is already logged by helper */
    }

    /* Free all thread buffers */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        free(workers[t].inputs);
        free(workers[t].outputs);
        free(workers[t].joinsplits);
        free(workers[t].sspends);
        free(workers[t].soutputs);
        free(workers[t].oprets);
        free(workers[t].blocks_sh);
    }
    free(all_bsh);

    sqlite3_finalize(stmt_txo);
    sqlite3_finalize(stmt_txi);
    sqlite3_finalize(stmt_js);
    sqlite3_finalize(stmt_ss);
    sqlite3_finalize(stmt_so);
    sqlite3_finalize(stmt_spnf);
    sqlite3_finalize(stmt_opret);
    sqlite3_finalize(stmt_integrity);
    sqlite3_finalize(stmt_update_shielded);
    if (phase_b_db_opened && phase_b_db) {
        sqlite3_close(phase_b_db);
        phase_b_db = NULL;
    }
    if (!phase_b_ok) {
        LOG_FAIL("blockchain", "indexlegacy: Phase B failed and rolled back");
    }

    int64_t write_time = (int64_t)time(NULL) - t_write;
    printf("indexlegacy: Phase B wrote %" PRId64 " inputs, %" PRId64 " outputs, "
           "%" PRId64 " joinsplits, %" PRId64 " sspends, %" PRId64 " soutputs, "
           "%" PRId64 " spnf, %" PRId64 " oprets in %" PRId64 "s\n",
           total_inputs, total_outputs, joinsplits_indexed,
           sapling_spends_indexed, sapling_outputs_indexed,
           sprout_nullifiers_indexed, op_returns_indexed, write_time);
    fflush(stdout);

    /* ── Rebuild all indexes (dropped before bulk insert for speed) ── */
    printf("indexlegacy: Rebuilding indexes...\n");
    fflush(stdout);
    int64_t t_idx = (int64_t)time(NULL);
    /* Core block/tx/utxo indexes */
    sqlite3_exec(ctx->node_db->db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_blocks_height ON blocks(height) WHERE status >= 3", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_prev ON blocks(prev_hash)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_chainwork ON blocks(chain_work DESC)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_height_all ON blocks(height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_time ON blocks(time)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_sprout_value ON blocks(sprout_value) WHERE sprout_value != 0", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_sapling_value ON blocks(sapling_value) WHERE sapling_value != 0", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_time_sprout ON blocks(time, sprout_value) WHERE sprout_value != 0", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_time_sapling ON blocks(time, sapling_value) WHERE sapling_value != 0", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_blocks_num_tx ON blocks(num_tx DESC)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_tx_block ON transactions(block_hash)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_tx_height ON transactions(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_utxo_address ON utxos(address_hash) WHERE address_hash IS NOT NULL", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_utxo_value ON utxos(value DESC)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_utxo_height ON utxos(height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_utxo_height_value ON utxos(height, value)", NULL, NULL, NULL);
    /* New chain index table indexes */
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_txo_addr ON tx_outputs(address_hash) WHERE address_hash IS NOT NULL", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_txo_height ON tx_outputs(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_txi_prev ON tx_inputs(prev_txid, prev_vout)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_txi_height ON tx_inputs(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_ss_nf ON sapling_spends(nullifier)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_ss_height ON sapling_spends(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_so_height ON sapling_outputs(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_js_height ON joinsplits(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_spnf_height ON sprout_nullifiers(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_opret_height ON op_returns(block_height)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_opret_slp ON op_returns(is_slp) WHERE is_slp = 1", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_token ON zslp_transfers(token_id)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_height ON zslp_transfers(block_height DESC)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_addr ON zslp_transfers(to_addr) WHERE to_addr IS NOT NULL", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_zslp_ticker ON zslp_tokens(ticker)", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "CREATE INDEX IF NOT EXISTS idx_addr_balance ON addresses(balance DESC)", NULL, NULL, NULL);
    printf("indexlegacy: Indexes rebuilt in %llds\n",
        (long long)((int64_t)time(NULL) - t_idx));
    fflush(stdout);

    /* Restore safe SQLite settings */
    sqlite3_exec(ctx->node_db->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
    sqlite3_wal_checkpoint_v2(ctx->node_db->db, NULL,
        SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);

    /* ── Populate addresses from UTXO set ── */
    printf("indexlegacy: Populating addresses from UTXO set...\n");
    fflush(stdout);
    node_db_exec(ctx->node_db, "DELETE FROM addresses");
    sqlite3_exec(ctx->node_db->db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash IS NOT NULL "
        "GROUP BY address_hash",
        NULL, NULL, NULL);
    int64_t addr_count = 0;
    {
        sqlite3_stmt *chk = NULL;
        if (sqlite3_prepare_v2(ctx->node_db->db,
                "SELECT count(*) FROM addresses", -1, &chk, NULL) == SQLITE_OK && chk) {
            if (sqlite3_step(chk) == SQLITE_ROW)
                addr_count = sqlite3_column_int64(chk, 0);
            sqlite3_finalize(chk);
        }
    }
    printf("indexlegacy: Populated %" PRId64 " addresses\n", addr_count);
    fflush(stdout);

    /* Update tip */
    {
        struct db_block tip_blk;
        if (db_block_find_by_height(ctx->node_db, max_height, &tip_blk))
            node_db_sync_set_tip(ctx->node_db, tip_blk.hash, max_height);
    }

    int64_t total_elapsed = (int64_t)time(NULL) - t_start;
    int net_utxos = utxos_created - utxos_spent;
    printf("indexlegacy: COMPLETE — %d blocks, %d txs, %d net UTXOs "
           "(+%d -%d), %" PRId64 " js, %" PRId64 " sspend, %" PRId64 " sout, "
           "%" PRId64 " spnf, %" PRId64 " opret, %" PRId64 " addrs "
           "in %" PRId64 "m%" PRId64 "s\n",
           blocks_indexed, txs_indexed, net_utxos,
           utxos_created, utxos_spent,
           joinsplits_indexed, sapling_spends_indexed,
           sapling_outputs_indexed, sprout_nullifiers_indexed,
           op_returns_indexed, addr_count,
           total_elapsed / 60, total_elapsed % 60);
    fflush(stdout);

    free(locs);

    json_set_object(result);
    json_push_kv_int(result, "blocks_indexed", blocks_indexed);
    json_push_kv_int(result, "transactions_indexed", txs_indexed);
    json_push_kv_int(result, "utxos_net", net_utxos);
    json_push_kv_int(result, "utxos_created", utxos_created);
    json_push_kv_int(result, "utxos_spent", utxos_spent);
    json_push_kv_int(result, "joinsplits_indexed", (int)joinsplits_indexed);
    json_push_kv_int(result, "sapling_spends_indexed", (int)sapling_spends_indexed);
    json_push_kv_int(result, "sapling_outputs_indexed", (int)sapling_outputs_indexed);
    json_push_kv_int(result, "sprout_nullifiers_indexed", (int)sprout_nullifiers_indexed);
    json_push_kv_int(result, "op_returns_indexed", (int)op_returns_indexed);
    json_push_kv_int(result, "addresses_populated", (int)addr_count);
    json_push_kv_int(result, "max_height", max_height);
    json_push_kv_int(result, "elapsed_seconds", total_elapsed);
    return true;
}

/* ── Global MMR ────────────────────────────────────────── */

static struct mmr g_mmr;
static bool g_mmr_initialized = false;

void rpc_blockchain_mmr_append(const uint8_t block_hash[32])
{
    if (!g_mmr_initialized) {
        mmr_init(&g_mmr);
        g_mmr_initialized = true;
    }
    mmr_append(&g_mmr, block_hash);
}

struct mmr *rpc_blockchain_get_mmr(void) { return &g_mmr; }

void rpc_blockchain_mmr_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT value FROM node_state WHERE key='mmr_state'",
            -1, &s, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *blob = (const uint8_t *)sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob && len >= 12 && mmr_deserialize(&g_mmr, blob, (size_t)len))
            g_mmr_initialized = true;
    }
    sqlite3_finalize(s);
    if (!g_mmr_initialized) {
        mmr_init(&g_mmr);
        g_mmr_initialized = true;
    }
}

void rpc_blockchain_mmr_catchup(struct main_state *ms)
{
    if (!g_mmr_initialized || !ms) return;
    int chain_height = active_chain_height(&ms->chain_active);
    int mmr_height = (int)g_mmr.num_leaves - 1;

    if (mmr_height >= chain_height) return;

    int start = mmr_height + 1;
    int64_t t0 = (int64_t)time(NULL);
    for (int h = start; h <= chain_height; h++) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (bi && bi->phashBlock)
            mmr_append(&g_mmr, bi->phashBlock->data);
    }
    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("MMR catchup: %d → %d (%d blocks, %llds)\n",
           start, chain_height, chain_height - start + 1, (long long)elapsed);
}

void rpc_blockchain_mmr_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_mmr_initialized) return;
    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = mmr_serialize(&g_mmr, buf, sizeof(buf));
    if (len == 0) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('mmr_state',?)", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_blob(s, 1, buf, (int)len, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* ── Global MMB (Merkle Mountain Belt) ────────────────── */

static struct mmb g_mmb;
static bool g_mmb_initialized = false;

void rpc_blockchain_mmb_append(const struct mmb_leaf *leaf)
{
    if (!g_mmb_initialized) {
        mmb_init(&g_mmb);
        g_mmb_initialized = true;
    }
    mmb_append(&g_mmb, leaf);
}

struct mmb *rpc_blockchain_get_mmb(void) { return &g_mmb; }

void rpc_blockchain_mmb_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT value FROM node_state WHERE key='mmb_state'",
            -1, &s, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *blob = (const uint8_t *)sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob && len >= 13 && mmb_deserialize(&g_mmb, blob, (size_t)len))
            g_mmb_initialized = true;
    }
    sqlite3_finalize(s);
    if (!g_mmb_initialized) {
        mmb_init(&g_mmb);
        g_mmb_initialized = true;
    }
    printf("MMB: loaded %llu leaves, %u peaks\n",
           (unsigned long long)g_mmb.num_leaves, g_mmb.num_mountains);
}

void rpc_blockchain_mmb_catchup(struct main_state *ms)
{
    if (!g_mmb_initialized || !ms) return;
    int chain_height = active_chain_height(&ms->chain_active);
    int mmb_height = (int)g_mmb.num_leaves - 1;

    if (mmb_height >= chain_height) return;

    int start = mmb_height + 1;
    int64_t t0 = (int64_t)time(NULL);
    for (int h = start; h <= chain_height; h++) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (bi && bi->phashBlock) {
            struct mmb_leaf leaf;
            mmb_leaf_from_block(&leaf,
                bi->phashBlock->data,
                bi->nHeight, bi->nTime, bi->nBits,
                bi->hashFinalSaplingRoot.data,
                (const uint8_t *)bi->nChainWork.pn);
            mmb_append(&g_mmb, &leaf);
        }
    }
    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("MMB catchup: %d → %d (%d blocks, %llds, %u peaks)\n",
           start, chain_height, chain_height - start + 1,
           (long long)elapsed, g_mmb.num_mountains);
}

void rpc_blockchain_mmb_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_mmb_initialized) return;
    uint8_t buf[MMB_SERIALIZED_MAX];
    size_t len = mmb_serialize(&g_mmb, buf, sizeof(buf));
    if (len == 0) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('mmb_state',?)", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_blob(s, 1, buf, (int)len, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* ── Commitment MMR (UTXO state binding) ─────────────── */

static struct mmr g_commitment_mmr;
static bool g_commitment_mmr_initialized = false;

struct mmr *rpc_blockchain_get_commitment_mmr(void)
{
    if (!g_commitment_mmr_initialized) {
        mmr_init(&g_commitment_mmr);
        g_commitment_mmr_initialized = true;
    }
    return &g_commitment_mmr;
}

void rpc_blockchain_commitment_mmr_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT value FROM node_state WHERE key='commitment_mmr_state'",
            -1, &s, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *blob = (const uint8_t *)sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob && len >= 12 &&
            mmr_deserialize(&g_commitment_mmr, blob, (size_t)len)) {
            g_commitment_mmr_initialized = true;
            printf("Commitment MMR loaded: %llu leaves\n",
                   (unsigned long long)g_commitment_mmr.num_leaves);
        }
    }
    sqlite3_finalize(s);
    if (!g_commitment_mmr_initialized) {
        mmr_init(&g_commitment_mmr);
        g_commitment_mmr_initialized = true;
    }
}

void rpc_blockchain_commitment_mmr_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_commitment_mmr_initialized) return;
    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = mmr_serialize(&g_commitment_mmr, buf, sizeof(buf));
    if (len == 0) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('commitment_mmr_state',?)", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_blob(s, 1, buf, (int)len, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void rpc_blockchain_maybe_commit(int32_t height,
                                  const uint8_t block_hash[32],
                                  const uint8_t xor_accumulator[32],
                                  uint64_t utxo_count)
{
    if (height <= 0 || height % MMR_COMMITMENT_INTERVAL != 0)
        return;

    /* Skip commitment during assume-valid IBD — the MMR will be built
     * from scratch once we pass assume-valid height. */
    extern int g_assume_valid_height;
    if (g_assume_valid_height >= 0 && height <= g_assume_valid_height)
        return;

    if (!g_commitment_mmr_initialized) {
        mmr_init(&g_commitment_mmr);
        g_commitment_mmr_initialized = true;
    }

    /* Use the incremental XOR accumulator (O(1)) instead of the O(N)
     * SHA3 full-table scan that was killing sync performance. */
    struct mmr_commitment c = { .height = height };
    memcpy(c.block_hash, block_hash, 32);
    memcpy(c.utxo_root, xor_accumulator, 32);
    memset(c.data_root, 0, 32);

    mmr_append_commitment(&g_commitment_mmr, &c);

    if (height % 10000 == 0) {
        uint8_t root[32];
        mmr_root(&g_commitment_mmr, root);
        printf("MMR commitment: h=%d utxos=%llu root=",
               height, (unsigned long long)utxo_count);
        for (int i = 0; i < 8; i++) printf("%02x", root[i]);
        printf("... (%u leaves)\n",
               (unsigned)g_commitment_mmr.num_leaves);
    }
}

/* ── SHA3 UTXO commitment RPC ──────────────────────────── */

static bool rpc_getutxocommitment(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getutxocommitment\n"
        "\nComputes SHA3-256 hash over the entire UTXO set in canonical order.\n"
        "This is a deterministic commitment that two nodes can compare.\n");

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Database not available");
        LOG_FAIL("blockchain", "getutxocommitment: database not available");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Chain not loaded");
        LOG_FAIL("blockchain", "getutxocommitment: chain not loaded");
    }

    /* Flush coins cache first */
    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    uint8_t sha3_hash[32];
    uint64_t count = 0;
    int64_t t0 = (int64_t)time(NULL);
    utxo_commitment_sha3_compute(ctx->node_db->db, sha3_hash, &count);
    int64_t elapsed = (int64_t)time(NULL) - t0;

    int tip = active_chain_height(&ctx->main_state->chain_active);

    /* Save checkpoint */
    utxo_commitment_sha3_save(ctx->node_db->db, sha3_hash, tip, count);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", sha3_hash[i]);

    json_set_object(result);
    json_push_kv_str(result, "sha3_hash", hex);
    json_push_kv_int(result, "height", tip);
    json_push_kv_int(result, "utxo_count", (int64_t)count);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

/* ── SHA3 checkpoint verification RPC ──────────────────── */

static bool rpc_verifycheckpoint(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "verifycheckpoint\n"
        "\nVerifies the UTXO set against the hardcoded SHA3 checkpoint.\n"
        "Returns pass/fail with details. Flushes coins cache first.\n");

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp) {
        json_set_str(result, "No checkpoint available");
        LOG_FAIL("blockchain", "verifycheckpoint: no SHA3 checkpoint available");
    }
    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Database not available");
        LOG_FAIL("blockchain", "verifycheckpoint: database not available");
    }
    if (!ctx->main_state) {
        json_set_str(result, "Chain not loaded");
        LOG_FAIL("blockchain", "verifycheckpoint: chain not loaded");
    }

    int tip = active_chain_height(&ctx->main_state->chain_active);
    if (tip < cp->height) {
        json_set_object(result);
        json_push_kv_str(result, "status", "pending");
        json_push_kv_int(result, "checkpoint_height", cp->height);
        json_push_kv_int(result, "current_height", tip);
        return true;
    }

    /* Flush coins cache */
    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    uint8_t sha3_hash[32];
    uint64_t count = 0;
    utxo_commitment_sha3_compute(ctx->node_db->db, sha3_hash, &count);

    bool match = (memcmp(sha3_hash, cp->sha3_hash, 32) == 0);

    char local_hex[65], expected_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(local_hex + i * 2, 3, "%02x", sha3_hash[i]);
        snprintf(expected_hex + i * 2, 3, "%02x", cp->sha3_hash[i]);
    }

    json_set_object(result);
    json_push_kv_str(result, "status", match ? "PASSED" : "FAILED");
    json_push_kv_int(result, "checkpoint_height", cp->height);
    json_push_kv_str(result, "expected_sha3", expected_hex);
    json_push_kv_str(result, "computed_sha3", local_hex);
    json_push_kv_int(result, "expected_utxos", (int64_t)cp->utxo_count);
    json_push_kv_int(result, "computed_utxos", (int64_t)count);

    if (match)
        printf("SHA3 checkpoint verification: PASSED at height %d\n",
               cp->height);
    else
        printf("SHA3 checkpoint verification: FAILED at height %d!\n"
               "  expected: %s\n  computed: %s\n",
               cp->height, expected_hex, local_hex);

    return true;
}

/* ── Full data integrity hash RPC ──────────────────────── */

static bool rpc_getdataintegrity(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getdataintegrity\n"
        "\nComputes SHA3-256 hashes over ALL consensus data tables:\n"
        "blocks, transactions, tx_inputs, tx_outputs, utxos,\n"
        "sapling_nullifiers, sapling_outputs, sapling_spends,\n"
        "sprout_nullifiers, joinsplits, zslp_tokens, zslp_transfers.\n"
        "Returns per-table hashes and a master hash combining all.\n");

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Database not available");
        LOG_FAIL("blockchain", "getdataintegrity: database not available");
    }

    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    int64_t t0 = (int64_t)time(NULL);
    struct data_integrity_detail d;
    data_integrity_compute(ctx->node_db->db, &d);
    int64_t elapsed = (int64_t)time(NULL) - t0;

    json_set_object(result);

    /* Helper: convert 32-byte hash to hex and push as kv */
    const struct { const char *name; const uint8_t *hash; } tables[] = {
        { "blocks",              d.blocks },
        { "transactions",        d.transactions },
        { "tx_inputs",           d.tx_inputs },
        { "tx_outputs",          d.tx_outputs },
        { "utxos",               d.utxos },
        { "sapling_nullifiers",  d.sapling_nullifiers },
        { "sapling_outputs",     d.sapling_outputs },
        { "sapling_spends",      d.sapling_spends },
        { "sprout_nullifiers",   d.sprout_nullifiers },
        { "joinsplits",          d.joinsplits },
        { "zslp_tokens",         d.zslp_tokens },
        { "zslp_transfers",      d.zslp_transfers },
        { "master",              d.master },
    };

    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char hex[65];
        for (int j = 0; j < 32; j++)
            snprintf(hex + j * 2, 3, "%02x", tables[i].hash[j]);
        json_push_kv_str(result, tables[i].name, hex);
    }

    int tip = ctx->main_state ? active_chain_height(&ctx->main_state->chain_active) : 0;
    json_push_kv_int(result, "height", tip);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

/* ── MMR root RPC ──────────────────────────────────────── */

static bool rpc_getmmrroot(const struct json_value *params, bool help,
                             struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getmmrroot\n"
        "\nReturns the Merkle Mountain Range root over all block hashes.\n"
        "Uses SHA3-256 with domain separation for power node sync.\n");

    if (!g_mmr_initialized) {
        json_set_str(result, "MMR not initialized");
        LOG_FAIL("blockchain", "getmmrroot: MMR not initialized");
    }

    uint8_t root[32];
    mmr_root(&g_mmr, root);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", root[i]);

    json_set_object(result);
    json_push_kv_str(result, "mmr_root", hex);
    json_push_kv_int(result, "num_leaves", (int64_t)g_mmr.num_leaves);
    json_push_kv_int(result, "num_peaks", g_mmr.num_peaks);
    return true;
}

static bool rpc_getcommitmentmmr(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getcommitmentmmr\n"
        "\nReturns the commitment MMR root (binds UTXO state to PoW chain).\n"
        "Each leaf: SHA3(height || block_hash || utxo_root) every 100 blocks.\n"
        "Used to verify imported UTXO snapshots without replaying history.\n");

    struct mmr *cm = rpc_blockchain_get_commitment_mmr();
    uint8_t root[32];
    mmr_root(cm, root);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", root[i]);

    json_set_object(result);
    json_push_kv_str(result, "commitment_mmr_root", hex);
    json_push_kv_int(result, "num_commitments", (int64_t)cm->num_leaves);
    json_push_kv_int(result, "commitment_interval", MMR_COMMITMENT_INTERVAL);
    json_push_kv_int(result, "covers_height",
        (int64_t)cm->num_leaves * MMR_COMMITMENT_INTERVAL);
    return true;
}

static bool rpc_auditchain(const struct json_value *params, bool help,
                            struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "auditchain\n"
        "\nFull chain audit: verify block-hash MMR and commitment MMR.\n"
        "Reports the state of both MMRs and what height they cover.\n"
        "Use reindexchainstate for full block replay + UTXO rebuild.\n"
        "Use verifycheckpoint to check UTXO SHA3 at hardcoded height.\n");

    json_set_object(result);

    /* Block-hash MMR */
    uint8_t broot[32];
    mmr_root(&g_mmr, broot);
    char bhex[65];
    for (int i = 0; i < 32; i++)
        snprintf(bhex + i * 2, 3, "%02x", broot[i]);
    json_push_kv_str(result, "block_mmr_root", bhex);
    json_push_kv_int(result, "block_mmr_leaves", (int64_t)g_mmr.num_leaves);

    /* Commitment MMR */
    struct mmr *cm = rpc_blockchain_get_commitment_mmr();
    uint8_t croot[32];
    mmr_root(cm, croot);
    char chex[65];
    for (int i = 0; i < 32; i++)
        snprintf(chex + i * 2, 3, "%02x", croot[i]);
    json_push_kv_str(result, "commitment_mmr_root", chex);
    json_push_kv_int(result, "commitment_leaves", (int64_t)cm->num_leaves);
    json_push_kv_int(result, "commitment_covers_height",
        (int64_t)cm->num_leaves * MMR_COMMITMENT_INTERVAL);

    /* Chain state */
    int chain_h = ctx->main_state ? active_chain_height(
        &ctx->main_state->chain_active) : 0;
    json_push_kv_int(result, "chain_height", chain_h);

    /* Consistency check */
    bool block_mmr_ok = (int64_t)g_mmr.num_leaves >= chain_h;
    bool commit_ok = (int64_t)cm->num_leaves * MMR_COMMITMENT_INTERVAL >=
                     chain_h - MMR_COMMITMENT_INTERVAL;
    json_push_kv_bool(result, "block_mmr_consistent", block_mmr_ok);
    json_push_kv_bool(result, "commitment_mmr_consistent", commit_ok);
    json_push_kv_bool(result, "audit_passed", block_mmr_ok && commit_ok);

    return true;
}

/* ── rebuildsaplingtree — replay Sapling outputs, rebuild tree ─── */

static bool rpc_rebuildsaplingtree(const struct json_value *params,
                                    bool help, struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "rebuildsaplingtree\n"
        "\nRebuild the Sapling commitment tree by replaying every shielded\n"
        "output from Sapling activation (height 476969) to chain tip.\n"
        "Verifies computed root matches hashFinalSaplingRoot in each header.\n"
        "Fixes tree divergence caused by corrupted persisted state.\n"
        "This may take several minutes for the full chain.\n");

    if (!ctx->main_state || !ctx->datadir || !ctx->node_db) {
        json_set_str(result, "error: node not fully initialized");
        LOG_FAIL("blockchain", "rebuildsaplingtree: node not fully initialized (main_state=%p datadir=%p node_db=%p)",
                 (void *)ctx->main_state, (void *)ctx->datadir, (void *)ctx->node_db);
    }

    struct main_state *ms = ctx->main_state;
    struct node_db *ndb = ctx->node_db;
    const char *datadir = ctx->datadir;

    extern _Atomic bool g_sapling_tree_rebuilding;
    atomic_store(&g_sapling_tree_rebuilding, true);
    int n = sapling_tree_rebuild(ndb, &ms->chain_active, datadir);
    atomic_store(&g_sapling_tree_rebuilding, false);

    if (n < 0) {
        json_set_str(result, "error: rebuild failed");
        LOG_FAIL("blockchain", "rebuildsaplingtree: sapling_tree_rebuild returned %d", n);
    }

    /* Reload rebuilt tree into main_state */
    uint8_t tbuf[8192];
    size_t tlen = 0;
    if (node_db_state_get(ndb, "sapling_tree", tbuf, sizeof(tbuf), &tlen)
        && tlen > 0) {
        struct byte_stream ts;
        stream_init_from_data(&ts, tbuf, tlen);
        sapling_tree_init(&ms->sapling_tree);
        incremental_tree_deserialize(&ms->sapling_tree, &ts);
        ms->sapling_tree_loaded = true;
    }

    /* Verify against chain tip */
    struct uint256 final_root;
    incremental_tree_root(&ms->sapling_tree, &final_root);
    char root_hex[65];
    uint256_get_hex(&final_root, root_hex);

    const struct block_index *tip = active_chain_tip(&ms->chain_active);
    char expected_hex[65] = "n/a";
    bool roots_match = false;
    if (tip) {
        uint256_get_hex(&tip->hashFinalSaplingRoot, expected_hex);
        roots_match = (memcmp(final_root.data,
                              tip->hashFinalSaplingRoot.data, 32) == 0);
    }

    json_set_object(result);
    json_push_kv_str(result, "status", roots_match ? "success" : "mismatch");
    json_push_kv_int(result, "total_commitments", (int64_t)n);
    json_push_kv_int(result, "tree_size",
        (int64_t)incremental_tree_size(&ms->sapling_tree));
    json_push_kv_str(result, "computed_root", root_hex);
    json_push_kv_str(result, "expected_root", expected_hex);
    json_push_kv_bool(result, "roots_match", roots_match);
    return true;
}

void register_blockchain_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "blockchain", "getblockcount",     rpc_getblockcount,     true },
        { "blockchain", "getbestblockhash",  rpc_getbestblockhash,  true },
        { "blockchain", "getdifficulty",     rpc_getdifficulty,     true },
        { "blockchain", "getblockhash",      rpc_getblockhash,      true },
        { "blockchain", "getblockheader",    rpc_getblockheader,    true },
        { "blockchain", "getblock",          rpc_getblock,          true },
        { "blockchain", "getblockchaininfo", rpc_getblockchaininfo, true },
        { "blockchain", "getmempoolinfo",    rpc_getmempoolinfo,    true },
        { "blockchain", "gettxoutsetinfo",      rpc_gettxoutsetinfo,      true },
        /* HODL wave commands in hodl_controller.c */
        { "blockchain", "reindexchainstate",    rpc_reindexchainstate,     false },
        { "blockchain", "importchainstate",     rpc_importchainstate,       false },
        { "blockchain", "indexlegacy",          rpc_indexlegacy,            false },
        { "blockchain", "getutxocommitment",   rpc_getutxocommitment,     true },
        { "blockchain", "getmmrroot",          rpc_getmmrroot,            true },
        { "blockchain", "getcommitmentmmr",   rpc_getcommitmentmmr,     true },
        { "blockchain", "auditchain",          rpc_auditchain,            true },
        { "blockchain", "verifycheckpoint",    rpc_verifycheckpoint,      true },
        { "blockchain", "getdataintegrity",    rpc_getdataintegrity,      true },
        { "blockchain", "rebuildsaplingtree", rpc_rebuildsaplingtree,    false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
