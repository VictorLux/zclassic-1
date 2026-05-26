/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * file_export_consensus_snapshot + its checked-SQL helpers. Split out
 * of file_controller.c (D5); behavior byte-identical. */

#include "platform/time_compat.h"
#include "controllers/file_controller.h"
#include "controllers/strong_params.h"
#include "views/format_helpers.h"
#include "chain/mmr.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sqlite3.h>
#include <pthread.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static bool file_export_exec_checked(sqlite3 *db, const char *sql,
                                    const char *label)
{
    if (!db || !sql)
        LOG_FAIL("file", "exec_checked: NULL %s", !db ? "db" : "sql");

    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_FAIL("file", "file_export_snapshot: %s failed: %s",
                label, sqlite3_errmsg(db));
    }
    return true;
}

static bool file_export_prepare_checked(sqlite3 *db, const char *sql,
                                      sqlite3_stmt **stmt,
                                      const char *label)
{
    if (!db || !sql || !stmt)
        LOG_FAIL("file", "prepare_checked: NULL %s", !db ? "db" : !sql ? "sql" : "stmt");

    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK ||
        !*stmt) {
        LOG_FAIL("file", "file_export_snapshot: %s failed: %s",
                label, sqlite3_errmsg(db));
    }
    return true;
}

static bool file_export_step_checked(sqlite3_stmt *stmt, sqlite3 *db,
                                    const char *label)
{
    if (!stmt || !db)
        LOG_FAIL("file", "step_checked: NULL %s", !stmt ? "stmt" : "db");

    /* Snapshot-export helper routed through AR_STEP_ROW_READONLY
     * because every statement this helper sees is a VACUUM/ATTACH
     * control that writes to a side-database owned by the export
     * (not by any AR-managed model). Real model writes still go
     * through AR_BEGIN_SAVE. */
    int rc = AR_STEP_ROW_READONLY(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        LOG_FAIL("file", "file_export_snapshot: %s failed: rc=%d err=%s",
                label, rc, sqlite3_errmsg(db));
    }
    return true;
}


bool file_export_consensus_snapshot(const char *datadir)
{
    if (!datadir)
        LOG_FAIL("file", "export_snapshot: NULL datadir");

    char src_path[576], dst_path[576];
    snprintf(src_path, sizeof(src_path), "%s/node.db", datadir);
    snprintf(dst_path, sizeof(dst_path), "%s/consensus_snapshot.db", datadir);

    struct stat src_st;
    if (stat(src_path, &src_st) != 0 || src_st.st_size < 1000000)
        LOG_FAIL("file", "export_snapshot: %s missing or too small", src_path);

    /* Refuse to clobber a downloaded snapshot with an empty rebuild.
     * On a fresh node, node.db has only the genesis-era UTXOs that
     * block-by-block IBD has produced so far. Exporting that would
     * also unlink the downloaded consensus_snapshot.db that the next
     * boot needs to import — destroying the secure-snapshot fast-path
     * for any node that runs file_service and then restarts before
     * full chain catchup. Bail when the source is too small to make
     * a useful snapshot. */
    {
        sqlite3 *probe = NULL;
        int64_t src_utxos = 0;
        if (sqlite3_open_v2(src_path, &probe,
                            SQLITE_OPEN_READONLY, NULL) == SQLITE_OK
            && probe) {
            sqlite3_stmt *q = NULL;
            if (sqlite3_prepare_v2(probe,
                    "SELECT COUNT(*) FROM utxos",
                    -1, &q, NULL) == SQLITE_OK && q) {
                if (sqlite3_step(q) == SQLITE_ROW)  // raw-sql-ok:read-only-probe
                    src_utxos = sqlite3_column_int64(q, 0);
                sqlite3_finalize(q);
            }
            sqlite3_close(probe);
        }
        if (src_utxos < 1000)
            LOG_FAIL("file",
                "export_snapshot: source utxos=%lld is below the "
                "1000-row threshold — preserving any downloaded "
                "consensus_snapshot.db so the next boot can import it",
                (long long)src_utxos);
    }

    /* Remove old snapshot */
    unlink(dst_path);

    /* Open source read-only */
    sqlite3 *src_db = NULL;
    bool src_db_opened = false;
    bool dst_db_opened = false;
    bool dst_txn_open = false;
    bool src_attached = false;
    bool ok = true;

    if (sqlite3_open_v2(src_path, &src_db, SQLITE_OPEN_READONLY, NULL)
        != SQLITE_OK || !src_db) {
        LOG_FAIL("file", "export_snapshot: cannot open source db %s", src_path);
    }
    src_db_opened = true;

    /* Create destination and copy only consensus tables via ATTACH */
    sqlite3 *dst_db = NULL;
    if (sqlite3_open_v2(dst_path, &dst_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        sqlite3_close(src_db);
        LOG_FAIL("file", "export_snapshot: cannot create destination db %s", dst_path);
    }
    dst_db_opened = true;

    /* Performance: batch everything in one transaction */
    if (!file_export_exec_checked(dst_db, "PRAGMA journal_mode=WAL",
                                 "set journal_mode WAL") ||
        !file_export_exec_checked(dst_db, "PRAGMA synchronous=OFF",
                                 "set synchronous OFF") ||
        !file_export_exec_checked(dst_db, "PRAGMA cache_size=-65536",
                                 "set cache_size") ||
        !file_export_exec_checked(dst_db, "PRAGMA temp_store=FILE",
                                 "set temp_store FILE")) {
        ok = false;
        goto export_cleanup;
    }

    /* Attach source database */
    char *attach_sql = sqlite3_mprintf("ATTACH DATABASE '%q' AS src", src_path);
    if (!attach_sql) {
        LOG_WARN("file_export_snapshot", "file_export_snapshot: out of memory building ATTACH SQL");
        ok = false;
        goto export_cleanup;
    }
    bool attach_ok = file_export_exec_checked(dst_db, attach_sql, "attach source db");
    sqlite3_free(attach_sql);
    if (!attach_ok) {
        ok = false;
        goto export_cleanup;
    }
    src_attached = true;

    /* Copy ONLY public consensus tables.
     * This whitelist approach is safer than a blacklist — new private
     * tables added later won't accidentally get exported. */
    static const char *safe_tables[] = {
        "blocks", "transactions", "utxos", "addresses",
        "chain_stats", "zslp_tokens", "zslp_balances",
        NULL
    };

    if (!file_export_exec_checked(dst_db, "BEGIN", "begin snapshot transaction")) {
        ok = false;
        goto export_cleanup;
    }
    dst_txn_open = true;

    int tables_copied = 0;
    for (int i = 0; safe_tables[i]; i++) {
        /* Create table with same schema */
        char create_sql[512];
        snprintf(create_sql, sizeof(create_sql),
            "CREATE TABLE IF NOT EXISTS %s AS SELECT * FROM src.%s",
            safe_tables[i], safe_tables[i]);
        if (file_export_exec_checked(dst_db, create_sql,
                                    "copy consensus table")) {
            tables_copied++;
        } else {
            ok = false;
            goto export_cleanup;
        }
    }

    /* Store snapshot metadata */
    if (!file_export_exec_checked(dst_db,
        "CREATE TABLE IF NOT EXISTS _snapshot_meta "
        "(key TEXT PRIMARY KEY, value TEXT)",
        "create metadata table")) {
        ok = false;
        goto export_cleanup;
    }

    /* Get source chain height */
    sqlite3_stmt *sel = NULL;
    if (!file_export_prepare_checked(dst_db,
            "SELECT MAX(height) FROM blocks",
            &sel, "read snapshot height")) {
        ok = false;
        goto export_cleanup;
    }
    int snap_height = 0;
    if (!file_export_step_checked(sel, dst_db, "read snapshot height"))
        ok = false;
    else if (sqlite3_column_type(sel, 0) != SQLITE_NULL)
        snap_height = sqlite3_column_int(sel, 0);
    if (sel) sqlite3_finalize(sel);
    if (!ok) goto export_cleanup;

    {
        sqlite3_stmt *meta = NULL;
        if (!file_export_prepare_checked(
                dst_db,
                "INSERT INTO _snapshot_meta(key,value) VALUES(?,?)",
                &meta, "prepare metadata insert")) {
            ok = false;
            goto export_cleanup;
        }
        char value_buf[32];
        snprintf(value_buf, sizeof(value_buf), "%d", snap_height);
        if (sqlite3_bind_text(meta, 1, "height", -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(meta, 2, value_buf, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            !file_export_step_checked(meta, dst_db, "insert metadata height")) {
            ok = false;
            sqlite3_finalize(meta);
            goto export_cleanup;
        }

        sqlite3_reset(meta);
        sqlite3_clear_bindings(meta);

        snprintf(value_buf, sizeof(value_buf), "%d", tables_copied);
        if (sqlite3_bind_text(meta, 1, "tables", -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(meta, 2, value_buf, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            !file_export_step_checked(meta, dst_db, "insert metadata table count")) {
            ok = false;
            sqlite3_finalize(meta);
            goto export_cleanup;
        }
        sqlite3_finalize(meta);
    }

    if (!file_export_exec_checked(dst_db, "COMMIT", "commit snapshot transaction")) {
        ok = false;
        goto export_cleanup;
    }
    dst_txn_open = false;
    src_attached = false;
    if (!file_export_exec_checked(dst_db, "DETACH DATABASE src", "detach source db")) {
        ok = false;
        goto export_cleanup;
    }

    /* Compact */
    if (!file_export_exec_checked(dst_db, "PRAGMA synchronous=NORMAL",
                                 "restore sync NORMAL") ||
        !file_export_exec_checked(dst_db, "VACUUM", "vacuum snapshot")) {
        ok = false;
        goto export_cleanup;
    }

    struct stat dst_st;
    if (stat(dst_path, &dst_st) != 0) {
        ok = false;
        goto export_cleanup;
    }

    printf("Consensus snapshot: %d tables, height %d, %.0f MB\n",
           tables_copied, snap_height,
           (double)dst_st.st_size / (1024.0*1024.0));
    if (tables_copied == 0) {
        LOG_WARN("file_export_snapshot", "file_export_snapshot: no tables exported");
        ok = false;
        goto export_cleanup;
    }

export_cleanup:
    if (dst_db_opened && dst_db) {
        if (dst_txn_open && !sqlite3_get_autocommit(dst_db) &&
            !file_export_exec_checked(dst_db, "ROLLBACK", "rollback snapshot tx")) {
            /* best-effort rollback logged by helper */
        }
        if (src_attached)
            file_export_exec_checked(dst_db, "DETACH DATABASE src", "detach source db");
        sqlite3_close(dst_db);
        dst_db = NULL;
        dst_db_opened = false;
    }
    if (src_db_opened && src_db) {
        sqlite3_close(src_db);
        src_db = NULL;
        src_db_opened = false;
    }
    if (!ok)
        unlink(dst_path);
#ifdef __GLIBC__
    malloc_trim(0);
#endif

    return ok;
}

/* ── RPC handlers ──────────────────────────────────────────────── */

