/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_helpers — shared static-inline helpers for the eight Job stages
 * in app/jobs/src. These are the verbatim de-duplications of helpers that
 * were copy-pasted across the stage files; behaviour is byte-identical to
 * the per-file copies they replace.
 *
 *   stage_cursor_persisted    — read the DURABLY committed cursor of an
 *                               upstream stage from the stage_cursor table.
 *   stage_default_block_reader — read a block body from disk via the
 *                               block_index entry (HAVE_DATA guarded).
 *   stage_log_row_count       — SELECT COUNT(*) over a stage's log table
 *                               for the *_dump_state_json observability.
 *
 * Each takes a `tag` argument so the LOG_WARN attribution stays the
 * stage name ("body_persist", "script_validate", …) — byte-identical to
 * what each call site logged before the fold. */

#ifndef ZCL_JOBS_STAGE_HELPERS_H
#define ZCL_JOBS_STAGE_HELPERS_H

#include "storage/disk_block_io.h"
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
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT cursor FROM stage_cursor WHERE name = ?",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(tag, "[%s] upstream cursor prepare failed: %s",
                 tag, sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    uint64_t out = 0;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        out = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
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
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN(tag, "[%s] log count prepare failed: %s",
                 tag, sqlite3_errmsg(db));
        return -1;
    }
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Reducer chain-window extender (the missing chain-extender).
 *
 * The reducer's tip_finalize uses a one-block lookahead (finalize H by
 * reading active_chain_at(H+1)) and then collapses the visible chain[]
 * window back to the finalized height via active_chain_set_tip. Legacy
 * keeps a wider window because activate_best_chain assembles chain[] out to
 * find_most_work_chain's candidate before connect_tip walks it; the reducer
 * had no equivalent and wedged after a single block.
 *
 * This helper restores that property: it selects the most-work candidate
 * (side-effect-free mirror of find_most_work_chain) and forward-extends the
 * visible chain[] window to it WITHOUT moving the authoritative tip. Both
 * utxo_apply (forward-apply + reorg-unwind detection read active_chain_at)
 * and tip_finalize (lookahead) call it at the top of their step_once so the
 * window is always supplied to the height they are about to process.
 *
 * STRICTLY a no-op unless the reducer holds authority — the caller passes
 * `authoritative` from its own mode gate, so under the live default (SHADOW)
 * this never touches chain[] and legacy connect_tip remains the sole tip
 * writer (byte-identical live behaviour). */
static inline void reducer_extend_window_to_candidate(struct main_state *ms,
                                                       bool authoritative)
{
    if (!authoritative || !ms)
        return;
    struct block_index *cand =
        active_chain_most_work_candidate(&ms->chain_active,
                                         &ms->map_block_index);
    if (cand)
        (void)active_chain_extend_window(&ms->chain_active, cand);
}

#endif /* ZCL_JOBS_STAGE_HELPERS_H */
