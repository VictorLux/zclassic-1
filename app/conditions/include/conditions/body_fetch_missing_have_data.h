/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_BODY_FETCH_MISSING_HAVE_DATA_H
#define ZCL_CONDITIONS_BODY_FETCH_MISSING_HAVE_DATA_H

void register_body_fetch_missing_have_data(void);

#ifdef ZCL_TESTING
void body_fetch_missing_have_data_test_reset(void);
int body_fetch_missing_have_data_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_BODY_FETCH_MISSING_HAVE_DATA_H */
