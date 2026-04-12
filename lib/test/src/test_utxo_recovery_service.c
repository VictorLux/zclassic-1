/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for utxo_recovery_service — boot-time UTXO wipe, import,
 * restore, and integrity operations.
 */

#include "test/test_helpers.h"
#include "services/utxo_recovery_service.h"
#include "services/recovery_policy.h"
#include "validation/main_state.h"
#include "models/database.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#define URS_CHECK(name, expr) do {              \
    printf("%s... ", (name));                   \
    if ((expr)) printf("OK\n");                 \
    else { printf("FAIL\n"); failures++; }      \
} while (0)

/* Build a minimal chain in main_state */
static void urs_build_chain(struct main_state *ms, int n)
{
    struct uint256 hashes[256];
    int limit = n < 256 ? n : 256;

    for (int h = 0; h < limit; h++) {
        memset(&hashes[h], 0, sizeof(hashes[h]));
        hashes[h].data[0] = (uint8_t)(h & 0xFF);
        hashes[h].data[1] = (uint8_t)((h >> 8) & 0xFF);
        hashes[h].data[3] = 0xCC;  /* distinct from CSV tests */

        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)ms, &hashes[h]);
        if (!pi) continue;

        pi->nHeight = h;
        pi->nBits = 0x1f07ffff;
        pi->nTime = 1000000 + (uint32_t)h * 150;
        pi->nVersion = 4;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx = 1;
        pi->nChainTx = (uint32_t)(h + 1);
        arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));

        if (h > 0) {
            struct block_index *prev = block_map_find(
                &ms->map_block_index, &hashes[h - 1]);
            if (prev) pi->pprev = prev;
        }
    }
    if (limit > 0) {
        struct block_index *tip = block_map_find(
            &ms->map_block_index, &hashes[limit - 1]);
        if (tip) active_chain_set_tip(&ms->chain_active, tip);
    }
}

int test_utxo_recovery_service(void)
{
    printf("\n=== utxo recovery service tests ===\n");
    int failures = 0;

    /* ── 1. Policy-gated wipe: allowed (small UTXO set) ── */

    {
        /* Create temp SQLite DB with a few UTXOs */
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_wipe.db", getpid());
        mkdir("./test-tmp", 0755);

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Insert 5 fake UTXOs */
            for (int i = 0; i < 5; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, i + 1);
                node_db_exec(&ndb, sql);
            }

            int64_t before = node_db_utxo_count(&ndb);

            /* Set env to allow wipe of 10 rows */
            setenv("ZCL_MAX_UTXO_WIPE_ROWS", "10", 1);
            bool ok = utxo_recovery_wipe(&ndb, "test.small_wipe");
            int64_t after = node_db_utxo_count(&ndb);
            unsetenv("ZCL_MAX_UTXO_WIPE_ROWS");

            URS_CHECK("urs: policy allows small wipe",
                      ok && before == 5 && after == 0);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: policy allows small wipe (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 2. Policy-gated wipe: refused (too many rows) ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_refuse.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Insert 50 fake UTXOs */
            node_db_begin(&ndb);
            for (int i = 0; i < 50; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, i + 1);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            /* Set env to allow only 10 rows — should refuse 50 */
            setenv("ZCL_MAX_UTXO_WIPE_ROWS", "10", 1);
            bool ok = utxo_recovery_wipe(&ndb, "test.large_wipe");
            int64_t after = node_db_utxo_count(&ndb);
            unsetenv("ZCL_MAX_UTXO_WIPE_ROWS");

            URS_CHECK("urs: policy refuses large wipe",
                      !ok && after == 50);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: policy refuses large wipe (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 3. Reimport flag detection ── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_urs_reimport", getpid());
        mkdir(tmpdir, 0755);

        /* Write needs_reimport flag */
        char flag_path[512];
        snprintf(flag_path, sizeof(flag_path), "%s/needs_reimport", tmpdir);
        FILE *f = fopen(flag_path, "w");
        if (f) { fputs("1", f); fclose(f); }

        bool found = utxo_recovery_check_reimport_flag(tmpdir);
        /* File should be removed after check */
        struct stat st;
        bool file_gone = (stat(flag_path, &st) != 0);

        URS_CHECK("urs: reimport flag detected and removed",
                  found && file_gone);

        rmdir(tmpdir);
    }

    /* ── 4. Reimport flag absent → returns false ── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_urs_no_reimport", getpid());
        mkdir(tmpdir, 0755);

        bool found = utxo_recovery_check_reimport_flag(tmpdir);
        URS_CHECK("urs: no reimport flag → false", !found);

        rmdir(tmpdir);
    }

    /* ── 5. Prepare reimport: wipe + clear migration flag ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_prep.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Set migration flag */
            uint8_t one = 1;
            node_db_state_set(&ndb, "leveldb_utxo_migrated", &one, 1);

            /* Insert 3 UTXOs */
            for (int i = 0; i < 3; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, 1, "
                    "100000, X'00')", i, i);
                node_db_exec(&ndb, sql);
            }

            setenv("ZCL_MAX_UTXO_WIPE_ROWS", "100", 1);
            bool ok = utxo_recovery_prepare_reimport(&ndb);
            unsetenv("ZCL_MAX_UTXO_WIPE_ROWS");

            int64_t utxos = node_db_utxo_count(&ndb);
            uint8_t buf[8];
            size_t len = 0;
            bool flag = node_db_state_get(&ndb, "leveldb_utxo_migrated",
                                           buf, sizeof(buf), &len);

            URS_CHECK("urs: prepare reimport: wipe + clear flag",
                      ok && utxos == 0 && !flag);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: prepare reimport (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 6. Clean above tip: removes only stragglers ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_above.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        urs_build_chain(&ms, 50);  /* tip at h=49 */

        if (node_db_open(&ndb, db_path)) {
            /* Insert UTXOs: 10 below tip, 5 above */
            node_db_begin(&ndb);
            for (int i = 0; i < 15; i++) {
                int h = (i < 10) ? (i + 1) : (50 + i - 9);
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, h);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            int64_t before = node_db_utxo_count(&ndb);
            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            int64_t after = node_db_utxo_count(&ndb);

            URS_CHECK("urs: clean above tip removes 5 stragglers",
                      before == 15 && cleaned == 5 && after == 10);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip (db open failed)", false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 7. Clean above tip: refuses when >1000 ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_refuse_above.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        urs_build_chain(&ms, 10);  /* tip at h=9 */

        if (node_db_open(&ndb, db_path)) {
            /* Insert 1500 UTXOs all above tip (h=10..1509) */
            node_db_begin(&ndb);
            for (int i = 0; i < 1500; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, 10 + i);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            int64_t before = node_db_utxo_count(&ndb);
            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            int64_t after = node_db_utxo_count(&ndb);

            URS_CHECK("urs: clean above tip refuses >1000",
                      before == 1500 && cleaned == 0 && after == 1500);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip refuses (db open failed)", false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 8. Reimport flag with "0" value → no reimport ── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_urs_reimport0", getpid());
        mkdir(tmpdir, 0755);

        char flag_path[512];
        snprintf(flag_path, sizeof(flag_path), "%s/needs_reimport", tmpdir);
        FILE *f = fopen(flag_path, "w");
        if (f) { fputs("0", f); fclose(f); }

        bool found = utxo_recovery_check_reimport_flag(tmpdir);
        URS_CHECK("urs: reimport flag '0' → no reimport", !found);

        /* File should still be removed */
        unlink(flag_path);
        rmdir(tmpdir);
    }

    /* ── 9. LDB import: already migrated → no-op ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_ldb_noop.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Mark as already migrated */
            uint8_t one = 1;
            node_db_state_set(&ndb, "leveldb_utxo_migrated", &one, 1);

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            block_map_init(&ms.map_block_index);
            active_chain_init(&ms.chain_active);

            struct coins_view_cache cache;
            struct coins_view nv;
            memset(&nv, 0, sizeof(nv));
            coins_view_cache_init(&cache, &nv);

            struct utxo_recovery_ctx uctx = {
                .state = &ms,
                .coins_sqlite = NULL,
                .coins_tip = &cache,
                .ndb = &ndb,
                .datadir = "/nonexistent",
                .params = NULL,
                .activation_ctl = NULL,
                .db_service = NULL,
            };

            struct utxo_import_result ir = utxo_recovery_import_ldb(&uctx);

            URS_CHECK("urs: LDB import skips when already migrated",
                      !ir.imported && !ir.skip_activate);

            coins_view_cache_free(&cache);
            block_map_free(&ms.map_block_index);
            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: LDB import skip (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 10. Clean above tip: no-op when tip=0 ── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        /* no chain built — tip is NULL */

        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_notip.db", getpid());
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            URS_CHECK("urs: clean above tip no-op when tip=0",
                      cleaned == 0);
            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip no-op (db open failed)", false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    printf("--- utxo_recovery_service: %d failure(s) ---\n", failures);
    return failures;
}
