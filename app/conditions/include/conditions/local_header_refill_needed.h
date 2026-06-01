/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_LOCAL_HEADER_REFILL_NEEDED_H
#define ZCL_CONDITIONS_LOCAL_HEADER_REFILL_NEEDED_H

void register_local_header_refill_needed(void);

#ifdef ZCL_TESTING
void local_header_refill_needed_test_reset(void);
int local_header_refill_needed_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_LOCAL_HEADER_REFILL_NEEDED_H */
