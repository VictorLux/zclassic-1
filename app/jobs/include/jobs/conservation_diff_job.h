/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * conservation_diff_job — drives the shadow-pipeline conservation
 * `diffed` counter (cutover finish, Item 3).
 *
 * THE GAP THIS CLOSES
 * -------------------
 * The shadow feeder bumps the process-global `fed` counter for every
 * block it appends to the shadow log and queues for validation (see
 * adapters/inbound/shadow_conservation.h). NOTHING in the live node
 * drives the `diffed` counter, so the conservation law `fed == diffed`
 * — the safety proof that no fed block was silently dropped — can never
 * go green. The live preflight blocker reads e.g. `fed=67 diffed=0`.
 *
 * This Job is the missing driver. It is a cursor-tracked background Job
 * (the fourth shape; see jobs/job.h) registered under the staged-sync
 * supervisor. Per step it takes the next FED-but-not-yet-DIFFED block,
 * reads the canonical block from the co-located zclassicd over RPC
 * (block_log_legacy_rpc — NO LevelDB lock, NO 12.7 GB byte copy),
 * compares it byte-for-byte against the shadow-pipeline's block, and on
 * a GENUINE match drives `shadow_conservation_record_diffed(1)` forward.
 *
 * THE HONESTY GUARDRAIL (NON-NEGOTIABLE)
 * --------------------------------------
 * `diffed` is a SAFETY PROOF. This Job increments it ONLY when a fed
 * block reconciles byte-for-byte against the canonical legacy block (the
 * full serialized block; equal bytes ⟹ equal block hash ⟹ identical
 * consensus content). It NEVER blind-increments diffed to fed and NEVER
 * fakes progress to force the gate green. On a real mismatch it does NOT
 * increment, does NOT advance the cursor, and returns JOB_BLOCKED with
 * the height + reason — a reorg/reconsider can then re-drive the same
 * cursor (transient, never a permanent wedge).
 *
 * Cursor
 * ------
 * The 64-bit cursor = the next shadow-log height to diff, persisted in
 * progress.kv via the F-2 stage primitive (stage name "conservation_diff").
 * Crash-mid-step replays idempotently: the diff is a pure comparison, so
 * re-running a height is harmless, and `diffed` only advances when the
 * cursor advances in the same transaction.
 *
 * Lifecycle: `init` binds the shadow-log dir + opens the legacy RPC read
 * port once per boot (module-static handle); `step_once` runs one diff;
 * `shutdown` closes the RPC port. The supervisor wiring lives in
 * app/supervisors/src/staged_sync_supervisor.c. */

#ifndef ZCL_JOBS_CONSERVATION_DIFF_JOB_H
#define ZCL_JOBS_CONSERVATION_DIFF_JOB_H

#include "jobs/job.h"
#include "ports/block_log_port.h"

#include <stdbool.h>
#include <stdint.h>

struct json_value;

/* Drain budget per supervisor tick. Each step is one shadow-log read +
 * one zclassicd RPC round trip; keep the batch modest so a slow daemon
 * never starves the other supervised children. */
#define CONSERVATION_DIFF_BATCH_PER_TICK 16

/* Bind the Job to the shadow log under `<datadir>/blocks.shadow` and open
 * the legacy RPC read port (open-once-per-boot — see the lifecycle note
 * in the .c). Idempotent: a second call with the same datadir returns
 * true. Requires `progress_store_open` to have succeeded first. Returns
 * false (and the Job is skipped this boot) on bad input or if the shadow
 * log / progress store is unavailable. */
bool conservation_diff_job_init(const char *datadir);

/* Test seam: inject a fixture legacy read port so the test does not need
 * a live zclassicd. Must be called BETWEEN init and the first step; reset
 * to the real RPC port by `shutdown`. Passing NULL restores the default
 * (real RPC) port. */
void conservation_diff_job_set_legacy_port(const struct block_log_port *port);

/* Test seam: inject a fixture shadow read port (the "fed" side). Must be
 * called BETWEEN init and the first step. Passing NULL restores the
 * default (the on-disk shadow log opened by init). */
void conservation_diff_job_set_shadow_port(const struct block_log_port *port);

job_result_t conservation_diff_job_step_once(void);
int          conservation_diff_job_drain(int max_steps);
void         conservation_diff_job_shutdown(void);

uint64_t conservation_diff_job_cursor(void);          /* next height to diff */
uint64_t conservation_diff_job_diffed_total(void);    /* diffs this Job drove */
int64_t  conservation_diff_job_last_blocked_height(void); /* -1 if none */

bool conservation_diff_job_dump_state_json(struct json_value *out,
                                           const char *key);

#endif /* ZCL_JOBS_CONSERVATION_DIFF_JOB_H */
