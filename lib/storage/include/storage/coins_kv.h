/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_kv — the reducer's canonical UTXO set as a `coins` table IN progress.kv.
 *
 * WHY (docs/work/tip-durability-collapse.md): the live coins set used to live in
 * a SEPARATE WAL database (utxo_projection.db), folded from an out-of-txn
 * event_log, while the stage cursor + inverse-delta + utxo_apply_log row commit
 * in progress.kv. Two WAL databases have NO atomic cross-commit (WAL has no
 * master journal) — a crash drifted the coins from the cursor and no forward
 * path could realign them (the entire tip-wedge class). Storing the coins HERE,
 * on the progress.kv handle, makes every mutation commit inside the SAME
 * stage_run_once BEGIN IMMEDIATE as the cursor: every effect of a block lands or
 * rolls back as one atomic unit. Mirrors the proven created_outputs_index.
 *
 * Every function operates on the passed progress.kv handle and therefore
 * participates in whatever transaction the caller already holds open. Schema +
 * serialisation mirror utxo_projection's `utxo` table column-for-column so the
 * SHA3 UTXO commitment is byte-identical.
 */
#ifndef STORAGE_COINS_KV_H
#define STORAGE_COINS_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3;
struct coins;

/* CREATE TABLE IF NOT EXISTS coins(...). Idempotent. */
bool coins_kv_ensure_schema(struct sqlite3 *db);

/* INSERT OR REPLACE one output. `script` may be NULL iff `script_len`==0. */
bool coins_kv_add(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout,
                  int64_t value, int32_t height, bool is_coinbase,
                  const uint8_t *script, size_t script_len);

/* DELETE one output (spend). A missing row is not an error. */
bool coins_kv_spend(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout);

/* True iff (txid,vout) is currently live (unspent). */
bool coins_kv_exists(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout);

/* Count of live outputs. Returns -1 on error. */
int64_t coins_kv_count(struct sqlite3 *db);

/* Reconstruct a `struct coins` for `txid` from live rows (`out` is coins_init'd
 * by this call). Returns false (num_vout==0) if the txid has no live outputs.
 * Same two-pass shape as utxo_projection_get_coins / coins_view_sqlite. */
bool coins_kv_get_coins(struct sqlite3 *db, const uint8_t txid[32],
                        struct coins *out);

/* gettxoutsetinfo aggregate: distinct txids, total outputs, summed value.
 * Mirrors utxo_projection_setinfo exactly. Returns false on error. */
bool coins_kv_setinfo(struct sqlite3 *db, int64_t *num_txs,
                      int64_t *num_txouts, int64_t *total_amount);

/* SHA3-256 UTXO commitment over the coins set in canonical (txid,vout) order.
 * BYTE-IDENTICAL serialisation to utxo_projection_commitment (the read-flip
 * relies on this matching the oracle gettxoutsetinfo commitment). Returns 0 on
 * success, -1 on error. */
int coins_kv_commitment(struct sqlite3 *db, uint8_t out[32]);

#endif /* STORAGE_COINS_KV_H */
