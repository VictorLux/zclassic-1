/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_SNAPSHOT_OFFER_READY_H
#define ZCL_CONDITIONS_SNAPSHOT_OFFER_READY_H

void register_snapshot_offer_ready(void);

#ifdef ZCL_TESTING
struct snapshot_sync_service;
void snapshot_offer_ready_test_reset(void);
void snapshot_offer_ready_test_set_service(struct snapshot_sync_service *svc);
int snapshot_offer_ready_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_SNAPSHOT_OFFER_READY_H */
