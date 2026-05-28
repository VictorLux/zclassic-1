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
void cutover_modes_set_tip_finalize(cutover_stage_mode_t mode);
void cutover_modes_set_header_pipeline(cutover_stage_mode_t header_admit,
                                       cutover_stage_mode_t validate_headers);
void cutover_modes_revert_all_to_shadow(void);

cutover_stage_mode_t cutover_modes_get_header_admit(void);
cutover_stage_mode_t cutover_modes_get_validate_headers(void);
cutover_stage_mode_t cutover_modes_get_tip_finalize(void);
bool cutover_modes_any_authoritative_active(void);

void cutover_modes_record_change(int64_t height,
                                 int64_t header_height,
                                 int64_t peer_best_height,
                                 int64_t tip_lag);
void cutover_modes_canary_snapshot(int64_t current_tip_height,
                                   struct cutover_canary_snapshot *out);
bool cutover_modes_canary_target_reached(int64_t current_tip_height);
void cutover_modes_clear_canary(void);

#ifdef ZCL_TESTING
void cutover_modes_test_reset(void);
#endif

/* See CLAUDE.md "Adding state introspection". Reentrant-safe,
 * non-allocating, READ-ONLY. Consolidates the cheap, lock-free cutover
 * flip state into one `zcl_state subsystem=cutover` snapshot: per-stage
 * modes (header_admit / validate_headers, shadow vs authoritative) plus
 * authoritative_active, the recorded canary change anchor (atomic loads
 * only — no live tip), and the shadow-pipeline conservation counters
 * (fed / diffed / skipped / conserved). The tip-relative `passed`/`failed`
 * canary verdict and the full `ready` gate breakdown stay on the heavier
 * `cutoverpreflight` RPC (it runs a header_admit diff + node_health
 * collect — too heavy / not reentrant-safe for a dumper). */
struct json_value;
bool cutover_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SERVICES_CUTOVER_MODES_H */
