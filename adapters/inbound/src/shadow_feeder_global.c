/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "adapters/inbound/shadow_feeder_global.h"
#include "adapters/inbound/shadow_feeder.h"

#include <stdatomic.h>

static _Atomic(struct shadow_feeder *) g_feeder = NULL;

void shadow_feeder_global_set(struct shadow_feeder *f)
{
    atomic_store_explicit(&g_feeder, f, memory_order_release);
}

bool shadow_feeder_global_is_active(void)
{
    return atomic_load_explicit(&g_feeder, memory_order_acquire) != NULL;
}

void shadow_feeder_global_observe(uint32_t height,
                                  const struct block *block,
                                  const uint8_t *bytes,
                                  size_t len)
{
    struct shadow_feeder *f =
        atomic_load_explicit(&g_feeder, memory_order_acquire);
    if (!f || !block)
        return;
    /* Fire-and-forget — counters/blockers inside the feeder surface
     * backpressure and append failures. The hot ingest path must not
     * stall on shadow observation. */
    (void)shadow_feeder_observe_block(f, height, block, bytes, len);
}
