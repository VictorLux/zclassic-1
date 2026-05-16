/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Chain-state RPCs: getblockchaininfo, getmempoolinfo, gettxoutsetinfo,
 * UTXO commitments + audit, MMR/MMB roots, data integrity, checkpoint
 * verification, sapling-tree rebuild and chain audit. See
 * blockchain_controller_internal.h for shared declarations. */

#include "controllers/blockchain_controller.h"
#include "blockchain_controller_internal.h"
#include "controllers/network_controller.h"
#include "controllers/strong_params.h"
#include "controllers/sync_controller.h"
#include "views/format_helpers.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/checkpoints.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "net/connman.h"
#include "primitives/block.h"
#include "sapling/incremental_merkle_tree.h"
#include "services/utxo_audit_service.h"
#include "storage/block_index_db.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool rpc_getblockchaininfo(const struct json_value *params, bool help,
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

bool rpc_getmempoolinfo(const struct json_value *params, bool help,
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
bool rpc_gettxoutsetinfo(const struct json_value *params, bool help,
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
    if (s && AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
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

bool rpc_getutxocommitment(const struct json_value *params, bool help,
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

static void utxo_audit_result_to_json(const struct utxo_audit_result *audit,
                                      struct json_value *result)
{
    json_set_object(result);
    json_push_kv_str(result, "status", utxo_audit_status_name(audit->status));
    json_push_kv_bool(result, "drift_detected", audit->drift_detected);
    json_push_kv_str(result, "local_sha3", audit->local_sha3);
    json_push_kv_int(result, "local_height", audit->local_height);
    json_push_kv_int(result, "local_utxo_count",
                     (int64_t)audit->local_utxo_count);
    if (audit->remote_sha3[0])
        json_push_kv_str(result, "remote_sha3", audit->remote_sha3);
    if (audit->remote_height > 0)
        json_push_kv_int(result, "remote_height", audit->remote_height);
    if (audit->source[0])
        json_push_kv_str(result, "source", audit->source);
    if (audit->error[0])
        json_push_kv_str(result, "error", audit->error);
}

bool rpc_getutxoaudit(const struct json_value *params, bool help,
                             struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
        "getutxoaudit [remote_sha3] [remote_height] [source]\n"
        "\nComputes the local SHA3 UTXO commitment and optionally compares it\n"
        "to a peer-supplied commitment. A mismatch is advisory: it emits an\n"
        "event and sets node_state['utxo_drift_detected']; it never wipes.\n");

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Database not available");
        LOG_FAIL("blockchain", "getutxoaudit: database not available");
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 3);
    const char *remote_sha3 = rpc_permit_str(&p, 0, "remote_sha3", NULL);
    int64_t remote_height = rpc_permit_int(&p, 1, "remote_height", 0);
    const char *source = rpc_permit_str(&p, 2, "source", "peer-commitment");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        LOG_FAIL("blockchain", "getutxoaudit: invalid params");
    }

    int height = 0;
    if (ctx->main_state)
        height = active_chain_height(&ctx->main_state->chain_active);
    if (remote_height <= 0)
        remote_height = height;

    struct utxo_audit_result audit;
    bool ok = remote_sha3 && remote_sha3[0]
        ? utxo_audit_compare_remote(ctx->node_db, remote_sha3,
                                    (int32_t)remote_height, source, &audit)
        : utxo_audit_local(ctx->node_db, height, &audit);
    if (!ok) {
        json_set_str(result, "UTXO audit failed");
        LOG_FAIL("blockchain", "getutxoaudit: audit failed");
    }
    utxo_audit_result_to_json(&audit, result);
    return audit.status != UTXO_AUDIT_ERROR;
}

/* ── SHA3 checkpoint verification RPC ──────────────────── */

bool rpc_verifycheckpoint(const struct json_value *params, bool help,
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

bool rpc_getdataintegrity(const struct json_value *params, bool help,
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

bool rpc_getmmrroot(const struct json_value *params, bool help,
                             struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getmmrroot\n"
        "\nReturns the Merkle Mountain Range root over all block hashes.\n"
        "Uses SHA3-256 with domain separation for power node sync.\n");

    if (!rpc_blockchain_mmr_initialized()) {
        json_set_str(result, "MMR not initialized");
        LOG_FAIL("blockchain", "getmmrroot: MMR not initialized");
    }

    struct mmr *bm = rpc_blockchain_get_mmr();
    uint8_t root[32];
    mmr_root(bm, root);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", root[i]);

    json_set_object(result);
    json_push_kv_str(result, "mmr_root", hex);
    json_push_kv_int(result, "num_leaves", (int64_t)bm->num_leaves);
    json_push_kv_int(result, "num_peaks", bm->num_peaks);
    return true;
}

bool rpc_getcommitmentmmr(const struct json_value *params, bool help,
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

bool rpc_auditchain(const struct json_value *params, bool help,
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
    struct mmr *bm = rpc_blockchain_get_mmr();
    uint8_t broot[32];
    mmr_root(bm, broot);
    char bhex[65];
    for (int i = 0; i < 32; i++)
        snprintf(bhex + i * 2, 3, "%02x", broot[i]);
    json_push_kv_str(result, "block_mmr_root", bhex);
    json_push_kv_int(result, "block_mmr_leaves", (int64_t)bm->num_leaves);

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
    bool block_mmr_ok = (int64_t)bm->num_leaves >= chain_h;
    bool commit_ok = (int64_t)cm->num_leaves * MMR_COMMITMENT_INTERVAL >=
                     chain_h - MMR_COMMITMENT_INTERVAL;
    json_push_kv_bool(result, "block_mmr_consistent", block_mmr_ok);
    json_push_kv_bool(result, "commitment_mmr_consistent", commit_ok);
    json_push_kv_bool(result, "audit_passed", block_mmr_ok && commit_ok);

    return true;
}

/* ── rebuildsaplingtree — replay Sapling outputs, rebuild tree ─── */

bool rpc_rebuildsaplingtree(const struct json_value *params,
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

