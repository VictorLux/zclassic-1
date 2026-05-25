/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_WATCHDOG_DISSOLVE_PR3_H
#define ZCL_CONDITIONS_WATCHDOG_DISSOLVE_PR3_H

#ifdef ZCL_TESTING
void peer_floor_violated_test_reset(void);
int peer_floor_violated_test_remedy_calls(void);
void sync_violation_lag_test_reset(void);
int sync_violation_lag_test_remedy_calls(void);
struct snapshot_sync_service;
void snapshot_offer_ready_test_reset(void);
void snapshot_offer_ready_test_set_service(struct snapshot_sync_service *svc);
int snapshot_offer_ready_test_remedy_calls(void);
void cutover_no_forward_progress_test_reset(void);
int cutover_no_forward_progress_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_WATCHDOG_DISSOLVE_PR3_H */
