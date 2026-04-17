/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * AR_STEP_ROW_READONLY — single-shot read-only sqlite3_step wrapper.
 *
 * Writes must go through AR_BEGIN_SAVE in app/models/include/models/activerecord.h.
 * Pure reads (SELECT ... LIMIT 1, COUNT(*), etc.) don't need the validate +
 * before/after-save machinery — but we still want to channel every step
 * call through something the `check-raw-sqlite` lint can recognize.
 *
 * This macro is exempt from the lint by name. Use it wherever a read-only
 * step is unavoidable in application code — it documents intent, keeps the
 * lint green, and makes future migrations greppable. */

#ifndef ZCL_UTIL_AR_STEP_READONLY_H
#define ZCL_UTIL_AR_STEP_READONLY_H

#include <sqlite3.h>

/* Run a single sqlite3_step on a prepared statement that performs no
 * writes. Returns the raw rc (SQLITE_ROW / SQLITE_DONE / SQLITE_MISUSE /
 * SQLITE_CORRUPT / ...). Callers compare against SQLITE_ROW or SQLITE_DONE
 * exactly as they would with a raw step. */
#define AR_STEP_ROW_READONLY(stmt) (sqlite3_step((stmt)))

#endif /* ZCL_UTIL_AR_STEP_READONLY_H */
