/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_CUTOVER_MODES_H
#define ZCL_SERVICES_CUTOVER_MODES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CUTOVER_STAGE_MODE_SHADOW = 0,
    CUTOVER_STAGE_MODE_AUTHORITATIVE
} cutover_stage_mode_t;

#define CUTOVER_CANARY_WATCH_SECS 180

struct cutover_canary_snapshot {
    bool has_change;
    bool authoritative_active;
    bool passed;
    bool failed;
    int64_t changed_at_unix;
    int64_t change_height;
    int64_t target_height;
    int64_t current_tip_height;
    int64_t elapsed_seconds;
    int64_t deadline_unix;
    int64_t change_header_height;
    int64_t change_peer_best_height;
    int64_t change_tip_lag;
    int64_t watch_window_seconds;
};

void cutover_modes_set_header_admit(cutover_stage_mode_t mode);
void cutover_modes_set_validate_headers(cutover_stage_mode_t mode);
void cutover_modes_set_header_pipeline(cutover_stage_mode_t header_admit,
                                       cutover_stage_mode_t validate_headers);

cutover_stage_mode_t cutover_modes_get_header_admit(void);
cutover_stage_mode_t cutover_modes_get_validate_headers(void);
bool cutover_modes_any_authoritative_active(void);

void cutover_modes_record_change(int64_t height,
                                 int64_t header_height,
                                 int64_t peer_best_height,
                                 int64_t tip_lag);
void cutover_modes_canary_snapshot(int64_t current_tip_height,
                                   struct cutover_canary_snapshot *out);
bool cutover_modes_canary_target_reached(int64_t current_tip_height);

#ifdef ZCL_TESTING
void cutover_modes_test_reset(void);
#endif

#endif /* ZCL_SERVICES_CUTOVER_MODES_H */
