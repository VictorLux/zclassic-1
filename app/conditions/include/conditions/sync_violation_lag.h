/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_SYNC_VIOLATION_LAG_H
#define ZCL_CONDITIONS_SYNC_VIOLATION_LAG_H

void register_sync_violation_lag(void);

#ifdef ZCL_TESTING
void sync_violation_lag_test_reset(void);
int sync_violation_lag_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_SYNC_VIOLATION_LAG_H */
