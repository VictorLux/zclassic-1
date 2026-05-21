/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Test-only crash-injection hooks for connect_tip's ordering protocol.
 *
 * The atomicity test (test_chain_advance_atomicity.c) forks a child,
 * arms a crash stage with `process_block_test_set_crash_stage(...)`,
 * runs one block through connect_tip, and the child `_exit(137)`s at
 * the named protocol point. Production never sets a stage; the
 * atomic_load + branch per check site is the only cost (negligible).
 *
 * Split out of process_block.c — pure code motion from the original
 * file. See <validation/process_block.h> for the stage semantics. */

#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "validation/process_block.h"
#include "process_block_internal.h"

/* Test-only crash-injection hook. The atomic is referenced by the
 * static inline process_block_check_crash_stage() in the internal
 * header — keeping the storage here means every TU that includes the
 * header reads the same global. */
_Atomic int g_test_crash_stage_storage = PBCS_NONE;

void process_block_test_set_crash_stage(enum process_block_crash_stage s)
{
    if (s < PBCS_NONE || s >= PBCS_NUM_STAGES) s = PBCS_NONE;
    atomic_store_explicit(&g_test_crash_stage_storage, (int)s,
                          memory_order_relaxed);
}

enum process_block_crash_stage process_block_test_get_crash_stage(void)
{
    return (enum process_block_crash_stage)
        atomic_load_explicit(&g_test_crash_stage_storage,
                             memory_order_relaxed);
}

const char *process_block_crash_stage_name(enum process_block_crash_stage s)
{
    switch (s) {
    case PBCS_NONE:                    return "none";
    case PBCS_AFTER_CONNECT_BLOCK:     return "after_connect_block";
    case PBCS_AFTER_COINS_VIEW_FLUSH:  return "after_coins_view_flush";
    case PBCS_AFTER_UPDATE_TIP:        return "after_update_tip";
    case PBCS_AFTER_COINS_DISK_FLUSH:  return "after_coins_disk_flush";
    case PBCS_AFTER_BLOCK_INDEX_WRITE: return "after_block_index_write";
    default:                           return "unknown";
    }
}

/* Out-of-line variant for chain_advance.c (which lives in a different
 * translation unit and can't reach the static inline). */
void process_block_check_crash_stage_ext(enum process_block_crash_stage here)
{
    process_block_check_crash_stage(here);
}
