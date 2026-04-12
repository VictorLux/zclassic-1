/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * SQLite ActiveRecord model tests for ZClassic C23. */

#include "test/test_helpers.h"
#include "controllers/snapshot_controller.h"
#include "controllers/sync_controller.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "wallet/wallet.h"
#include "script/standard.h"
#include "validation/chainstate.h"
#include "util/safe_alloc.h"
#include <unistd.h>

static struct transaction make_sync_test_tx(void)
{
    struct transaction tx;
    uint8_t sig[] = {0x00, 0x00};
    uint8_t pk[] = {0x76, 0xa9, 0x14};

    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.overwintered = false;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "test_tx_vin");
    memset(tx.vin[0].prevout.hash.data, 0xAA, 32);
    tx.vin[0].prevout.n = 0;
    script_set(&tx.vin[0].script_sig, sig, sizeof(sig));
    tx.vin[0].sequence = 0xFFFFFFFF;
    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "test_tx_vout");
    tx.vout[0].value = 50 * 100000000LL;
    script_set(&tx.vout[0].script_pub_key, pk, sizeof(pk));
    transaction_compute_hash(&tx);
    return tx;
}

static void free_sync_test_tx(struct transaction *tx)
{
    if (!tx)
        return;
    free(tx->vin);
    free(tx->vout);
    memset(tx, 0, sizeof(*tx));
}

static void runtime_set_db_service(struct app_runtime_context *runtime,
                                   struct db_service *svc)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->db_service = svc;
    app_runtime_set_current(runtime);
}

static void cleanup_temp_db_dir(const char *dir_path)
{
    char path[1024];

    if (!dir_path || !dir_path[0])
        return;
    snprintf(path, sizeof(path), "%s/node.db-wal", dir_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/node.db-shm", dir_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/node.db", dir_path);
    unlink(path);
    rmdir(dir_path);
}

static bool test_db_service_write_callback(struct node_db *ndb, void *ctx)
{
    bool *ran = ctx;

    if (ran)
        *ran = true;
    return node_db_exec(ndb,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('db_service_callback', X'02')");
}

static bool test_db_service_nested_write_callback(struct node_db *ndb, void *ctx)
{
    struct db_service *svc = ctx;

    if (!ndb || !svc)
        return false;
    if (!db_service_is_worker_thread(svc))
        return false;
    if (!db_service_begin_write(svc))
        return false;
    if (!db_service_exec_write(svc,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('db_service_nested', X'03')")) {
        db_service_rollback_write(svc);
        return false;
    }
    if (!db_service_commit_write(svc)) {
        db_service_rollback_write(svc);
        return false;
    }
    return true;
}

int test_sqlite(void) {
    int failures = 0;

    /* DB open/close and schema creation */
    {
        printf("SQLite DB open/close... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        if (ok) {
            int ver = node_db_schema_version(&ndb);
            ok = ok && (ver == NODE_DB_SCHEMA_LATEST);
            node_db_close(&ndb);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB state key-value store */
    {
        printf("SQLite state set/get... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        int64_t val = 0;
        ok = ok && node_db_state_set_int(&ndb, "tip_height", 3034538);
        ok = ok && node_db_state_get_int(&ndb, "tip_height", &val);
        ok = ok && (val == 3034538);

        uint8_t blob[32] = {0xde, 0xad, 0xbe, 0xef};
        ok = ok && node_db_state_set(&ndb, "best_hash", blob, 32);
        uint8_t got[32];
        size_t got_len = 0;
        ok = ok && node_db_state_get(&ndb, "best_hash", got, 32, &got_len);
        ok = ok && (got_len == 32) && (got[0] == 0xde);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB runtime status reporting */
    {
        printf("SQLite runtime status tracks tx/turbo/checkpoint state... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct node_db_status st = {0};

        ok = ok && ndb.open;
        node_db_get_status(&ndb, &st);
        ok = ok && st.open;
        ok = ok && !st.tx_open;
        ok = ok && !st.turbo_mode;
        ok = ok && strcmp(st.last_op, "open") == 0;

        ok = ok && node_db_begin(&ndb);
        node_db_get_status(&ndb, &st);
        ok = ok && st.tx_open;
        ok = ok && strcmp(st.last_op, "BEGIN TRANSACTION") == 0;

        ok = ok && node_db_commit(&ndb);
        node_db_get_status(&ndb, &st);
        ok = ok && !st.tx_open;
        ok = ok && strcmp(st.last_op, "COMMIT") == 0;

        ok = ok && node_db_ibd_turbo_mode(&ndb);
        node_db_get_status(&ndb, &st);
        ok = ok && st.turbo_mode;
        ok = ok && strcmp(st.last_op, "ibd_turbo_mode") == 0;

        ok = ok && node_db_wal_checkpoint(&ndb);
        node_db_get_status(&ndb, &st);
        ok = ok && strcmp(st.last_op, "wal_checkpoint") == 0;
        ok = ok && st.last_sqlite_rc == SQLITE_OK;

        ok = ok && node_db_normal_mode(&ndb);
        node_db_get_status(&ndb, &st);
        ok = ok && !st.turbo_mode;
        ok = ok && strcmp(st.last_op, "normal_mode") == 0;

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB service wrapper */
    {
        printf("SQLite DB service attaches and gates node_db access... ");
        struct node_db ndb;
        struct db_service svc;
        bool ok = node_db_open(&ndb, ":memory:");
        db_service_init(&svc);
        ok = ok && !db_service_is_started(&svc);
        ok = ok && db_service_node_db(&svc) == NULL;
        ok = ok && db_service_attach(&svc, &ndb);
        ok = ok && db_service_start(&svc);
        ok = ok && db_service_is_started(&svc);
        ok = ok && db_service_node_db(&svc) == &ndb;
        ok = ok && db_service_query_db(&svc) != NULL;
        {
            struct db_service_status svc_status = {0};
            db_service_get_status(&svc, &svc_status);
            ok = ok && svc_status.started;
            ok = ok && svc_status.worker_started;
            ok = ok && !svc_status.stop_requested;
            ok = ok && svc_status.queue_depth == 0;
        }
        db_service_stop(&svc);
        ok = ok && !db_service_is_started(&svc);
        ok = ok && db_service_node_db(&svc) == NULL;
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB service write wrappers */
    {
        printf("SQLite DB service write wrappers forward safely... ");
        struct node_db ndb;
        struct db_service svc;
        struct node_db_status st = {0};
        bool ok = node_db_open(&ndb, ":memory:");
        db_service_init(&svc);
        ok = ok && db_service_attach(&svc, &ndb);
        ok = ok && db_service_start(&svc);
        ok = ok && db_service_begin_write(&svc);
        node_db_get_status(&ndb, &st);
        ok = ok && st.tx_open;
        ok = ok && db_service_exec_write(&svc,
            "INSERT OR REPLACE INTO node_state(key,value)"
            " VALUES('db_service_test', X'01')");
        ok = ok && db_service_commit_write(&svc);
        node_db_get_status(&ndb, &st);
        ok = ok && !st.tx_open;
        ok = ok && db_service_flush_write(&svc);
        db_service_stop(&svc);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB service runtime mode helpers */
    {
        printf("SQLite DB service runtime mode helpers update DB state... ");
        struct node_db ndb;
        struct db_service svc;
        struct node_db_status st = {0};
        bool ok = node_db_open(&ndb, ":memory:");
        db_service_init(&svc);
        ok = ok && db_service_attach(&svc, &ndb);
        ok = ok && db_service_start(&svc);
        ok = ok && db_service_ibd_turbo_mode(&svc);
        ok = ok && db_service_set_sync_batch_size(&svc, 250);
        node_db_get_status(&ndb, &st);
        ok = ok && st.turbo_mode;
        ok = ok && strcmp(st.last_op, "set_sync_batch_size") == 0;
        ok = ok && db_service_wal_checkpoint(&svc);
        ok = ok && db_service_normal_mode(&svc);
        node_db_get_status(&ndb, &st);
        ok = ok && !st.turbo_mode;
        ok = ok && strcmp(st.last_op, "normal_mode") == 0;
        db_service_stop(&svc);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB service callback writes */
    {
        printf("SQLite DB service callback runs whole write on worker... ");
        struct node_db ndb;
        struct db_service svc;
        bool ran = false;
        uint8_t got[8] = {0};
        size_t got_len = 0;
        bool ok = node_db_open(&ndb, ":memory:");
        db_service_init(&svc);
        ok = ok && db_service_attach(&svc, &ndb);
        ok = ok && db_service_start(&svc);
        ok = ok && db_service_run_write(&svc,
            test_db_service_write_callback, &ran);
        ok = ok && ran;
        ok = ok && node_db_state_get(&ndb, "db_service_callback",
                                     got, sizeof(got), &got_len);
        ok = ok && got_len == 1 && got[0] == 0x02;
        db_service_stop(&svc);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB service nested worker writes */
    {
        printf("SQLite DB service nested worker writes stay reentrant... ");
        struct node_db ndb;
        struct db_service svc;
        uint8_t got[8] = {0};
        size_t got_len = 0;
        bool ok = node_db_open(&ndb, ":memory:");
        db_service_init(&svc);
        ok = ok && db_service_attach(&svc, &ndb);
        ok = ok && db_service_start(&svc);
        ok = ok && db_service_run_write(&svc,
            test_db_service_nested_write_callback, &svc);
        ok = ok && node_db_state_get(&ndb, "db_service_nested",
                                     got, sizeof(got), &got_len);
        ok = ok && got_len == 1 && got[0] == 0x03;
        db_service_stop(&svc);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    {
        printf("SQLite sync_controller opens private file-backed DB handles... ");
        char dir_template[] = "/tmp/zclassic23-private-db-XXXXXX";
        char *dir_path = mkdtemp(dir_template);
        char db_path[1024];
        struct node_db ndb;
        struct node_db private_db;
        int64_t value = 0;
        bool ok = dir_path != NULL;

        memset(&ndb, 0, sizeof(ndb));
        memset(&private_db, 0, sizeof(private_db));
        if (ok) {
            snprintf(db_path, sizeof(db_path), "%s/node.db", dir_path);
            ok = node_db_open(&ndb, db_path);
        }
        if (ok)
            ok = node_db_sync_open_private_db_like(&ndb, &private_db);
        if (ok) {
            ok = private_db.open;
            ok = ok && private_db.db != ndb.db;
            ok = ok && node_db_state_set_int(&private_db,
                                             "private_handle_test", 77);
            ok = ok && node_db_state_get_int(&ndb,
                                             "private_handle_test", &value);
            ok = ok && value == 77;
        }

        if (private_db.open)
            node_db_close(&private_db);
        if (ndb.open)
            node_db_close(&ndb);
        cleanup_temp_db_dir(dir_path);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    {
        printf("SQLite snapshot tx-index job starts and joins cleanly... ");
        char dir_template[] = "/tmp/zclassic23-tx-index-job-XXXXXX";
        char *dir_path = mkdtemp(dir_template);
        char db_path[1024];
        struct node_db ndb;
        struct snapshot_tx_index_job job;
        int result = -1;
        bool ok = dir_path != NULL;

        memset(&ndb, 0, sizeof(ndb));
        snapshot_tx_index_job_init(&job);
        ok = ok && !snapshot_tx_index_job_is_started(&job);
        if (ok) {
            snprintf(db_path, sizeof(db_path), "%s/node.db", dir_path);
            ok = node_db_open(&ndb, db_path);
        }
        if (ok) {
            node_db_close(&ndb);
            ok = snapshot_tx_index_job_start(&job, dir_path);
        }
        if (ok)
            ok = snapshot_tx_index_job_is_started(&job);
        if (ok)
            ok = snapshot_tx_index_job_join(&job, &result);
        ok = ok && !snapshot_tx_index_job_is_started(&job);
        ok = ok && result == 0;

        if (ndb.open)
            node_db_close(&ndb);
        cleanup_temp_db_dir(dir_path);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    {
        printf("SQLite sync job wrappers fail closed and fallback cleanly... ");
        struct node_db ndb;
        struct active_chain ac;
        struct node_db_sync_catchup_job catch_job;
        struct node_db_sync_import_job import_job;
        struct coins_view_db cvdb = {0};
        int result = -1;
        bool ok = node_db_open(&ndb, ":memory:");

        active_chain_init(&ac);
        node_db_sync_catchup_job_init(&catch_job);
        node_db_sync_import_job_init(&import_job);

        ok = ok && !node_db_sync_catchup_job_start(NULL, &ndb, &ac, NULL, NULL);
        ok = ok && !node_db_sync_catchup_job_start(&catch_job, NULL, &ac, NULL, NULL);
        ok = ok && !node_db_sync_catchup_job_start(&catch_job, &ndb, NULL, NULL, NULL);
        ok = ok && !node_db_sync_import_job_start(NULL, &ndb, &cvdb);
        ok = ok && !node_db_sync_import_job_start(&import_job, &ndb, NULL);
        ok = ok && !node_db_sync_catchup_job_join(&catch_job, NULL);
        ok = ok && !node_db_sync_import_job_join(&import_job, NULL);

        if (node_db_sync_catchup_job_start(&catch_job, &ndb, &ac, NULL, NULL))
            ok = ok && node_db_sync_catchup_job_join(&catch_job, &result) && result == 0;

        ok = ok && !node_db_sync_catchup_job_is_started(&catch_job);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }

        active_chain_free(&ac);
        node_db_close(&ndb);
    }

    /* sync_controller DB-service wrappers */
    {
        printf("SQLite sync_controller wrappers use runtime DB service... ");
        struct node_db ndb;
        struct db_service svc;
        struct app_runtime_context runtime;
        struct transaction tx;
        struct transaction wallet_tx;
        struct wallet *wallet = NULL;
        struct privkey key;
        struct pubkey pubkey;
        struct key_id kid;
        struct tx_destination dest;
        struct db_wallet_utxo wallet_utxo = {0};
        struct db_wallet_tx saved_wallet_tx = {0};
        struct db_peer peer = {0};
        struct db_sapling_note note = {0};
        uint8_t tip_hash[32];
        uint8_t got_tip_hash[32];
        size_t got_tip_len = 0;
        uint8_t nullifier[32];
        uint8_t spending_txid[32];
        int64_t tip_height = -1;
        bool ok = node_db_open(&ndb, ":memory:");

        db_service_init(&svc);
        ok = ok && db_service_attach(&svc, &ndb);
        ok = ok && db_service_start(&svc);
        runtime_set_db_service(&runtime, &svc);
        wallet = zcl_calloc(1, sizeof(*wallet), "test_wallet");
        ok = ok && wallet != NULL;

        tx = make_sync_test_tx();
        wallet_tx = make_sync_test_tx();
        ok = ok && node_db_sync_mempool_add(&ndb, &tx, 1234, 777);
        ok = ok && (db_mempool_count(&ndb) == 1);
        ok = ok && node_db_sync_mempool_remove(&ndb, tx.hash.data);
        ok = ok && (db_mempool_count(&ndb) == 0);

        memset(tip_hash, 0xAB, sizeof(tip_hash));
        ok = ok && node_db_sync_set_tip(&ndb, tip_hash, 12345);
        ok = ok && node_db_state_get_int(&ndb, "tip_height", &tip_height);
        ok = ok && (tip_height == 12345);
        ok = ok && node_db_state_get(&ndb, "tip_hash",
                                     got_tip_hash, sizeof(got_tip_hash),
                                     &got_tip_len);
        ok = ok && got_tip_len == sizeof(got_tip_hash);
        ok = ok && memcmp(got_tip_hash, tip_hash, sizeof(tip_hash)) == 0;

        memset(peer.ip, 0, sizeof(peer.ip));
        peer.ip[10] = 0xFF;
        peer.ip[11] = 0xFF;
        peer.ip[12] = 127;
        peer.ip[15] = 1;
        ok = ok && node_db_sync_peer(&ndb, peer.ip, 8033, 9, 1700000000);
        ok = ok && db_peer_find_by_addr(&ndb, peer.ip, 8033, &peer);
        ok = ok && peer.services == 9;
        ok = ok && node_db_sync_peer_score(&ndb, peer.ip, 8033, 44, true);
        ok = ok && db_peer_find_by_addr(&ndb, peer.ip, 8033, &peer);
        ok = ok && peer.bandwidth_score == 44;
        ok = ok && peer.is_zcl23;

        if (wallet)
            wallet_init(wallet);
        privkey_make_new(&key, true);
        ok = ok && privkey_get_pubkey(&key, &pubkey);
        ok = ok && wallet && keystore_add_key(&wallet->keystore, &key);
        kid = pubkey_get_id(&pubkey);
        dest.type = DEST_KEY_ID;
        dest.id.key = kid;
        script_for_destination(&wallet_tx.vout[0].script_pub_key, &dest);
        transaction_compute_hash(&wallet_tx);
        ok = ok && wallet &&
             node_db_sync_wallet_tx(&ndb, &wallet_tx, wallet, 0);
        ok = ok && db_wallet_utxo_find(&ndb, wallet_tx.hash.data, 0, &wallet_utxo);
        ok = ok && wallet_utxo.value == wallet_tx.vout[0].value;
        ok = ok && db_wallet_tx_find(&ndb, wallet_tx.hash.data, &saved_wallet_tx);
        ok = ok && !saved_wallet_tx.has_block;
        ok = ok && saved_wallet_tx.block_height == 0;
        free(wallet_utxo.script);
        free(saved_wallet_tx.raw_tx);
        if (wallet) {
            wallet_free(wallet);
            free(wallet);
        }

        memset(&note, 0, sizeof(note));
        memset(note.txid, 0x01, sizeof(note.txid));
        note.output_index = 2;
        note.value = 4200;
        memset(note.rcm, 0x02, sizeof(note.rcm));
        memset(note.ivk, 0x03, sizeof(note.ivk));
        memset(note.diversifier, 0x04, sizeof(note.diversifier));
        memset(note.pk_d, 0x05, sizeof(note.pk_d));
        memset(note.cm, 0x06, sizeof(note.cm));
        memset(note.nullifier, 0x07, sizeof(note.nullifier));
        note.block_height = 88;
        ok = ok && node_db_sync_sapling_note(&ndb,
                                             note.txid,
                                             note.output_index,
                                             note.value,
                                             note.rcm,
                                             NULL,
                                             0,
                                             note.ivk,
                                             note.diversifier,
                                             note.pk_d,
                                             note.cm,
                                             note.nullifier,
                                             note.block_height);
        ok = ok && db_sapling_note_balance_for_ivk(&ndb, note.ivk) == note.value;
        memcpy(nullifier, note.nullifier, sizeof(nullifier));
        memset(spending_txid, 0x08, sizeof(spending_txid));
        ok = ok && node_db_sync_sapling_spend(&ndb, nullifier, spending_txid);
        ok = ok && db_sapling_note_balance_for_ivk(&ndb, note.ivk) == 0;

        app_runtime_set_current(NULL);
        db_service_stop(&svc);
        free_sync_test_tx(&tx);
        free_sync_test_tx(&wallet_tx);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB block CRUD */
    {
        printf("SQLite block save/find... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.hash, 0xAA, 32);
        blk.height = 100;
        memset(blk.prev_hash, 0xBB, 32);
        blk.version = 4;
        memset(blk.merkle_root, 0xCC, 32);
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        memset(blk.nonce, 0xDD, 32);
        uint8_t sol[] = {0x01, 0x02, 0x03};
        blk.solution = sol;
        blk.solution_len = 3;
        memset(blk.chain_work, 0xEE, 32);
        blk.status = 5;
        blk.file_num = 1;
        blk.data_pos = 8192;
        blk.num_tx = 42;

        ok = ok && db_block_save(&ndb, &blk);
        ok = ok && (db_block_count(&ndb) == 1);
        ok = ok && (db_block_max_height(&ndb) == 100);

        struct db_block found;
        ok = ok && db_block_find_by_hash(&ndb, blk.hash, &found);
        ok = ok && (found.height == 100);
        ok = ok && (found.num_tx == 42);
        ok = ok && (found.file_num == 1);

        ok = ok && db_block_find_by_height(&ndb, 100, &found);
        ok = ok && (memcmp(found.hash, blk.hash, 32) == 0);

        ok = ok && db_block_delete(&ndb, blk.hash);
        ok = ok && (db_block_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB transaction index CRUD */
    {
        printf("SQLite tx index save/find... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_tx_index tx;
        memset(&tx, 0, sizeof(tx));
        memset(tx.txid, 0x11, 32);
        memset(tx.block_hash, 0x22, 32);
        tx.block_height = 500;
        tx.tx_index = 3;
        tx.file_num = 2;
        tx.file_pos = 16384;
        tx.is_coinbase = true;

        ok = ok && db_tx_save(&ndb, &tx);

        struct db_tx_index found;
        ok = ok && db_tx_find(&ndb, tx.txid, &found);
        ok = ok && (found.block_height == 500);
        ok = ok && (found.tx_index == 3);
        ok = ok && found.is_coinbase;

        ok = ok && db_tx_delete(&ndb, tx.txid);
        ok = ok && !db_tx_find(&ndb, tx.txid, &found);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB UTXO CRUD and balance */
    {
        printf("SQLite UTXO save/find/balance... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t script[] = {0x76, 0xa9, 0x14};
        uint8_t addr[20];
        memset(addr, 0x42, 20);

        struct db_utxo u1 = {
            .vout = 0, .value = 50000000,
            .script = script, .script_len = 3,
            .script_type = SCRIPT_P2PKH,
            .has_address = true, .height = 100,
            .is_coinbase = false
        };
        memset(u1.txid, 0xAA, 32);
        memcpy(u1.address_hash, addr, 20);

        struct db_utxo u2 = {
            .vout = 1, .value = 30000000,
            .script = script, .script_len = 3,
            .script_type = SCRIPT_P2PKH,
            .has_address = true, .height = 101,
            .is_coinbase = false
        };
        memset(u2.txid, 0xBB, 32);
        memcpy(u2.address_hash, addr, 20);

        ok = ok && db_utxo_save(&ndb, &u1);
        ok = ok && db_utxo_save(&ndb, &u2);
        ok = ok && (db_utxo_count(&ndb) == 2);
        ok = ok && db_utxo_exists(&ndb, u1.txid, 0);
        ok = ok && !db_utxo_exists(&ndb, u1.txid, 1);

        int64_t bal = db_utxo_balance_for_address(&ndb, addr);
        ok = ok && (bal == 80000000);

        ok = ok && db_utxo_delete(&ndb, u1.txid, 0);
        ok = ok && (db_utxo_count(&ndb) == 1);
        bal = db_utxo_balance_for_address(&ndb, addr);
        ok = ok && (bal == 30000000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB wallet key CRUD */
    {
        printf("SQLite wallet key save/find... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_wallet_key k;
        memset(&k, 0, sizeof(k));
        memset(k.pubkey_hash, 0x11, 20);
        memset(k.pubkey, 0x02, 33);
        k.pubkey_len = 33;
        memset(k.privkey, 0xFF, 32);
        k.compressed = true;
        k.created_at = 1700000000;

        ok = ok && db_wallet_key_save(&ndb, &k);
        ok = ok && db_wallet_key_exists(&ndb, k.pubkey_hash);
        ok = ok && (db_wallet_key_count(&ndb) == 1);

        struct db_wallet_key found;
        ok = ok && db_wallet_key_find(&ndb, k.pubkey_hash, &found);
        ok = ok && found.compressed;
        ok = ok && (found.pubkey_len == 33);
        ok = ok && (memcmp(found.privkey, k.privkey, 32) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB wallet transaction paging */
    {
        printf("SQLite wallet tx list/find... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t raw1[] = {0x01};
        uint8_t raw2[] = {0x02, 0x03};
        uint8_t raw3[] = {0x04, 0x05, 0x06};

        struct db_wallet_tx t1;
        memset(&t1, 0, sizeof(t1));
        memset(t1.txid, 0xA1, 32);
        t1.raw_tx = raw1;
        t1.raw_tx_len = sizeof(raw1);
        t1.time_received = 100;

        struct db_wallet_tx t2;
        memset(&t2, 0, sizeof(t2));
        memset(t2.txid, 0xB2, 32);
        t2.raw_tx = raw2;
        t2.raw_tx_len = sizeof(raw2);
        memset(t2.block_hash, 0x22, 32);
        t2.has_block = true;
        t2.block_height = 10;
        t2.time_received = 300;
        t2.from_me = true;
        t2.fee = 1234;

        struct db_wallet_tx t3;
        memset(&t3, 0, sizeof(t3));
        memset(t3.txid, 0xC3, 32);
        t3.raw_tx = raw3;
        t3.raw_tx_len = sizeof(raw3);
        t3.time_received = 200;

        ok = ok && db_wallet_tx_save(&ndb, &t1);
        ok = ok && db_wallet_tx_save(&ndb, &t2);
        ok = ok && db_wallet_tx_save(&ndb, &t3);
        ok = ok && (db_wallet_tx_count(&ndb) == 3);

        struct db_wallet_tx rows[2];
        memset(rows, 0, sizeof(rows));
        int n = db_wallet_tx_list(&ndb, rows, 2, 0);
        ok = ok && (n == 2);
        ok = ok && (rows[0].time_received == 300);
        ok = ok && (rows[1].time_received == 200);
        ok = ok && rows[0].from_me;
        ok = ok && (rows[0].fee == 1234);
        ok = ok && rows[0].has_block;
        ok = ok && (rows[0].block_height == 10);
        db_wallet_tx_free(&rows[0]);
        db_wallet_tx_free(&rows[1]);

        memset(rows, 0, sizeof(rows));
        n = db_wallet_tx_list(&ndb, rows, 1, 2);
        ok = ok && (n == 1);
        ok = ok && (rows[0].time_received == 100);
        db_wallet_tx_free(&rows[0]);

        struct db_wallet_tx found;
        ok = ok && db_wallet_tx_find(&ndb, t2.txid, &found);
        ok = ok && found.from_me;
        ok = ok && (found.fee == 1234);
        ok = ok && found.has_block;
        ok = ok && (memcmp(found.block_hash, t2.block_hash, 32) == 0);
        db_wallet_tx_free(&found);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB wallet UTXO and balance */
    {
        printf("SQLite wallet UTXO balance/spend... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t script[] = {0x76, 0xa9};
        struct db_wallet_utxo u1;
        memset(&u1, 0, sizeof(u1));
        memset(u1.txid, 0xAA, 32);
        u1.vout = 0;
        u1.value = 100000000;
        memset(u1.address_hash, 0x42, 20);
        u1.script = script;
        u1.script_len = 2;
        u1.height = 500;

        struct db_wallet_utxo u2;
        memset(&u2, 0, sizeof(u2));
        memset(u2.txid, 0xBB, 32);
        u2.vout = 0;
        u2.value = 50000000;
        memcpy(u2.address_hash, u1.address_hash, 20);
        u2.script = script;
        u2.script_len = 2;
        u2.height = 501;

        ok = ok && db_wallet_utxo_save(&ndb, &u1);
        ok = ok && db_wallet_utxo_save(&ndb, &u2);

        int64_t bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 150000000);

        /* Spend one UTXO */
        uint8_t spending_tx[32];
        memset(spending_tx, 0xCC, 32);
        ok = ok && db_wallet_utxo_mark_spent(&ndb, u1.txid, 0,
                                              spending_tx, 0);

        bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 50000000);

        /* List unspent */
        struct db_wallet_utxo unspent[10];
        int n = db_wallet_utxo_list_unspent(&ndb, unspent, 10);
        ok = ok && (n == 1);
        ok = ok && (unspent[0].value == 50000000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB Sapling note balance */
    {
        printf("SQLite Sapling note save/balance... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_sapling_note n1;
        memset(&n1, 0, sizeof(n1));
        memset(n1.txid, 0xAA, 32);
        n1.output_index = 0;
        n1.value = 200000000;
        memset(n1.rcm, 0x11, 32);
        memset(n1.ivk, 0x22, 32);
        memset(n1.diversifier, 0x33, 11);
        memset(n1.pk_d, 0x44, 32);
        memset(n1.cm, 0x55, 32);
        memset(n1.nullifier, 0x66, 32);
        n1.block_height = 1000;

        ok = ok && db_sapling_note_save(&ndb, &n1);

        int64_t bal = db_sapling_note_balance(&ndb);
        ok = ok && (bal == 200000000);

        bal = db_sapling_note_balance_for_ivk(&ndb, n1.ivk);
        ok = ok && (bal == 200000000);

        /* Mark spent via nullifier */
        uint8_t spent_by[32];
        memset(spent_by, 0x77, 32);
        ok = ok && db_sapling_note_mark_spent(&ndb, n1.nullifier, spent_by);

        bal = db_sapling_note_balance(&ndb);
        ok = ok && (bal == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB mempool persistence */
    {
        printf("SQLite mempool save/find/clear... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t raw[] = {0x01, 0x00, 0x00, 0x00};
        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        memset(e.txid, 0xAA, 32);
        e.raw_tx = raw;
        e.raw_tx_len = 4;
        e.fee = 10000;
        e.size = 250;
        e.time_added = 1700000000;
        e.height_added = 500;

        ok = ok && db_mempool_save(&ndb, &e);
        ok = ok && (db_mempool_count(&ndb) == 1);

        /* Add a spend record */
        uint8_t spent_txid[32];
        memset(spent_txid, 0xBB, 32);
        ok = ok && db_mempool_add_spend(&ndb, e.txid, spent_txid, 0);
        ok = ok && db_mempool_is_spent(&ndb, spent_txid, 0);
        ok = ok && !db_mempool_is_spent(&ndb, spent_txid, 1);

        ok = ok && db_mempool_clear(&ndb);
        ok = ok && (db_mempool_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB peer storage */
    {
        printf("SQLite peer save/find/recent... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_peer p;
        memset(&p, 0, sizeof(p));
        p.ip[10] = 0xFF; p.ip[11] = 0xFF;
        p.ip[12] = 127; p.ip[13] = 0; p.ip[14] = 0; p.ip[15] = 1;
        p.port = 8033;
        p.services = 1;
        p.last_seen = 1700000000;

        ok = ok && db_peer_save(&ndb, &p);
        ok = ok && (db_peer_count(&ndb) == 1);

        struct db_peer found;
        ok = ok && db_peer_find_by_addr(&ndb, p.ip, p.port, &found);
        ok = ok && (found.port == 8033);
        ok = ok && (found.services == 1);

        ok = ok && db_peer_mark_tried(&ndb, p.ip, p.port);
        ok = ok && db_peer_find_by_addr(&ndb, p.ip, p.port, &found);
        ok = ok && (found.attempts == 1);

        ok = ok && db_peer_mark_seen(&ndb, p.ip, p.port, 1700000100);
        ok = ok && db_peer_find_by_addr(&ndb, p.ip, p.port, &found);
        ok = ok && (found.attempts == 0);
        ok = ok && (found.last_seen == 1700000100);

        struct db_peer recent[10];
        int n = db_peer_recent(&ndb, recent, 10);
        ok = ok && (n == 1);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB transaction batching */
    {
        printf("SQLite batch insert with begin/commit... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        ok = ok && node_db_begin(&ndb);
        for (int i = 0; i < 100; i++) {
            struct db_tx_index tx;
            memset(&tx, 0, sizeof(tx));
            memset(tx.txid, 0x11, 32);
            tx.txid[0] = (uint8_t)((i >> 8) + 1);
            tx.txid[1] = (uint8_t)((i & 0xFF) + 1);
            memset(tx.block_hash, 0x22, 32);
            tx.block_height = i;
            tx.tx_index = 0;
            tx.file_num = 0;
            tx.file_pos = i * 1000;
            ok = ok && db_tx_save(&ndb, &tx);
        }
        ok = ok && node_db_commit(&ndb);

        /* Verify all 100 were inserted */
        struct db_tx_index found;
        uint8_t lookup[32];
        memset(lookup, 0x11, 32);
        lookup[0] = 1;
        lookup[1] = 51; /* i=50: (50>>8)+1=1, (50&0xFF)+1=51 */
        ok = ok && db_tx_find(&ndb, lookup, &found);
        ok = ok && (found.block_height == 50);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB sapling key CRUD */
    {
        printf("SQLite Sapling key save/find... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_sapling_key k;
        memset(&k, 0, sizeof(k));
        memset(k.ivk, 0x11, 32);
        memset(k.xsk, 0x22, 169);
        memset(k.xfvk, 0x33, 169);
        memset(k.diversifier, 0x44, 11);
        memset(k.pk_d, 0x55, 32);
        k.child_index = 0;
        snprintf(k.address, sizeof(k.address), "zs1testaddr");

        ok = ok && db_sapling_key_save(&ndb, &k);
        ok = ok && (db_sapling_key_count(&ndb) == 1);

        struct db_sapling_key found;
        ok = ok && db_sapling_key_find_by_ivk(&ndb, k.ivk, &found);
        ok = ok && (found.child_index == 0);
        ok = ok && (strcmp(found.address, "zs1testaddr") == 0);

        ok = ok && db_sapling_key_find_by_address(&ndb, "zs1testaddr", &found);
        ok = ok && (memcmp(found.ivk, k.ivk, 32) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* DB wallet seed singleton */
    {
        printf("SQLite wallet seed save/load... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t seed[32];
        memset(seed, 0xAB, 32);
        ok = ok && db_wallet_seed_save(&ndb, seed, 5);

        uint8_t loaded[32];
        uint32_t next = 0;
        ok = ok && db_wallet_seed_load(&ndb, loaded, &next);
        ok = ok && (memcmp(loaded, seed, 32) == 0);
        ok = ok && (next == 5);

        /* Update */
        ok = ok && db_wallet_seed_save(&ndb, seed, 10);
        ok = ok && db_wallet_seed_load(&ndb, loaded, &next);
        ok = ok && (next == 10);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Wallet UTXO lifecycle: save, balance, mark spent, balance ── */
    {
        printf("SQLite wallet UTXO lifecycle... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t script[] = {0x76, 0xa9, 0x14};
        uint8_t addr[20];
        memset(addr, 0x42, 20);

        /* Create two wallet UTXOs */
        struct db_wallet_utxo wu1;
        memset(&wu1, 0, sizeof(wu1));
        memset(wu1.txid, 0xAA, 32);
        wu1.vout = 0;
        wu1.value = 50000000; /* 0.5 ZCL */
        memcpy(wu1.address_hash, addr, 20);
        wu1.script = script;
        wu1.script_len = 3;
        wu1.height = 100;
        wu1.is_coinbase = false;

        struct db_wallet_utxo wu2;
        memset(&wu2, 0, sizeof(wu2));
        memset(wu2.txid, 0xBB, 32);
        wu2.vout = 0;
        wu2.value = 48000000; /* 0.48 ZCL */
        memcpy(wu2.address_hash, addr, 20);
        wu2.script = script;
        wu2.script_len = 3;
        wu2.height = 101;
        wu2.is_coinbase = false;

        ok = ok && db_wallet_utxo_save(&ndb, &wu1);
        ok = ok && db_wallet_utxo_save(&ndb, &wu2);

        /* Balance = 0.98 ZCL unspent */
        int64_t bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 98000000);

        /* Mark one as spent */
        uint8_t spent_by[32];
        memset(spent_by, 0xCC, 32);
        ok = ok && db_wallet_utxo_mark_spent(&ndb, wu1.txid, 0,
                                              spent_by, 0);

        /* Balance = 0.48 ZCL */
        bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 48000000);

        /* INSERT OR REPLACE: re-save wu1 should reset spent state */
        ok = ok && db_wallet_utxo_save(&ndb, &wu1);
        bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 98000000); /* back to 0.98 */

        /* Delete one */
        ok = ok && db_wallet_utxo_delete(&ndb, wu2.txid, 0);
        bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 50000000); /* only wu1 */

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── UTXO value CHECK constraint ── */
    {
        printf("SQLite UTXO CHECK constraint (negative value rejected)... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t script[] = {0x76, 0xa9, 0x14};
        struct db_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0xDD, 32);
        u.vout = 0;
        u.value = -1; /* negative — must be rejected */
        u.script = script;
        u.script_len = 3;
        u.script_type = SCRIPT_P2PKH;
        u.height = 1;

        /* Should fail: validation rejects negative value */
        bool saved = db_utxo_save(&ndb, &u);
        ok = ok && !saved;

        /* MAX_MONEY + 1 should also fail */
        u.value = 2100000000000001LL;
        saved = db_utxo_save(&ndb, &u);
        ok = ok && !saved;

        /* Exactly MAX_MONEY should succeed */
        u.value = 2100000000000000LL;
        saved = db_utxo_save(&ndb, &u);
        ok = ok && saved;

        /* Normal value should succeed */
        u.value = 100000000; /* 1 ZCL */
        memset(u.txid, 0xEE, 32);
        saved = db_utxo_save(&ndb, &u);
        ok = ok && saved;

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Cross-validation: global UTXO vs wallet UTXO balance ── */
    {
        printf("SQLite cross-validate global vs wallet balance... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t addr[20], script[] = {0x76, 0xa9, 0x14};
        memset(addr, 0x42, 20);

        /* Add UTXO to global table */
        struct db_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0xAA, 32);
        u.vout = 0;
        u.value = 98000000; /* 0.98 ZCL */
        u.script = script;
        u.script_len = 3;
        u.script_type = SCRIPT_P2PKH;
        u.has_address = true;
        memcpy(u.address_hash, addr, 20);
        u.height = 100;
        ok = ok && db_utxo_save(&ndb, &u);

        /* Add same UTXO to wallet table */
        struct db_wallet_utxo wu;
        memset(&wu, 0, sizeof(wu));
        memcpy(wu.txid, u.txid, 32);
        wu.vout = 0;
        wu.value = 98000000;
        memcpy(wu.address_hash, addr, 20);
        wu.script = script;
        wu.script_len = 3;
        wu.height = 100;
        ok = ok && db_wallet_utxo_save(&ndb, &wu);

        /* Both should report same balance */
        int64_t global_bal = db_utxo_balance_for_address(&ndb, addr);
        int64_t wallet_bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (global_bal == 98000000);
        ok = ok && (wallet_bal == 98000000);
        ok = ok && (global_bal == wallet_bal);

        /* Mark wallet UTXO spent — wallet should be 0, global unchanged */
        uint8_t spent_by[32];
        memset(spent_by, 0xCC, 32);
        ok = ok && db_wallet_utxo_mark_spent(&ndb, wu.txid, 0,
                                              spent_by, 0);
        wallet_bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (wallet_bal == 0);
        global_bal = db_utxo_balance_for_address(&ndb, addr);
        ok = ok && (global_bal == 98000000); /* global unchanged */

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
