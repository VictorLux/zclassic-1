/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * shadow_feeder_global — process-wide anchor for the shadow_feeder so
 * that hot ingest paths in lib/net can post observations without
 * threading a feeder pointer through the message processor.
 *
 * Lifecycle:
 *   - At boot, when the -shadow flag is set, the boot code calls
 *     shadow_feeder_global_set(f). Until that point the global is
 *     NULL and shadow_feeder_global_observe() is a no-op.
 *   - At shutdown, the boot code calls shadow_feeder_global_set(NULL)
 *     *before* destroying the feeder, so the global is cleared before
 *     the underlying object goes away.
 *
 * Concurrency: the global is an atomic pointer. observe() snapshots it
 * once per call. If shutdown clears the pointer concurrently with an
 * observe() call, the observe sees either the live feeder (and that
 * call completes against a still-alive object) or NULL (no-op). The
 * shutdown sequence MUST quiesce ingest before destroying the feeder
 * — see boot_services for the ordering.
 */

#ifndef ZCL_ADAPTERS_INBOUND_SHADOW_FEEDER_GLOBAL_H
#define ZCL_ADAPTERS_INBOUND_SHADOW_FEEDER_GLOBAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block;
struct shadow_feeder;

/* Install (or clear, with NULL) the process-wide shadow_feeder. The
 * caller retains ownership of the feeder. Setting the global to a new
 * non-NULL value when one is already installed is allowed (overwrite),
 * but typical wiring sets exactly once at boot and clears once at
 * shutdown. */
void shadow_feeder_global_set(struct shadow_feeder *f);

/* No-op when the global is unset. Otherwise delegates to
 * shadow_feeder_observe_block. Errors from the feeder are swallowed
 * here — the hot path is a fire-and-forget observer; backpressure and
 * append failures are observable via shadow_feeder counters and event
 * emission inside the feeder, not via this call's return value. */
void shadow_feeder_global_observe(uint32_t height,
                                  const struct block *block,
                                  const uint8_t *bytes,
                                  size_t len);

/* Diagnostic: is a feeder currently installed? */
bool shadow_feeder_global_is_active(void);

#endif /* ZCL_ADAPTERS_INBOUND_SHADOW_FEEDER_GLOBAL_H */
