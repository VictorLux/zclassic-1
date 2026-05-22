/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * progress_store — singleton owner of the `progress.kv` SQLite file.
 *
 * Wave S, milestone S-1.
 *
 * Why this exists
 * ----------------
 * The staged-sync pipeline (Wave S) decomposes chain advance into eight
 * stages, each of which owns a 64-bit cursor on disk. Crash-mid-step
 * replays the step idempotently because the cursor is unchanged on next
 * boot. The F-2 `stage` primitive already implements that contract on
 * top of any sqlite3 handle, via the `stage_cursor` table; what was
 * missing was a *home* for that table.
 *
 * `progress.kv` is that home: a small dedicated SQLite file alongside
 * `node.db`, opened once at boot, shared by every stage. Keeping it
 * separate from `node.db` matters because:
 *
 *   - Cursor commits are tiny and on the hot path; a dedicated WAL keeps
 *     them out of the way of the much larger node.db txns.
 *   - The architecture doc (docs/ARCHITECTURE.md, L4) lists `progress.kv`
 *     as a distinct storage engine. One file == one writer-actor.
 *   - Future stages may want to use blob columns, FTS, or LMDB without
 *     dragging node.db's schema along.
 *
 * Threading
 * ----------
 * One process-wide handle. The handle itself is safe for concurrent
 * reads (SQLite WAL); writes are serialised by each stage's own per-
 * stage mutex (see `stage_run_once`). The progress_store module does
 * not take any locks on the handle in the hot path — open/close go
 * through a one-shot mutex, but `progress_store_db()` is a plain
 * pointer load. */

#ifndef ZCL_STORAGE_PROGRESS_STORE_H
#define ZCL_STORAGE_PROGRESS_STORE_H

#include <sqlite3.h>
#include <stdbool.h>

struct json_value;

/* Open <datadir>/progress.kv in WAL mode and ensure the stage_cursor
 * table exists. Idempotent — a second call with the same datadir is a
 * no-op and returns true. A second call with a *different* datadir
 * returns false (one process, one progress store). */
bool progress_store_open(const char *datadir);

/* Singleton handle. NULL if not yet opened or already closed. */
sqlite3 *progress_store_db(void);

/* Graceful close: PRAGMA wal_checkpoint(TRUNCATE), sqlite3_close. Safe
 * to call repeatedly and from shutdown paths. */
void progress_store_close(void);

/* For zcl_state subsystem=progress (CLAUDE.md convention). `out` is
 * expected to have been json_set_object'd by the caller; this function
 * also calls json_set_object(out) defensively. `key` is unused. */
bool progress_store_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_STORAGE_PROGRESS_STORE_H */
