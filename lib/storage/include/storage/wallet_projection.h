/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_STORAGE_WALLET_PROJECTION_H
#define ZCL_STORAGE_WALLET_PROJECTION_H

#include "storage/event_log.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct wallet_projection wallet_projection_t;

wallet_projection_t *wallet_projection_open(const char *projection_path,
                                            event_log_t *log);
void wallet_projection_close(wallet_projection_t *p);

uint64_t wallet_projection_catch_up(wallet_projection_t *p);

uint64_t wallet_projection_address_count(wallet_projection_t *p);
uint64_t wallet_projection_tx_count(wallet_projection_t *p);
uint64_t wallet_projection_utxo_count(wallet_projection_t *p);
uint64_t wallet_projection_note_count(wallet_projection_t *p);
int64_t wallet_projection_total_value_zat(wallet_projection_t *p);

void wallet_projection_set_event_log(event_log_t *log);
event_log_t *wallet_projection_event_log(void);
wallet_projection_t *wallet_projection_current(void);

struct json_value;
bool wallet_projection_dump_state_json(struct json_value *out,
                                       const char *key);

#endif /* ZCL_STORAGE_WALLET_PROJECTION_H */
