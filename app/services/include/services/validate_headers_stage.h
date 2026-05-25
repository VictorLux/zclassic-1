/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_stage — Wave S, S-3.
 *
 * Second saga stage. Consumes `header_admit_log` (produced by S-2) and
 * records the result of full header validation in `validate_headers_log`.
 * In authoritative mode, a passing stage row also marks the in-memory
 * block_index entry as at least BLOCK_VALID_HEADER; legacy ingress becomes
 * a divergence guard.
 *
 * Validation pipeline (per height)
 * --------------------------------
 *  1. Read `(height, hash)` from `header_admit_log`.
 *  2. Look up `active_chain_at(ms, height)` for the in-memory
 *     `struct block_index` (carries nVersion / nBits / nNonce /
 *     hashMerkleRoot / nTime / nFile / nDataPos — but NOT the
 *     Equihash solution; block_index intentionally drops nSolution
 *     to save RAM, see lib/storage/src/block_index_db.c:301).
 *  3. Validate cheap fields first:
 *       - nVersion ≥ MIN_BLOCK_VERSION
 *       - CheckProofOfWork(hash, nBits)   (target check, no Equihash)
 *  4. Read the full block from disk via
 *     `read_block_from_disk_index_pread()` (POSIX pread, thread-safe).
 *  5. Validate the Equihash solution against (N, K) from the active
 *     chain params at this height.
 *  6. Write a `validate_headers_log` row in the same `BEGIN IMMEDIATE`
 *     transaction that bumps the cursor (F-2 saga contract).
 *
 * Worker pool
 * -----------
 * Steps 3–5 are CPU+I/O bound (Equihash is ~ms per block). The stage
 * runs a fixed pool of `VH_POOL_SIZE` pthread workers; each step
 * submits up to `VH_BATCH_SIZE` jobs, awaits them all, then writes the
 * batch + cursor bump atomically. Workers are created at init and
 * joined at shutdown.
 *
 * Cursor floor
 * ------------
 * The stage never validates beyond the header_admit cursor. If
 * `vh_cursor + VH_BATCH_SIZE > header_admit_cursor`, the batch shrinks
 * to whatever is available; if zero, the step returns STAGE_IDLE.
 *
 * Schema
 * -------
 *   CREATE TABLE IF NOT EXISTS validate_headers_log (
 *     height       INTEGER PRIMARY KEY,
 *     hash         BLOB    NOT NULL,
 *     ok           INTEGER NOT NULL,   -- 0 = failed, 1 = passed
 *     fail_reason  TEXT,               -- NULL when ok=1
 *     validated_at INTEGER NOT NULL
 *   );
 *
 * Lifecycle
 * ---------
 * `init` binds the stage to a `main_state`, ensures the schema, and
 * spins up the worker pool. `step_once` runs one batched step.
 * `shutdown` joins the workers, disposes the stage, and resets per-init
 * observability counters (the persisted cursor + log rows are not
 * touched). The supervisor wiring lives in
 * `config/src/boot_services.c` — `staged.validate_headers` is
 * registered with `period_secs=2`. */

#ifndef ZCL_SERVICES_VALIDATE_HEADERS_STAGE_H
#define ZCL_SERVICES_VALIDATE_HEADERS_STAGE_H

#include "util/stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct main_state;
struct block_index;
struct uint256;
struct json_value;

#define VH_POOL_SIZE         4
#define VH_BATCH_SIZE        8
#define VH_MAX_REASON       64
#define VH_BATCH_PER_TICK   64

/* Test seam: injectable validator. The default validator runs the full
 * PoW + Equihash pipeline against disk; tests inject a stub that
 * decides purely from in-memory fields.
 *
 * Contract: writes a NUL-terminated reason string into `out_reason`
 * on failure (≤ VH_MAX_REASON-1 chars). On success may leave it empty.
 * Returns true on pass. Must be reentrant-safe (called from N workers
 * concurrently). */
typedef bool (*vh_validator_fn)(const struct block_index *bi,
                                 const char *datadir,
                                 char *out_reason,
                                 size_t out_reason_size,
                                 void *user);

typedef enum {
    VALIDATE_HEADERS_MODE_SHADOW = 0,
    VALIDATE_HEADERS_MODE_AUTHORITATIVE
} validate_headers_mode_t;

void validate_headers_set_mode(validate_headers_mode_t mode);
validate_headers_mode_t validate_headers_get_mode(void);

/* Bind the stage to `ms`, ensure the validate_headers_log schema, and
 * launch VH_POOL_SIZE workers. Idempotent — a second call against the
 * same `ms` returns true. Requires `progress_store_open` first. */
bool validate_headers_stage_init(struct main_state *ms);

/* Inject a non-default validator. Must be called BETWEEN init and the
 * first step. Reset to the default by `shutdown`. */
void validate_headers_stage_set_validator(vh_validator_fn fn, void *user);

stage_result_t validate_headers_stage_step_once(void);
int            validate_headers_stage_drain(int max_steps);
void           validate_headers_stage_shutdown(void);

uint64_t validate_headers_stage_cursor(void);
uint64_t validate_headers_stage_passed_total(void);
uint64_t validate_headers_stage_failed_total(void);

bool validate_headers_stage_has_pass_record(int32_t height,
                                            const struct uint256 *hash);

struct validate_headers_window_report {
    bool available;
    bool complete;
    int64_t start_height;
    int64_t end_height;
    int64_t expected_count;
    int64_t checked_count;
    int64_t failed_count;
    int64_t first_failed_height;
    char first_fail_reason[VH_MAX_REASON];
};

bool validate_headers_stage_window_report(
    int64_t start_height,
    int64_t end_height,
    struct validate_headers_window_report *out);

bool validate_headers_stage_dump_state_json(struct json_value *out,
                                             const char *key);

#endif /* ZCL_SERVICES_VALIDATE_HEADERS_STAGE_H */
