/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_PEER_FLOOR_VIOLATED_H
#define ZCL_CONDITIONS_PEER_FLOOR_VIOLATED_H

void register_peer_floor_violated(void);

#ifdef ZCL_TESTING
void peer_floor_violated_test_reset(void);
int peer_floor_violated_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_PEER_FLOOR_VIOLATED_H */
