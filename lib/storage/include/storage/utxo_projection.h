/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_projection — Phase 4b: the FIRST production consumer of the
 * append-only event_log (Phase 4a).
 *
 * The projection is a rebuildable SQLite UTXO set derived from
 * EV_UTXO_ADD / EV_UTXO_SPEND events. In shadow mode (this PR), the
 * legacy `update_coins()` path still authors UTXO writes against
 * `coins.db`; we just additionally emit events that the projection
 * consumes. A separate 4b-cutover PR (gated on 24h of clean
 * `zcl_utxo_projection_diff` runs) disables the legacy SQLite write
 * and makes this projection authoritative.
 *
 * Threading
 * ----------
 * Single open handle per process (mirrors peers_projection / event_log
 * conventions). `_get` / `_count` / `_commitment` are reentrant reads;
 * `_catch_up` serialises internally via a SQLite IMMEDIATE txn.
 *
 * See `docs/work/wt-phase4b-utxo-projection.md` for the assignment that
 * shipped this file. */

#ifndef ZCL_STORAGE_UTXO_PROJECTION_H
#define ZCL_STORAGE_UTXO_PROJECTION_H

#include "storage/event_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct utxo_projection utxo_projection_t;

/* Open or create the projection at `projection_path`. Replays from the
 * `last_consumed_offset` stored in the projection's own metadata table
 * (idempotent across crashes). Returns NULL on unrecoverable error
 * (schema mismatch, sqlite open failure). The returned handle is also
 * published as the process-global accessor (see `utxo_projection_get_global`). */
utxo_projection_t *utxo_projection_open(const char *projection_path,
                                        event_log_t *log);

/* Graceful close: PRAGMA wal_checkpoint(TRUNCATE), sqlite3_close.
 * NULL-safe. */
void utxo_projection_close(utxo_projection_t *p);

/* Consume new events from the event log starting at the projection's
 * own last_consumed_offset. Idempotent — replaying the same byte range
 * twice produces the same UTXO set. Returns the new
 * last_consumed_offset on success, or `UINT64_MAX` on error. */
uint64_t utxo_projection_catch_up(utxo_projection_t *p);

/* Lookup a UTXO by (txid, vout). Returns true if present; fills
 * value/script if the corresponding out-pointer is non-NULL. If the
 * caller's `script_cap` is smaller than the stored script length, the
 * script is truncated to `script_cap` bytes — but `*script_len_out`
 * always reports the true length. */
bool utxo_projection_get(utxo_projection_t *p,
                         const uint8_t txid[32], uint32_t vout,
                         int64_t *value_out,
                         uint8_t *script_out, size_t script_cap,
                         size_t *script_len_out);

/* Total live UTXOs in the projection. O(SELECT COUNT(*)) — acceptable
 * given we run with WITHOUT ROWID and SQLite caches the count. */
uint64_t utxo_projection_count(utxo_projection_t *p);

/* SHA3-256 over every (txid|vout_le|value_le|script_len_le|script|
 * height_le|is_coinbase) UTXO in ORDER BY txid, vout. Matches the
 * canonical serialisation in `lib/coins/src/utxo_commitment.c` so
 * the shadow-diff tool can compare the projection commitment to the
 * legacy coins.db commitment byte-for-byte. Returns 0 on success. */
int utxo_projection_commitment(utxo_projection_t *p, uint8_t out[32]);

/* Process-global accessor for the projection handle published by the
 * boot wiring (`config/src/boot.c`). Returns NULL if not yet opened or
 * already closed. Safe to call from any thread. */
utxo_projection_t *utxo_projection_get_global(void);

/* Process-global setter for the event log used by the shadow emission
 * path in `update_coins.c`. NULL log disables emission (the legacy
 * SQLite write still happens). Mirrors peers_projection wiring. */
void utxo_projection_set_event_log(event_log_t *log);
event_log_t *utxo_projection_event_log(void);

/* Shadow emission helpers used by `lib/validation/src/update_coins.c`.
 * Both increment `g_utxo_event_emit_*_total` counters internally so the
 * dump_state_json output shows how many events have been authored.
 *
 * `script_bytes` may be NULL iff `script_len == 0`. */
bool utxo_projection_emit_add(const uint8_t txid[32], uint32_t vout,
                              int64_t value, uint32_t height,
                              bool is_coinbase,
                              const uint8_t *script_bytes,
                              uint32_t script_len);
bool utxo_projection_emit_spend(const uint8_t txid[32], uint32_t vout);

/* For zcl_state subsystem=utxo_projection (CLAUDE.md convention).
 * `out` is initialized by the caller; this function also calls
 * json_set_object(out) defensively. `key` is unused. */
struct json_value;
bool utxo_projection_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_STORAGE_UTXO_PROJECTION_H */
