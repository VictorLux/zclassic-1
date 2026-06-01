/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_HEADER_STALL_AT_HEIGHT_H
#define ZCL_CONDITIONS_HEADER_STALL_AT_HEIGHT_H

void register_header_stall_at_height(void);

#ifdef ZCL_TESTING
void header_stall_at_height_test_reset(void);
int header_stall_at_height_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_HEADER_STALL_AT_HEIGHT_H */
