/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_stage — Wave S, S-8 shadow stage.
 *
 * Consumes `proof_validate_log`; for each height where proof validation
 * passed, computes the transparent UTXO delta and records the result.
 * Shadow mode: no mutation of consensus state. */

#ifndef ZCL_SERVICES_UTXO_APPLY_STAGE_H
#define ZCL_SERVICES_UTXO_APPLY_STAGE_H

#include "core/uint256.h"
#include "util/stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block;
struct block_index;
struct json_value;
struct main_state;
struct tx_out;

#define UTXO_APPLY_BATCH_PER_TICK 100

struct utxo_apply_lookup {
    bool found;
    int64_t value;
};

typedef bool (*utxo_apply_reader_fn)(struct block *out,
                                     const struct block_index *bi,
                                     const char *datadir,
                                     void *user);

typedef bool (*utxo_apply_lookup_fn)(const struct uint256 *txid,
                                     uint32_t vout,
                                     struct utxo_apply_lookup *out,
                                     void *user);

typedef bool (*utxo_apply_live_check_fn)(int height,
                                         size_t spent_count,
                                         size_t added_count,
                                         int64_t total_value_delta,
                                         const char **out_detail,
                                         void *user);

bool utxo_apply_stage_init(struct main_state *ms);
void utxo_apply_stage_shutdown(void);

stage_result_t utxo_apply_stage_step_once(void);
int utxo_apply_stage_drain(int max_steps);

uint64_t utxo_apply_stage_cursor(void);
uint64_t utxo_apply_stage_verified_total(void);
uint64_t utxo_apply_stage_spend_unknown_total(void);
uint64_t utxo_apply_stage_utxo_collision_total(void);
uint64_t utxo_apply_stage_value_overflow_total(void);
uint64_t utxo_apply_stage_delta_diverged_total(void);
uint64_t utxo_apply_stage_upstream_failed_total(void);
uint64_t utxo_apply_stage_internal_error_total(void);
uint64_t utxo_apply_stage_outputs_added_total(void);
uint64_t utxo_apply_stage_outputs_spent_total(void);

void utxo_apply_stage_set_reader(utxo_apply_reader_fn fn, void *user);
void utxo_apply_stage_set_lookup(utxo_apply_lookup_fn fn, void *user);
void utxo_apply_stage_set_live_checker(utxo_apply_live_check_fn fn, void *user);

bool utxo_apply_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SERVICES_UTXO_APPLY_STAGE_H */
