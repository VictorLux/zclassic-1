/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Runtime service initialization: mempool, P2P, RPC, Tor, HTTPS,
 * mining, wallet sync, shutdown, and utility functions. */

#include "config/boot_internal.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "coins/coins_view.h"
#include "controllers/blockchain_controller.h"
#include "controllers/hodl_controller.h"
#include "controllers/repair_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/misc_controller.h"
#include "controllers/network_controller.h"
#include "controllers/mining_controller.h"
#include "controllers/file_controller.h"
#include "net/file_service.h"
#include "controllers/transaction_controller.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_controller.h"
#include "controllers/wallet_controller.h"
#include "controllers/zslp_controller.h"
#include "controllers/sync_controller.h"
#include "controllers/event_controller.h"
#include "controllers/snapshot_controller.h"
#include "rpc/httpserver.h"
#include "rpc/server.h"
#include "net/https_server.h"
#include "net/fast_sync.h"
#include "net/peer_strategy.h"
#include "net/tor_integration.h"
#include "validation/process_block.h"
#include "event/event.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "sapling/params_init.h"
#include <netdb.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sqlite3.h>

extern int g_assume_valid_height;

extern struct node_db *g_active_node_db;
extern struct tx_mempool *g_active_mempool;
extern struct wallet *g_active_wallet;

/* Module-local pointer to boot context (set once by app_init_services) */
static struct boot_svc_ctx *S;

/* ── Helper threads ────────────────────────────────────────── */

extern void store_process_payments(const char *datadir);
static void *payment_processor_thread(void *arg)
{
    const char *datadir = (const char *)arg;
    while (1) {
        sleep(30);
        store_process_payments(datadir);
    }
    return NULL;
}

extern size_t onion_service_handle_request(const char *, const char *,
    const uint8_t *, size_t, uint8_t *, size_t);

static size_t onion_request_adapter(const char *method, const char *path,
    const uint8_t *req_data, size_t req_len,
    uint8_t *resp, size_t resp_max, void *ctx)
{
    (void)ctx;
    return onion_service_handle_request(method, path,
        req_data, req_len, resp, resp_max);
}

static void *build_snapshot_offer_thread(void *arg)
{
    const char *datadir = (const char *)arg;
    printf("Building fast sync snapshot offer...\n");

    struct snapshot_offer offer;
    if (fast_sync_build_offer(datadir, &offer)) {
        msg_processor_update_offer(&offer);
        printf("Fast sync ready: h=%d, %llu UTXOs\n",
               offer.height, (unsigned long long)offer.num_utxos);
    } else {
        printf("Fast sync: no snapshot available yet\n");
    }

    extern struct sync_manifest g_cached_manifest;
    extern _Atomic bool g_cached_manifest_valid;

    printf("Building chunk sync manifest...\n");
    if (fast_sync_build_manifest(datadir, &g_cached_manifest)) {
        g_cached_manifest_valid = true;
        printf("Chunk manifest ready: h=%d, %u chunks (%llu UTXOs)\n",
               g_cached_manifest.height, g_cached_manifest.num_chunks,
               (unsigned long long)g_cached_manifest.num_utxos);
    } else {
        printf("Chunk manifest: not available yet\n");
    }

    extern struct block_piece_manifest g_cached_block_manifest;
    extern _Atomic bool g_cached_block_manifest_valid;

    int32_t tip_h = offer.height;

    if (tip_h > BLOCKS_PER_PIECE) {
        printf("Building block piece manifest...\n");
        if (block_piece_manifest_build(datadir, 1, tip_h,
                                        &g_cached_block_manifest)) {
            g_cached_block_manifest_valid = true;
            extern int32_t g_manifest_built_at_height;
            g_manifest_built_at_height = tip_h;
            printf("Block manifest ready: h=%d..%d, %u pieces\n",
                   g_cached_block_manifest.start_height,
                   g_cached_block_manifest.end_height,
                   g_cached_block_manifest.num_pieces);
        } else {
            printf("Block manifest: build failed\n");
        }
    }

    return NULL;
}

/* ── Runtime service startup (called from app_init) ────────── */

bool app_init_services(struct app_context *ctx,
                        const struct chain_params *params,
                        struct boot_svc_ctx *svc)
{
    S = svc;

    /* Initialize mempool */
    tx_mempool_init(svc->mempool, 1000);
    g_active_mempool = svc->mempool;

    if (g_active_node_db)
        node_db_sync_mempool_load(g_active_node_db, svc->mempool);

    /* Rescan blockchain for wallet transactions if wallet is behind chain tip */
    {
        struct block_index *chain_tip = active_chain_tip(&svc->state->chain_active);
        int tip_height = active_chain_height(&svc->state->chain_active);
        if (chain_tip && svc->wallet->best_block_height < tip_height) {
            int scan_from = svc->wallet->best_block_height > 0
                ? svc->wallet->best_block_height + 1 : 0;
            if (svc->wallet->time_first_key > 0 && scan_from == 0) {
                int64_t scan_time = svc->wallet->time_first_key - 7200;
                for (int h = tip_height; h >= 0; h--) {
                    struct block_index *bi = active_chain_at(
                        &svc->state->chain_active, h);
                    if (bi && (int64_t)bi->nTime < scan_time) {
                        scan_from = h + 1;
                        break;
                    }
                }
            }
            if (scan_from == 0 && svc->wallet->best_block_height == 0 &&
                tip_height > 1000) {
                printf("Wallet scan height is 0 with %d blocks. "
                       "Use rescanblockchain RPC for targeted rescan.\n",
                       tip_height);
            } else if (tip_height - scan_from < 50000) {
                wallet_rescan(svc->wallet, &svc->state->chain_active,
                              scan_from, tip_height, ctx->datadir);
            } else {
                printf("Wallet needs rescan from %d to %d (%d blocks). "
                       "Deferring — use rescanblockchain RPC.\n",
                       scan_from, tip_height, tip_height - scan_from);
            }
        }
    }

    wallet_verify_utxos(svc->wallet, svc->coins_tip);

    /* Rebuild wallet_utxos from ground truth ONLY if empty */
    if (g_active_node_db && g_active_node_db->open) {
        int64_t t0 = (int64_t)time(NULL);
        sqlite3_stmt *chk = NULL;
        int existing = 0;
        if (sqlite3_prepare_v2(g_active_node_db->db,
                "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
                -1, &chk, NULL) == SQLITE_OK) {
            if (sqlite3_step(chk) == SQLITE_ROW)
                existing = sqlite3_column_int(chk, 0);
            sqlite3_finalize(chk);
        }
        if (existing > 0) {
            printf("wallet_utxos: keeping %d existing UTXOs (synced from zclassicd)\n",
                existing);
        } else {
            char *err = NULL;
            sqlite3_exec(g_active_node_db->db, "BEGIN", NULL, NULL, NULL);
            int rc = sqlite3_exec(g_active_node_db->db,
                "INSERT OR IGNORE INTO wallet_utxos "
                "(txid, vout, value, address_hash, script, height, is_coinbase) "
                "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
                "u.height, u.is_coinbase "
                "FROM utxos u INNER JOIN wallet_keys wk "
                "ON u.address_hash = wk.pubkey_hash",
                NULL, NULL, &err);
            if (err) { printf("wallet_utxos INSERT: %s\n", err); sqlite3_free(err); err = NULL; }
            if (rc != SQLITE_OK)
                sqlite3_exec(g_active_node_db->db, "ROLLBACK", NULL, NULL, NULL);
            else
                sqlite3_exec(g_active_node_db->db, "COMMIT", NULL, NULL, NULL);
        }
        int64_t bal = 0;
        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT COALESCE(sum(value),0) FROM wallet_utxos "
            "WHERE spent_txid IS NULL", -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            bal = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        int cnt = 0;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            cnt = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        printf("Wallet: %.8f ZCL (%d UTXOs, %lldms)\n",
               (double)bal / 1e8, cnt,
               (long long)((int64_t)time(NULL) - t0) * 1000);
    }

    /* Sync wallet keys to SQLite */
    if (g_active_node_db)
        node_db_sync_wallet_keys(g_active_node_db, svc->wallet);

    /* Initialize message processor */
    msg_processor_init(svc->msg_processor, svc->state, svc->mempool,
                       svc->coins_tip, params, ctx->datadir,
                       &svc->connman->manager);

    /* Initialize P2P connection manager */
    struct node_signals signals = {
        .get_height = msg_get_height,
        .process_messages = msg_process_messages,
        .send_messages = msg_send_messages,
        .initialize_node = NULL,
        .finalize_node = NULL,
        .ctx = svc->msg_processor,
    };
    connman_init(svc->connman, params, &signals);
    svc->connman->datadir = ctx->datadir;

    /* Load persisted peer addresses from previous session */
    connman_load_addrman(svc->connman);

    addr_db_read(&svc->connman->manager, ctx->datadir);

    if (ctx->listen) {
        struct net_service bind4;
        net_service_init(&bind4);
        unsigned char any4[4] = {0, 0, 0, 0};
        net_addr_set_ipv4(&bind4.addr, any4);
        bind4.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&svc->connman->manager, &bind4, false))
            printf("P2P listening on 0.0.0.0:%d\n", ctx->p2p_port);
        struct net_service bind6;
        net_service_init(&bind6);
        bind6.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&svc->connman->manager, &bind6, false))
            printf("P2P listening on [::]:%d\n", ctx->p2p_port);
    }

    /* Wait for ZK params before P2P (needed for block verification) */
    if (ctx->params_dir) {
        pthread_join(svc->params_thread, NULL);
        if (!atomic_load(svc->params_loaded))
            fprintf(stderr, "Warning: ZK params not loaded\n");
    }

    /* File sync BEFORE P2P — download block files first, then start P2P.
     * This prevents concurrent writes to block files (file sync + P2P
     * both writing to blk*.dat caused crashes). */
    {
        int chain_height = active_chain_height(&svc->state->chain_active);
        if (chain_height <= 0) {
            printf("=== Fresh node — trying fast file sync ===\n");
            uint8_t utxo_root[32];
            memset(utxo_root, 0, 32);
            const char *file_seeds[] = {
                "74.50.74.102",
                "205.209.104.118",
                "140.174.189.3",
                NULL
            };
            bool file_sync_ok = false;
            for (int round = 0; round < 3 && !file_sync_ok; round++) {
                if (round > 0) {
                    printf("File sync: retrying in 10s (round %d/3)...\n",
                           round + 1);
                    sleep(10);
                }
                for (int i = 0; file_seeds[i] && !file_sync_ok; i++) {
                    printf("Trying file service at %s:%d...\n",
                           file_seeds[i], FS_PORT);
                    int64_t t0 = (int64_t)time(NULL);
                    if (fs_client_sync(file_seeds[i], FS_PORT,
                                        ctx->datadir, utxo_root)) {
                        int64_t elapsed = (int64_t)time(NULL) - t0;
                        printf("=== File sync complete from %s: %llds ===\n",
                               file_seeds[i], (long long)elapsed);
                        file_sync_ok = true;
                    }
                }
            }
        }
    }

    connman_start(svc->connman);
    sync_set_state(SYNC_FINDING_PEERS, "P2P started");

    /* Initialize RPC */
    rpc_table_init(svc->rpc_table);
    rpc_blockchain_set_state(svc->state, svc->mempool, ctx->datadir);
    rpc_blockchain_set_coins_db(NULL, svc->coins_tip);
    rpc_blockchain_set_node_db(g_active_node_db);
    rpc_blockchain_mmr_init_from_state(g_active_node_db);
    rpc_blockchain_mmr_catchup(svc->state);
    register_blockchain_rpc_commands(svc->rpc_table);

    rpc_hodl_set_state(svc->state, svc->coins_tip, g_active_node_db,
                        ctx->datadir);
    register_hodl_rpc_commands(svc->rpc_table);

    rpc_repair_set_state(svc->state, svc->coins_tip, g_active_node_db);
    register_repair_rpc_commands(svc->rpc_table);

    rpc_chain_inspect_set_state(svc->state, ctx->datadir,
                                 NULL, svc->coins_tip, g_active_node_db);
    register_chain_inspect_rpc_commands(svc->rpc_table);

    explorer_set_state(svc->state, svc->mempool, svc->coins_tip,
                        g_active_node_db, ctx->datadir);

    api_set_state(svc->state, svc->mempool, svc->coins_tip,
                   g_active_node_db, ctx->datadir);

    rpc_rawtx_set_state(svc->state, svc->mempool, svc->coins_tip, ctx->datadir);
    rpc_rawtx_set_keystore(&svc->wallet->keystore);
    rpc_rawtx_set_connman(svc->connman);
    register_rawtransaction_rpc_commands(svc->rpc_table);

    rpc_mining_set_state(svc->state, svc->mempool, svc->coins_tip, ctx->datadir);
    register_mining_rpc_commands(svc->rpc_table);

    rpc_misc_set_state(svc->state);
    rpc_misc_set_wallet(svc->wallet);
    register_misc_rpc_commands(svc->rpc_table);
    rpc_net_set_connman(svc->connman);
    register_net_rpc_commands(svc->rpc_table);

    /* File transfer service — SHA3-verified chunk serving */
    file_controller_init(ctx->datadir);
    register_file_rpc_commands(svc->rpc_table);

    /* blk_sync.dat from file service is on disk. P2P will re-request
     * blocks it needs — the OS disk cache serves them fast since the
     * data is already in memory from the recent file sync download.
     * The deferred scanner was causing crashes (SIGABRT from concurrent
     * block_index access) and isn't worth the complexity. */

    /* Start file service server on dedicated port.
     * Auto-serves blockchain data to any ZCL23 peer that connects. */
    fs_server_start(ctx->datadir, FS_PORT);

    rpc_wallet_set_state(svc->wallet, svc->state, ctx->datadir, svc->wallet_sqlite,
                         svc->mempool, svc->connman);
    rpc_wallet_set_coins_tip(svc->coins_tip);
    rpc_wallet_set_node_db(g_active_node_db);
    register_wallet_rpc_commands(svc->rpc_table);
    register_event_rpc_commands(svc->rpc_table);

    zslp_rpc_set_datadir(ctx->datadir);
    register_zslp_rpc_commands(svc->rpc_table);

    /* Pre-compute fast sync snapshot offer in background */
    {
        static char s_offer_datadir[1024];
        snprintf(s_offer_datadir, sizeof(s_offer_datadir), "%s", ctx->datadir);
        static pthread_t offer_thread;
        pthread_create(&offer_thread, NULL, build_snapshot_offer_thread,
                        s_offer_datadir);
        pthread_detach(offer_thread);
    }

    /* Start RPC HTTP server */
    set_rpc_warmup_finished();
    rpc_http_start(svc->rpc_table, (uint16_t)ctx->rpc_port,
                    ctx->rpc_user, ctx->rpc_password, ctx->datadir);

    /* Configure API + explorer RPC backends */
    {
        char cookie_path[1024], cookie[256] = "";
        snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", ctx->datadir);
        FILE *cf = fopen(cookie_path, "r");
        if (cf) {
            size_t n = fread(cookie, 1, sizeof(cookie) - 1, cf);
            fclose(cf);
            cookie[n] = '\0';
            char *nl = strchr(cookie, '\n');
            if (nl) *nl = '\0';
            char *colon = strchr(cookie, ':');
            if (colon) {
                *colon = '\0';
                api_set_rpc_backend(cookie, colon + 1, ctx->rpc_port);
                explorer_set_rpc(cookie, colon + 1, ctx->rpc_port);
            }
        } else if (ctx->rpc_user && ctx->rpc_password) {
            api_set_rpc_backend(ctx->rpc_user, ctx->rpc_password,
                                ctx->rpc_port);
            explorer_set_rpc(ctx->rpc_user, ctx->rpc_password,
                             ctx->rpc_port);
        }
    }

    api_start_cache();

    /* Start HTTPS block explorer (deferred during IBD) */
    {
        char cert_path[1024], key_path[1024];
        snprintf(cert_path, sizeof(cert_path), "%s/ssl/fullchain.pem",
                 ctx->datadir);
        snprintf(key_path, sizeof(key_path), "%s/ssl/privkey.pem",
                 ctx->datadir);
        if (access(cert_path, R_OK) == 0 && access(key_path, R_OK) == 0) {
            int chain_tip_h = active_chain_height(&svc->state->chain_active);
            int best_header = svc->state->pindex_best_header ?
                svc->state->pindex_best_header->nHeight : chain_tip_h;
            bool near_tip = (best_header - chain_tip_h < 1000) &&
                            (chain_tip_h > g_assume_valid_height - 10000);
            if (near_tip) {
                https_server_start(cert_path, key_path, "zclnet.net");
            } else {
                printf("HTTPS: deferred during IBD (chain=%d, headers=%d, "
                       "behind=%d). Will start when near tip.\n",
                       chain_tip_h, best_header, best_header - chain_tip_h);
                static char s_cert[1024], s_key[1024];
                strncpy(s_cert, cert_path, sizeof(s_cert) - 1);
                strncpy(s_key, key_path, sizeof(s_key) - 1);
                extern void https_deferred_set(const char *cert, const char *key);
                https_deferred_set(s_cert, s_key);
            }
        } else {
            printf("HTTPS: no cert at %s — block explorer not on clearnet\n",
                   cert_path);
        }
    }

    /* Start miner if -gen */
    if (ctx->gen) {
        svc->gen->ms = svc->state;
        svc->gen->coins_tip = svc->coins_tip;
        svc->gen->mempool = svc->mempool;
        svc->gen->params = params;
        svc->gen->datadir = ctx->datadir;
        svc->gen->num_threads = ctx->gen_threads > 0 ? ctx->gen_threads : 1;
        svc->gen->coinbase_script.size = 0;

        if (ctx->miner_address) {
            size_t pk_pfx_len, sc_pfx_len;
            const unsigned char *pk_pfx = chain_params_base58_prefix(
                params, B58_PUBKEY_ADDRESS, &pk_pfx_len);
            const unsigned char *sc_pfx = chain_params_base58_prefix(
                params, B58_SCRIPT_ADDRESS, &sc_pfx_len);
            struct tx_destination dest;
            if (decode_destination(ctx->miner_address, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, &dest))
                script_for_destination(&svc->gen->coinbase_script, &dest);
        }

        gen_start(svc->gen);
    }

    /* Start embedded Tor only if explicitly requested (-tor flag)
     * or if the node has a previous .onion key (returning node).
     * Fresh nodes skip Tor to avoid SIGABRT from bad torrc configs.
     * Clearnet P2P + file service works fine without Tor. */
    {
        char onion_dir[512];
        snprintf(onion_dir, sizeof(onion_dir), "%s/onion-keys", ctx->datadir);
        struct stat onion_st;
        bool has_onion_keys = (stat(onion_dir, &onion_st) == 0);

        if (ctx->tor || has_onion_keys) {
            extern const char *onion_service_start(const char *);
            onion_service_start(ctx->datadir);
            tor_integration_set_handler(onion_request_adapter, NULL);
            printf("Starting embedded Tor...\n");
            if (!tor_integration_start(ctx->datadir, (uint16_t)ctx->p2p_port))
                fprintf(stderr, "Warning: Tor failed to start\n");
            else {
                const char *onion = tor_integration_get_onion_address();
                if (onion)
                    printf("Tor .onion address: %s\n", onion);
                else
                    printf("Tor: bootstrapping...\n");
            }
        } else {
            printf("Tor: skipped (use -tor to enable)\n");
        }
    }

    /* Discover peer reachability */
    {
        static struct node_profile g_node_profile;
        peer_strategy_discover_self(&g_node_profile,
                                    (uint16_t)ctx->p2p_port);

        const char *cn = g_node_profile.has_public_ip ? "yes" : "no";
        const char *method = "";
        if (g_node_profile.nat_pmp_available)
            method = " (NAT-PMP)";
        else if (g_node_profile.upnp_available)
            method = " (UPnP)";
        const char *tor = g_node_profile.tor_available ? "yes" : "no";
        printf("Reachability: clearnet=%s%s tor=%s\n", cn, method, tor);

        char addrs[4][68];
        int n = peer_strategy_get_addresses(&g_node_profile, addrs, 4);
        if (n > 0) {
            printf("Addresses:");
            for (int i = 0; i < n; i++)
                printf(" %s", addrs[i]);
            printf("\n");
        }
    }

    /* Start store payment processor */
    {
        static char s_pay_datadir[1024];
        snprintf(s_pay_datadir, sizeof(s_pay_datadir), "%s", ctx->datadir);
        static pthread_t pt;
        pthread_create(&pt, NULL, payment_processor_thread, s_pay_datadir);
        pthread_detach(pt);
    }

    atomic_store(svc->running, true);
    {
        struct block_index *tip = active_chain_tip(&svc->state->chain_active);
        int h = tip ? tip->nHeight : 0;
        event_emitf(EV_NODE_READY, 0, "height=%d peers=%zu",
                    h, svc->connman->manager.num_nodes);
    }
    printf("ZClassic C23 node initialized.\n");

    /* SQLite catchup */
    if (g_active_node_db) {
        if (ctx->fastsync_dir) {
            struct block_index *fs_tip = active_chain_tip(&svc->state->chain_active);
            printf("=== SQLite Indexing (%d blocks) ===\n",
                   fs_tip ? fs_tip->nHeight : 0);
            int64_t t_import = (int64_t)time(NULL);
            node_db_sync_catchup(g_active_node_db,
                                 &svc->state->chain_active,
                                 svc->wallet, ctx->datadir);
            int64_t t_idx_done = (int64_t)time(NULL);
            printf("Block index: %llds\n", (long long)(t_idx_done - t_import));
            printf("=== SQLite complete in %llds ===\n",
                   (long long)(t_idx_done - t_import));
        } else {
            static pthread_t catchup_thread;
            static struct {
                struct node_db *ndb;
                const struct active_chain *chain;
                const struct wallet *w;
                const char *datadir;
            } catchup_args;
            catchup_args.ndb = g_active_node_db;
            catchup_args.chain = &svc->state->chain_active;
            catchup_args.w = svc->wallet;
            catchup_args.datadir = ctx->datadir;
            pthread_create(&catchup_thread, NULL, (void *(*)(void *))
                node_db_sync_catchup_thread, &catchup_args);
            pthread_detach(catchup_thread);
        }
    }

    return true;
}

/* ── Shutdown ──────────────────────────────────────────────── */

void app_shutdown_svc(struct boot_svc_ctx *svc)
{
    atomic_store(svc->running, false);
    event_emitf(EV_NODE_SHUTDOWN, 0, "graceful");

    printf("Shutting down...\n");

    tor_integration_stop();

    if (svc->gen->running)
        gen_stop(svc->gen);

    rpc_http_stop();

    /* Stop file service */
    fs_server_stop();

    /* Save block index flat file for instant next restart */
    if (svc->state->map_block_index.size > 1000) {
        printf("Saving block index flat file (%zu entries)...\n",
               svc->state->map_block_index.size);
        save_block_index_flat(svc->datadir, svc->state);
    }

    /* Save peer addresses */
    addr_db_write(&svc->connman->manager, svc->datadir);

    /* Signal P2P threads to stop, then flush coins while threads wind down.
     * The message thread checks g_stop each iteration (~100ms). Any
     * in-flight activate_best_chain sees g_shutdown_requested and returns.
     * After signal_stop, no new block processing starts. */
    connman_signal_stop(svc->connman);

    /* Flush coins to SQLite. The message thread is finishing its current
     * iteration. If it was mid-connect_block, it already flushed via the
     * g_shutdown_requested handler in activate_best_chain. */
    printf("Flushing coins cache to SQLite...\n");
    if (coins_view_cache_flush(svc->coins_tip)) {
        printf("Coins cache flushed.\n");
    } else {
        fprintf(stderr, "WARNING: Coins cache flush FAILED during shutdown!\n");
    }

    /* Now join threads — safe, coins already persisted */
    connman_join(svc->connman);
    connman_free(svc->connman);

    /* Final flush in case message thread connected blocks between
     * our flush and its exit */
    coins_view_cache_flush(svc->coins_tip);
    coins_view_cache_free(svc->coins_tip);
    coins_view_sqlite_close(svc->coins_sqlite);

    /* Save MMR state */
    rpc_blockchain_mmr_save(g_active_node_db);

    if (svc->block_tree_open) {
        block_tree_db_close(svc->block_tree);
        svc->block_tree_open = false;
    }

    if (svc->wallet_sqlite->open) {
        wallet_sqlite_flush(svc->wallet_sqlite, svc->wallet);
        wallet_sqlite_close(svc->wallet_sqlite);
    }
    if (svc->node_db->open) {
        node_db_sync_flush(svc->node_db);
        node_db_sync_mempool_save(svc->node_db, svc->mempool);
        node_db_close(svc->node_db);
    }
    g_active_node_db = NULL;
    wallet_free(svc->wallet);
    tx_mempool_free(svc->mempool);
    main_state_free(svc->state);
    sapling_free_params();

    ecc_verify_destroy();
    ecc_stop();

    printf("Shutdown complete.\n");
}

/* ── Utility functions ─────────────────────────────────────── */

void app_add_node(const char *host, int port)
{
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);

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
                                 : S->connman->manager.default_port;

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
        connman_open_connection(S->connman, &addr);
    } else {
        printf("Failed to resolve addnode %s\n", hostbuf);
    }
}

void app_start_metrics(bool mining)
{
    S->metrics->ms = S->state;
    S->metrics->cm = S->connman;
    S->metrics->params = chain_params_get();
    S->metrics->mining = mining;
    metrics_start(S->metrics);
}

void app_stop_metrics(void)
{
    metrics_stop(S->metrics);
}
