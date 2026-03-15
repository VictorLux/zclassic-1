/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * SQLite ActiveRecord model tests for ZClassic C23. */

#include "test/test_helpers.h"

int test_sqlite(void) {
    int failures = 0;

    /* DB open/close and schema creation */
    {
        printf("SQLite DB open/close... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        if (ok) {
            int ver = node_db_schema_version(&ndb);
            ok = ok && (ver == 1);
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

    return failures;
}
