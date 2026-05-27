/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Process-global conservation ledger for the shadow pipeline. See
 * adapters/inbound/shadow_conservation.h for the invariant and the
 * quiesced-state caveat. The counters observe; they never gate. */

#include "adapters/inbound/shadow_conservation.h"

#include <stdatomic.h>

/* Three relaxed counters. Relaxed is correct here: each counter is an
 * independent monotonically-increasing total, and the conservation check
 * is a quiesced-state snapshot, not a synchronization point. No counter
 * gates the pipeline, so there is nothing to order against. */
static atomic_ulong g_fed     = 0;
static atomic_ulong g_diffed  = 0;
static atomic_ulong g_skipped = 0;

void shadow_conservation_record_fed(unsigned long n)
{
    if (n == 0) return;
    atomic_fetch_add_explicit(&g_fed, n, memory_order_relaxed);
}

void shadow_conservation_record_skipped(unsigned long n)
{
    if (n == 0) return;
    atomic_fetch_add_explicit(&g_skipped, n, memory_order_relaxed);
}

void shadow_conservation_record_diffed(unsigned long n)
{
    if (n == 0) return;
    atomic_fetch_add_explicit(&g_diffed, n, memory_order_relaxed);
}

void shadow_conservation_snapshot(unsigned long *fed,
                                  unsigned long *diffed,
                                  unsigned long *skipped)
{
    if (fed)     *fed     = atomic_load_explicit(&g_fed, memory_order_relaxed);
    if (diffed)  *diffed  = atomic_load_explicit(&g_diffed, memory_order_relaxed);
    if (skipped) *skipped = atomic_load_explicit(&g_skipped, memory_order_relaxed);
}

bool shadow_conservation_ok(unsigned long *fed,
                            unsigned long *diffed,
                            unsigned long *skipped)
{
    unsigned long f = atomic_load_explicit(&g_fed, memory_order_relaxed);
    unsigned long d = atomic_load_explicit(&g_diffed, memory_order_relaxed);
    unsigned long s = atomic_load_explicit(&g_skipped, memory_order_relaxed);
    if (fed)     *fed     = f;
    if (diffed)  *diffed  = d;
    if (skipped) *skipped = s;
    /* The conservation law: every fed block was diffed. `skipped`
     * (backpressure drops) is reported for honesty but is NOT folded
     * into the equality — a skipped block was never pushed into the
     * diff path on this pass, so it should not be expected on the
     * diffed side. */
    return f == d;
}

void shadow_conservation_reset(void)
{
    atomic_store_explicit(&g_fed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_diffed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_skipped, 0, memory_order_relaxed);
}
