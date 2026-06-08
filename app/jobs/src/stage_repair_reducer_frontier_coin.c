/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_reducer_frontier_coin — L1 coin-tear discriminators.
 *
 * This file contains the guarded exception that can run before L1 refuses
 * coins_applied_height > H*: the one-shot value_overflow stale-verdict repair.
 * The main reducer-frontier file remains the flag/body/tip sweep. */

#include "stage_repair_reducer_frontier_internal.h"

#include "jobs/stage_repair.h"
#include "jobs/stage_repair_internal.h"
#include "jobs/utxo_apply_delta.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

static bool hole_below_cursor_unlocked(sqlite3 *db, int cursor,
                                       const char *status, int *out_height)
{
    *out_height = -1;
    if (cursor <= 0)
        return true;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT height FROM utxo_apply_log "
            "WHERE ok = 0 AND status = ? AND height < ? "
            "ORDER BY height LIMIT 1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s hole prepare failed: %s",
                 status, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, cursor);

    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *out_height = sqlite3_column_int(st, 0);
    } else if (rc != SQLITE_DONE) {
        LOG_WARN("stage_repair",
                 "[stage_repair] %s hole step failed rc=%d: %s",
                 status, rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);
    return true;
}

static bool read_active_block_checked(struct main_state *ms, int height,
                                      struct block *blk,
                                      struct uint256 *block_hash)
{
    if (!ms || !blk || !block_hash)
        LOG_FAIL("stage_repair", "read_active_block_checked: NULL input");

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    bool have = false;

    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (bi && bi->phashBlock && (bi->nStatus & BLOCK_HAVE_DATA) &&
        bi->nFile >= 0) {
        *block_hash = *bi->phashBlock;
        pos.nFile = bi->nFile;
        pos.nPos = bi->nDataPos;
        have = true;
    }
    zcl_mutex_unlock(&ms->cs_main);

    if (!have)
        return false;

    char datadir[2048];
    GetDataDir(true, datadir, sizeof(datadir));
    if (!read_block_from_disk_pread(blk, &pos, datadir))
        return false;

    struct uint256 got;
    block_get_hash(blk, &got);
    if (uint256_cmp(&got, block_hash) != 0) {
        char want_hex[65];
        char got_hex[65];
        uint256_get_hex(block_hash, want_hex);
        uint256_get_hex(&got, got_hex);
        LOG_WARN("stage_repair",
                 "[stage_repair] repair read wrong block h=%d want=%s got=%s",
                 height, want_hex, got_hex);
        return false;
    }
    return true;
}

static bool maybe_repair_value_overflow(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out)
{
    int cursor = -1;
    int height = -1;

    progress_store_tx_lock();
    bool ok = stage_repair_cursor_at_unlocked(db, "utxo_apply", &cursor) &&
              hole_below_cursor_unlocked(db, cursor, "value_overflow",
                                         &height);
    progress_store_tx_unlock();
    if (!ok)
        return false;

    out->value_overflow_repair_height = height;
    out->value_overflow_cursor_before = cursor;
    out->value_overflow_cursor_after = cursor;
    if (height < 0 || cursor <= 0 || height >= cursor)
        return true;
    if (!apply)
        return true;

    struct block blk;
    struct uint256 block_hash;
    block_init(&blk);
    if (!read_active_block_checked(ms, height, &blk, &block_hash)) {
        LOG_WARN("stage_repair",
                 "[stage_repair] value_overflow repair refused: cannot read "
                 "canonical block h=%d",
                 height);
        block_free(&blk);
        return true;
    }

    struct utxo_apply_value_overflow_repair_result rr;
    ok = utxo_apply_repair_value_overflow_hole(
        db, height, (uint64_t)cursor, &block_hash, &blk, &rr);
    block_free(&blk);
    if (!ok)
        return false;

    out->value_overflow_repair_attempted = rr.attempted;
    out->value_overflow_repaired = rr.repaired;
    out->value_overflow_repair_marker_seen = rr.marker_seen;
    out->value_overflow_repair_genuinely_invalid = rr.genuinely_invalid;
    out->value_overflow_cursor_after = (int)rr.cursor_after;
    if (rr.repaired) {
        out->refused_coin_tear = false;
        out->repaired = true;
    }
    return true;
}

bool stage_reducer_frontier_try_coin_tear_repair(
    sqlite3 *db,
    struct main_state *ms,
    bool apply,
    struct stage_reducer_frontier_reconcile_result *out,
    bool *handled)
{
    if (!out || !handled)
        LOG_FAIL("stage_repair", "coin tear repair: NULL output");
    *handled = false;

    if (!maybe_repair_value_overflow(db, ms, apply, out))
        return false;
    if (out->value_overflow_repaired) {
        LOG_WARN("stage_repair",
                 "[stage_repair] reducer_frontier repaired stale "
                 "value_overflow hole h=%d utxo_apply=%d->%d; "
                 "forward stage replay must fill the hole before L1 continues",
                 out->value_overflow_repair_height,
                 out->value_overflow_cursor_before,
                 out->value_overflow_cursor_after);
        *handled = true;
        return true;
    }

    return true;
}
