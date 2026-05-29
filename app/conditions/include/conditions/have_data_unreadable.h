/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_HAVE_DATA_UNREADABLE_H
#define ZCL_CONDITIONS_HAVE_DATA_UNREADABLE_H

void register_have_data_unreadable(void);

#ifdef ZCL_TESTING
void have_data_unreadable_test_reset(void);
int have_data_unreadable_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_HAVE_DATA_UNREADABLE_H */
