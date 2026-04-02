/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Focused wallet projection model tests. */

#include "test/test_helpers.h"
#include <unistd.h>

int test_model_wallet_projection(void)
{
    int failures = 0;

    printf("Wallet projection model summarizes balances and tips... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_wallet_projection_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            struct db_block blk;
            struct db_wallet_utxo utxo;
            struct db_sapling_note note;
            struct db_wallet_projection_summary summary;

            memset(&blk, 0, sizeof(blk));
            memset(&utxo, 0, sizeof(utxo));
            memset(&note, 0, sizeof(note));
            memset(&summary, 0, sizeof(summary));

            memset(blk.hash, 0x01, 32);
            memset(blk.prev_hash, 0x0b, 32);
            memset(blk.merkle_root, 0x0c, 32);
            blk.height = 25;
            blk.time = 12345;
            blk.bits = 0x1d00ffffU;
            blk.status = 3;
            ok = db_block_save(&ndb, &blk);

            memset(utxo.txid, 0x02, 32);
            memset(utxo.address_hash, 0x03, 20);
            utxo.value = 5000;
            utxo.height = 30;
            utxo.script = (uint8_t *)"\x76\xa9\x14\x00\x88\xac";
            utxo.script_len = 6;
            ok = ok && db_wallet_utxo_save(&ndb, &utxo);

            memset(note.txid, 0x04, 32);
            memset(note.rcm, 0x05, 32);
            memset(note.ivk, 0x06, 32);
            memset(note.diversifier, 0x07, 11);
            memset(note.pk_d, 0x08, 32);
            memset(note.cm, 0x09, 32);
            memset(note.nullifier, 0x0a, 32);
            note.value = 7000;
            note.block_height = 22;
            ok = ok && db_sapling_note_save(&ndb, &note);

            ok = ok && db_wallet_projection_summary(&ndb, &summary);
            ok = ok && (summary.chain_tip_height == 25);
            ok = ok && (summary.effective_tip_height == 30);
            ok = ok && (summary.utxo_count == 1);
            ok = ok && (summary.note_count == 1);
            ok = ok && (summary.transparent_balance == 5000);
            ok = ok && (summary.shielded_balance == 7000);
            ok = ok && (summary.speed_balance == 5000);
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
