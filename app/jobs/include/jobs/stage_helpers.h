/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_helpers — shared static-inline helpers for the eight Job stages
 * in app/jobs/src.
 *
 *   stage_cursor_persisted    — read the DURABLY committed cursor of an
 *                               upstream stage from the stage_cursor table.
 *   stage_default_block_reader — read a block body from disk via the
 *                               block_index entry (HAVE_DATA guarded).
 *   stage_log_row_count       — SELECT COUNT(*) over a stage's log table
 *                               for the *_dump_state_json observability.
 *
 * Each takes a `tag` argument so LOG_WARN attribution stays with the calling
 * stage name ("body_persist", "script_validate", ...). */

#ifndef ZCL_JOBS_STAGE_HELPERS_H
#define ZCL_JOBS_STAGE_HELPERS_H

#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Read the persisted cursor of an upstream stage. Query the stage_cursor
 * table directly rather than the in-memory accessor so the floor reflects
 * what is DURABLY committed, not the in-memory value (0 on fresh init
 * until the first stage_run_once). `tag` is the calling stage's name for
 * log attribution. Returns 0 on prepare failure or no row. */
static inline uint64_t stage_cursor_persisted(sqlite3 *db, const char *name,
                                              const char *tag)
{
    if (!db || !name || !name[0])
        return 0;

    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(tag, "[%s] upstream cursor prepare failed: %s",
                 tag, sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return 0;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    uint64_t out = 0;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        out = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return out;
}

/* Default block-body reader used by stages whose injectable reader is
 * NULL. Guards on out/bi/HAVE_DATA and reads via pread from the block's
 * (nFile, nDataPos) on-disk position. Matches the *_reader_fn signature
 * of every consuming stage. */
static inline bool stage_default_block_reader(struct block *out,
                                              const struct block_index *bi,
                                              const char *datadir, void *user)
{
    (void)user;
    if (!out || !bi || !(bi->nStatus & BLOCK_HAVE_DATA))
        return false;

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    pos.nFile = bi->nFile;
    pos.nPos = bi->nDataPos;
    return read_block_from_disk_pread(out, &pos, datadir ? datadir : "");
}

/* SELECT COUNT(*) FROM <table>, for the *_dump_state_json log_rows field.
 * `tag` is the calling stage's name for log attribution. Returns -1 on
 * prepare failure or no row. */
static inline int64_t stage_log_row_count(sqlite3 *db, const char *tag,
                                          const char *table)
{
    if (!db || !table || !table[0])
        return -1;

    progress_store_tx_lock();
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(tag, "[%s] log count prepare failed: %s",
                 tag, sqlite3_errmsg(db));
        progress_store_tx_unlock();
        return -1;
    }
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    progress_store_tx_unlock();
    return n;
}

/* Reducer chain-window extender (the missing chain-extender).
 *
 * The reducer's tip_finalize uses a one-block lookahead (finalize H by
 * reading active_chain_at(H+1)) and then collapses the visible chain[]
 * window back to the finalized height via active_chain_move_window_tip, then
 * publishes the authority through the reducer's explicit tip publication.
 * This helper forward-extends the visible chain[] window to the already
 * tracked best-header pointer WITHOUT moving the authoritative tip. Stages
 * that read active_chain_at() call it at the top of step_once so the window is
 * supplied to the height they are about to process.
 *
 * Do not scan the full block map from a stage tick. The live map is millions
 * of entries; a full most-work sweep inside the supervisor can monopolize the
 * liveness thread and prevent repaired cursors from resuming. Header ingress
 * already maintains pindex_best_header as the current best known header, so
 * the reducer can use that O(delta) chain pointer directly and let the normal
 * downstream stages classify missing bodies, forks, and precondition failures.
 *
 * STRICTLY a no-op unless the caller owns the active-chain window for the
 * current stage step. */
static inline void reducer_extend_window_to_candidate(struct main_state *ms,
                                                       bool authoritative)
{
    if (!authoritative || !ms)
        return;
    struct block_index *cand = ms->pindex_best_header;
    if (!cand)
        cand = active_chain_most_work_candidate(&ms->chain_active,
                                                &ms->map_block_index);
    if (cand)
        (void)active_chain_extend_window(&ms->chain_active, cand);
}

#endif /* ZCL_JOBS_STAGE_HELPERS_H */
