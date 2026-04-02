/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Focused ZSLP model tests. */

#include "test/test_helpers.h"
#include <unistd.h>

int test_model_zslp(void)
{
    int failures = 0;

    printf("ZSLP balance validates token_id and address... ");
    {
        struct db_zslp_balance bal;
        struct ar_errors e;
        memset(&bal, 0, sizeof(bal));
        bal.balance = 5;
        ar_errors_clear(&e);
        db_zslp_balance_validate(&bal, &e);
        if (ar_errors_any(&e)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ZSLP balance save normalizes token key before save... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_models_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            struct db_zslp_balance bal;
            struct db_zslp_balance got;
            memset(&bal, 0, sizeof(bal));
            memset(&got, 0, sizeof(got));
            snprintf(bal.token_id, sizeof(bal.token_id), "%s", "testcoin");
            snprintf(bal.address, sizeof(bal.address), "%s", "t1Buyer123");
            bal.balance = 42;
            ok = db_zslp_balance_save(&ndb, &bal);
            ok = ok && (strcmp(bal.token_id, "TESTCOIN") == 0);
            ok = ok && db_zslp_balance_find(&ndb, "testcoin", "t1Buyer123", &got);
            ok = ok && (strcmp(got.token_id, "TESTCOIN") == 0);
            ok = ok && (got.balance == 42);
            ok = ok && db_zslp_balance_credit(&ndb, "testcoin", "t1Buyer123", 8);
            ok = ok && db_zslp_balance_find(&ndb, "TESTCOIN", "t1Buyer123", &got);
            ok = ok && (got.balance == 50);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ZSLP token model saves, finds, and lists normalized keys... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_zslp_tokens_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            struct db_zslp_token_info token;
            struct db_zslp_token_info listed[4];
            struct ar_errors e;
            memset(&token, 0, sizeof(token));
            memset(listed, 0, sizeof(listed));
            ar_errors_clear(&e);
            ok = !db_zslp_token_validate_key("", &e) && ar_errors_any(&e);
            ok = ok && db_zslp_token_save_key(&ndb, "testcoin",
                                              "TESTCOIN", "Test Coin",
                                              2, "", 0, 1000);
            ok = ok && db_zslp_token_save_key(&ndb, "coinb",
                                              "COINB", "Coin B",
                                              0, "", 12, 500);
            ok = ok && db_zslp_token_find(&ndb, "TESTCOIN", &token);
            ok = ok && (strcmp(token.token_id, "TESTCOIN") == 0);
            ok = ok && (strcmp(token.ticker, "TESTCOIN") == 0);
            ok = ok && (token.total_minted == 1000);
            ok = ok && (db_zslp_token_list(&ndb, listed, 4) == 2);
            ok = ok && (strcmp(listed[0].ticker, "COINB") == 0);
            ok = ok && (strcmp(listed[1].ticker, "TESTCOIN") == 0);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("ZSLP transfer model lists token-scoped transfers... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_zslp_xfers_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            uint8_t txid[32];
            uint8_t token_id[32];
            uint8_t addr_hash[20];
            struct db_zslp_transfer_info listed[4];

            memset(txid, 0x11, sizeof(txid));
            memset(token_id, 0x22, sizeof(token_id));
            memset(addr_hash, 0x33, sizeof(addr_hash));
            memset(listed, 0, sizeof(listed));
            ok = db_zslp_transfer_save(&ndb, txid, 123, token_id, 2, 77, 1, addr_hash);
            ok = ok && (db_zslp_transfer_list_by_token(&ndb,
                    "2222222222222222222222222222222222222222222222222222222222222222",
                    listed, 4) == 1);
            ok = ok && (strcmp(listed[0].token_id,
                    "2222222222222222222222222222222222222222222222222222222222222222") == 0);
            ok = ok && (listed[0].block_height == 123);
            ok = ok && (listed[0].tx_type == 2);
            ok = ok && (listed[0].amount == 77);
            ok = ok && (listed[0].vout == 1);
            ok = ok && (strlen(listed[0].to_addr_hex) == 40);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
