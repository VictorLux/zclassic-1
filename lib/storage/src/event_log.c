/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * event_log — append-only event log implementation.
 *
 * Stub (Task 1) — public API surface only. Real I/O lands in Task 2.
 * See storage/event_log.h for the wire format and recovery contract.
 */

#include "storage/event_log.h"

#include "util/log_macros.h"

#include <stdlib.h>

struct event_log {
    /* Real fields land in Task 2. */
    int placeholder;
};

event_log_t *event_log_open(const char *path)
{
    (void)path;
    /* Stub: not yet implemented. */
    return NULL;
}

void event_log_close(event_log_t *log)
{
    (void)log;
}

uint64_t event_log_append(event_log_t *log,
                          enum event_type type,
                          const void *payload, size_t payload_len)
{
    (void)log; (void)type; (void)payload; (void)payload_len;
    return UINT64_MAX;
}

int event_log_read(event_log_t *log, uint64_t offset,
                   enum event_type *type_out,
                   void *buf, size_t buf_cap, size_t *out_len)
{
    (void)log; (void)offset; (void)type_out;
    (void)buf; (void)buf_cap; (void)out_len;
    return -1;
}

int event_log_stream(event_log_t *log, uint64_t start_offset,
                     event_log_cb cb, void *user)
{
    (void)log; (void)start_offset; (void)cb; (void)user;
    return -1;
}

int event_log_fingerprint(event_log_t *log, uint8_t out[32])
{
    (void)log; (void)out;
    return -1;
}

uint64_t event_log_size(event_log_t *log)
{
    (void)log;
    return 0;
}
