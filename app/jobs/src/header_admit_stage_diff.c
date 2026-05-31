/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_admit_stage_diff — S-11 mini-diff harness for header_admit.
 *
 * Extracted verbatim from header_admit_stage.c (E1 file-size split). The
 * harness compares the contents of header_admit_log (what S-2 recorded)
 * against the live in-memory active_chain (the source S-2 read from),
 * giving empirical confidence the shadow stage keeps parity before more
 * stages stack on top. Read-only: no writes, no cs_main lock; snapshot
 * races are accepted for a diagnostic tool. See jobs/header_admit_stage.h
 * for the report shape and status semantics. */

#include "jobs/header_admit_stage.h"

#include "header_admit_internal.h"

#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* SELECT MAX(height) FROM header_admit_log. Returns -1 if empty. */
static int32_t log_max_height(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT MAX(height) FROM header_admit_log", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("header_admit", "[header_admit] diff: prepare MAX(height) failed: %s", sqlite3_errmsg(db));
        return -1;  /* raw-return-ok:diagnostic-treats-as-empty */
    }
    int32_t out = -1;
    if (sqlite3_step(st) == SQLITE_ROW &&  // raw-sql-ok:kernel-primitive
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
        out = (int32_t)sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

static void diff_sample_record(struct header_admit_diff_report *r,
                                int32_t h,
                                const uint8_t *log_hash, bool log_present,
                                const uint8_t *chain_hash, bool chain_present)
{
    if (r->sample_count >= HEADER_ADMIT_DIFF_MAX_SAMPLES) return;
    struct header_admit_diff_sample *s = &r->samples[r->sample_count++];
    s->height        = h;
    s->log_present   = log_present;
    s->chain_present = chain_present;
    if (log_present && log_hash)   memcpy(s->log_hash,   log_hash,   32);
    else                           memset(s->log_hash,   0, 32);
    if (chain_present && chain_hash) memcpy(s->chain_hash, chain_hash, 32);
    else                             memset(s->chain_hash, 0, 32);
}

bool header_admit_stage_diff(int32_t start_h, int32_t end_h,
                              struct header_admit_diff_report *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->first_divergent_height = -1;
    out->log_max_height         = -1;
    out->chain_tip_height       = -1;

    /* Not-ready guard: stage uninit or progress.kv closed. The report
     * is still populated with sensible defaults so callers can render. */
    struct main_state *ms = header_admit_internal_ms();
    sqlite3 *db = progress_store_db();
    if (!header_admit_internal_stage() || !ms || !db) {
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        out->start_height = (start_h < 0) ? 0 : start_h;
        out->end_height   = (end_h   < 0) ? 0 : end_h;
        return true;
    }

    out->cursor             = (int32_t)stage_cursor(header_admit_internal_stage());
    out->log_max_height     = log_max_height(db);
    out->chain_tip_height   = active_chain_height(&ms->chain_active);

    /* Resolve auto-bounds. A fully automatic diff should answer the
     * operator question "does the recent stage path match the active
     * chain?" rather than burning the capped range on genesis. */
    int32_t s = (start_h < 0) ? 0 : start_h;
    int32_t e = end_h;
    if (e < 0) {
        int32_t a = out->log_max_height;
        int32_t b = out->chain_tip_height;
        if (a < 0 && b < 0)      e = -1;
        else if (a < 0)          e = b;
        else if (b < 0)          e = a;
        else                     e = (a < b) ? a : b;
    }
    if (start_h < 0 && e >= HEADER_ADMIT_DIFF_MAX_RANGE)
        s = e - HEADER_ADMIT_DIFF_MAX_RANGE + 1;

    if (e < s) {
        out->status       = HEADER_ADMIT_DIFF_EMPTY;
        out->start_height = s;
        out->end_height   = e;
        return true;
    }

    /* Hard cap the range. */
    int64_t span = (int64_t)e - (int64_t)s + 1;
    if (span > HEADER_ADMIT_DIFF_MAX_RANGE) {
        e = s + HEADER_ADMIT_DIFF_MAX_RANGE - 1;
        span = HEADER_ADMIT_DIFF_MAX_RANGE;
    }
    out->start_height = s;
    out->end_height   = e;

    /* Load all log rows in the range into a packed array indexed by
     * (height - s). Bounded: span <= HEADER_ADMIT_DIFF_MAX_RANGE → ≤ 330 KB. */
    struct row {
        uint8_t hash[32];
        bool    present;
    };
    struct row *rows = zcl_calloc((size_t)span, sizeof(struct row),
                                   "header_admit_diff_rows");
    if (!rows) {
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        return true;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT height, hash FROM header_admit_log "
        "WHERE height BETWEEN ? AND ? ORDER BY height",
        -1, &st, NULL);
    if (rc != SQLITE_OK) {
        free(rows);
        out->status = HEADER_ADMIT_DIFF_NOT_READY;
        return true;
    }
    sqlite3_bind_int(st, 1, s);
    sqlite3_bind_int(st, 2, e);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {  // raw-sql-ok:kernel-primitive
        int32_t h = sqlite3_column_int(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int nb = sqlite3_column_bytes(st, 1);
        if (h < s || h > e || !blob || nb != 32) continue;
        struct row *r = &rows[h - s];
        memcpy(r->hash, blob, 32);
        r->present = true;
    }
    sqlite3_finalize(st);

    /* Walk the range. For each height, compare log row vs in-memory chain. */
    for (int32_t h = s; h <= e; h++) {
        const struct row *r = &rows[h - s];
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        bool chain_present = (bi != NULL && bi->phashBlock != NULL);
        const uint8_t *chain_hash =
            chain_present ? bi->phashBlock->data : NULL;

        if (!r->present && !chain_present) continue;  /* both missing → skip */

        out->checked_count++;
        if (r->present && chain_present) {
            if (memcmp(r->hash, chain_hash, 32) == 0) {
                out->match_count++;
            } else {
                out->mismatch_count++;
                if (out->first_divergent_height < 0)
                    out->first_divergent_height = h;
                diff_sample_record(out, h, r->hash, true, chain_hash, true);
            }
        } else if (r->present && !chain_present) {
            /* log has it, chain doesn't — chain shrank or reorged through. */
            out->missing_in_chain_count++;
            if (out->first_divergent_height < 0)
                out->first_divergent_height = h;
            diff_sample_record(out, h, r->hash, true, NULL, false);
        } else {
            /* chain has it, log doesn't — S-2 cursor lag. */
            out->missing_in_log_count++;
            if (out->first_divergent_height < 0)
                out->first_divergent_height = h;
            diff_sample_record(out, h, NULL, false, chain_hash, true);
        }
    }

    free(rows);

    /* Status: hash mismatches dominate; reorg-style log-ahead next;
     * normal cursor-lag third; otherwise converged or empty. */
    if (out->mismatch_count > 0)             out->status = HEADER_ADMIT_DIFF_DIVERGENT;
    else if (out->missing_in_chain_count > 0) out->status = HEADER_ADMIT_DIFF_LOG_AHEAD;
    else if (out->missing_in_log_count > 0)   out->status = HEADER_ADMIT_DIFF_CHAIN_AHEAD;
    else if (out->checked_count > 0)          out->status = HEADER_ADMIT_DIFF_CONVERGED;
    else                                       out->status = HEADER_ADMIT_DIFF_EMPTY;

    return true;
}
