/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_DOWNLOAD_QUEUE_STARVED_H
#define ZCL_CONDITIONS_DOWNLOAD_QUEUE_STARVED_H

void register_download_queue_starved(void);

#ifdef ZCL_TESTING
void download_queue_starved_test_reset(void);
int download_queue_starved_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_DOWNLOAD_QUEUE_STARVED_H */
