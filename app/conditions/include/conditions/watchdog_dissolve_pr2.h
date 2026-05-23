/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_WATCHDOG_DISSOLVE_PR2_H
#define ZCL_CONDITIONS_WATCHDOG_DISSOLVE_PR2_H

#ifdef ZCL_TESTING
void header_stall_at_height_test_reset(void);
int header_stall_at_height_test_remedy_calls(void);
void sync_state_stuck_test_reset(void);
int sync_state_stuck_test_remedy_calls(void);
void download_queue_starved_test_reset(void);
int download_queue_starved_test_remedy_calls(void);
void local_header_refill_needed_test_reset(void);
int local_header_refill_needed_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_WATCHDOG_DISSOLVE_PR2_H */
