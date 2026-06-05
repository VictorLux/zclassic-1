/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_stage — reducer Job stage.
 *
 * Consumes `body_persist_log`; for each height where the body was
 * verified-on-disk, runs script verification on every input and logs the
 * result. */

#ifndef ZCL_SERVICES_SCRIPT_VALIDATE_STAGE_H
#define ZCL_SERVICES_SCRIPT_VALIDATE_STAGE_H

#include "core/amount.h"
#include "core/uint256.h"
#include "jobs/stage_helpers.h"
#include "primitives/transaction.h"
#include "util/stage.h"

#include <stdbool.h>
#include <stdint.h>

struct block;
struct block_index;
struct json_value;
struct main_state;

#define SCRIPT_VALIDATE_BATCH_PER_TICK 100

/* Alias onto the shared stage reader type (jobs/stage_helpers.h); the public
 * setter name script_validate_stage_set_reader is unchanged. */
typedef stage_block_reader_fn script_validate_reader_fn;

typedef bool (*script_validate_prevout_fn)(const struct outpoint *prevout,
                                           struct tx_out *out,
                                           void *user);

bool script_validate_stage_init(struct main_state *ms);
void script_validate_stage_shutdown(void);

job_result_t script_validate_stage_step_once(void);
int script_validate_stage_drain(int max_steps);

uint64_t script_validate_stage_cursor(void);
uint64_t script_validate_stage_verified_total(void);
uint64_t script_validate_stage_script_invalid_total(void);
uint64_t script_validate_stage_internal_error_total(void);
uint64_t script_validate_stage_upstream_failed_total(void);
uint64_t script_validate_stage_inputs_verified_total(void);
uint64_t script_validate_stage_inputs_failed_total(void);

void script_validate_stage_set_reader(script_validate_reader_fn fn,
                                      void *user);
void script_validate_stage_set_prevout_resolver(script_validate_prevout_fn fn,
                                                void *user);

bool script_validate_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SERVICES_SCRIPT_VALIDATE_STAGE_H */
