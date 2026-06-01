/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_SYNC_STATE_STUCK_H
#define ZCL_CONDITIONS_SYNC_STATE_STUCK_H

void register_sync_state_stuck(void);

#ifdef ZCL_TESTING
void sync_state_stuck_test_reset(void);
int sync_state_stuck_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_SYNC_STATE_STUCK_H */
