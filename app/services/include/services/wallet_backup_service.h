/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet Backup Service — periodic, rotated, verified backups
 * of the wallet_* tables inside node.db.
 *
 * Motivation
 * ----------
 * On 2026-04-10 an interactive debug procedure deleted node.db
 * and its WAL/SHM sidecar files while the node was running a
 * UTXO reimport. That wiped the `wallet_keys` and
 * `wallet_sapling_keys` rows, which are the sole source of truth
 * for the user's private keys. The next boot generated fresh
 * keys; the 0.4 ZCL already sent to the original address became
 * permanently unspendable.
 *
 * The safety rails we've built so far (recovery_policy, db_txn,
 * chain_state_repository, block_index_integrity) all make the
 * *running* node safer. None of them defend against an operator
 * mistake that takes the whole database file out of play before
 * the rails get a chance to run. The fix is an always-on,
 * always-external, always-versioned backup of just the wallet
 * tables — a copy the user can restore from even if node.db is
 * gone, corrupt, or deleted by mistake.
 *
 * Design
 * ------
 *
 *   - A background thread started from boot. Every
 *     `interval_seconds` the thread copies the six wallet tables
 *     (`wallet_keys`, `wallet_sapling_keys`, `wallet_seed`,
 *     `wallet_scripts`, `wallet_transactions`, `wallet_utxos`,
 *     `wallet_sapling_notes`) via ATTACH + `CREATE TABLE AS
 *     SELECT` into a `wallet_backup_<unix_ts>.sqlite` file in
 *     `backup_dir`.
 *   - After each write the service reopens the copy, counts the
 *     rows it wrote, and verifies the count matches the source.
 *     Mismatch → EV_WALLET_BACKUP_FAILED and the file is left on
 *     disk for forensic inspection.
 *   - Rotation: if the count of `wallet_backup_*.sqlite` files in
 *     `backup_dir` exceeds `max_versions`, the oldest is deleted.
 *     The newest is always kept.
 *   - `wallet_backup_now()` runs one backup synchronously. It's
 *     the same code path the thread uses, so RPC callers and the
 *     thread share a single failure mode.
 *
 * Encryption
 * ----------
 * Phase 1 ships unencrypted. Phase 2 will add AES-256-GCM via
 * scrypt-derived keys. The header already carries the hooks so a
 * follow-up commit can land the crypto path without API churn.
 *
 * Thread safety
 * -------------
 * The service owns its own pthread + a mutex that guards
 * start/stop/now calls. `wallet_backup_now()` is safe to call
 * from any thread and blocks until the one-shot backup completes.
 */

#ifndef ZCL_SERVICES_WALLET_BACKUP_SERVICE_H
#define ZCL_SERVICES_WALLET_BACKUP_SERVICE_H

#include "models/database.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Tunables ───────────────────────────────────────────────── */

#define WALLET_BACKUP_DEFAULT_INTERVAL_SEC 3600   /* 1 hour */
#define WALLET_BACKUP_DEFAULT_MAX_VERSIONS 168    /* 1 week @ hourly */
#define WALLET_BACKUP_FILENAME_PREFIX "wallet_backup_"
#define WALLET_BACKUP_FILENAME_SUFFIX ".sqlite"

/* ── Config ─────────────────────────────────────────────────── */

struct wallet_backup_config {
    const char *backup_dir;        /* absolute path, created 0700 if missing */
    int         interval_seconds;  /* 0 = use default */
    int         max_versions;      /* 0 = use default */
    bool        encrypt;           /* phase 2 only — ignored today */
    const char *encrypt_password;  /* env WALLET_BACKUP_PASSWORD if encrypt */
};

void wallet_backup_config_defaults(struct wallet_backup_config *cfg);

/* ── Status snapshot (read-only) ────────────────────────────── */

struct wallet_backup_status {
    bool    running;               /* thread is active */
    int64_t total_runs;            /* successful backups since start */
    int64_t total_failures;        /* emitted EV_WALLET_BACKUP_FAILED */
    int64_t last_run_unix;         /* wall-clock time of last success, 0 if none */
    int64_t last_size_bytes;       /* size of last successful backup */
    int64_t last_key_count;        /* wallet_keys rows in last backup */
    int64_t last_duration_ms;      /* elapsed ms of the last run */
    char    last_path[512];        /* absolute path of last backup */
    char    last_error[256];       /* most recent failure reason */
};

void wallet_backup_status_snapshot(struct wallet_backup_status *out);

/* ── Lifecycle ──────────────────────────────────────────────── */

/* Start the background backup thread. `db` is the opened node_db
 * that owns the wallet tables. If the service is already running,
 * this is a no-op and returns true. Returns false on any setup
 * error (missing backup_dir permission, thread create failure,
 * etc). Safe to call from any thread. */
bool wallet_backup_start(const struct wallet_backup_config *cfg,
                          struct node_db *db);

/* Stop the background thread. Safe to call even if not running. */
void wallet_backup_stop(void);

/* Run one backup synchronously. Returns true on success. Callable
 * whether or not the thread is running. Safe to call from any
 * thread — serialised by the service mutex. */
bool wallet_backup_now(void);

/* ── Low-level primitive (testable) ─────────────────────────── */

/* Create one backup file in `backup_dir` reading from `db`. On
 * success, writes the full path to `out_path` (if non-NULL) and
 * the wallet_keys row count to `out_key_count` (if non-NULL).
 * Returns false on any IO/SQL error. This is the single
 * entry point used by both the thread and wallet_backup_now,
 * exposed so tests can call it directly without spinning a
 * thread.
 *
 * err_out/err_cap take a caller-provided diagnostic buffer — the
 * same shape as bii_verify — so tests can assert the failure
 * reason without reading the event ring. */
bool wallet_backup_run_once(const char *backup_dir,
                             struct node_db *db,
                             char *out_path, size_t out_path_cap,
                             int64_t *out_key_count,
                             char *err_out, size_t err_cap);

/* Apply rotation in `backup_dir`, deleting the oldest
 * `wallet_backup_<ts>.sqlite` files until count <= max_versions.
 * Returns the number of files deleted. */
int wallet_backup_rotate(const char *backup_dir, int max_versions);

/* List `wallet_backup_<ts>.sqlite` files in `backup_dir`, newest
 * first. `out_paths` is a caller-provided array of `max` strings,
 * each `path_cap` bytes wide. Returns the number written. */
int wallet_backup_list(const char *backup_dir,
                        char (*out_paths)[512], int max);

#endif /* ZCL_SERVICES_WALLET_BACKUP_SERVICE_H */
