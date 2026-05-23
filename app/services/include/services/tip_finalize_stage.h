/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_stage — Wave S, S-9 shadow stage.
 *
 * Consumes `utxo_apply_log`; for each height where UTXO apply passed,
 * observes that the live chain has advanced to the next active tip and
 * records the finalize result. Shadow mode: no mutation of consensus state. */

#ifndef ZCL_SERVICES_TIP_FINALIZE_STAGE_H
#define ZCL_SERVICES_TIP_FINALIZE_STAGE_H

#include "util/stage.h"

#include <stdbool.h>
#include <stdint.h>

struct json_value;
struct main_state;

#define TIP_FINALIZE_BATCH_PER_TICK 100

typedef bool (*tip_finalize_utxo_count_fn)(int height_after,
                                           int64_t *out_count,
                                           void *user);

bool tip_finalize_stage_init(struct main_state *ms);
void tip_finalize_stage_shutdown(void);

stage_result_t tip_finalize_stage_step_once(void);
int tip_finalize_stage_drain(int max_steps);

uint64_t tip_finalize_stage_cursor(void);
uint64_t tip_finalize_stage_finalized_total(void);
uint64_t tip_finalize_stage_upstream_failed_total(void);
uint64_t tip_finalize_stage_reorg_detected_total(void);
uint64_t tip_finalize_stage_utxo_count_diverged_total(void);
uint64_t tip_finalize_stage_precondition_failed_total(void);
uint64_t tip_finalize_stage_total_work_added_high(void);
uint64_t tip_finalize_stage_total_work_added_low(void);

void tip_finalize_stage_set_utxo_counter(tip_finalize_utxo_count_fn fn,
                                         void *user);

bool tip_finalize_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SERVICES_TIP_FINALIZE_STAGE_H */
