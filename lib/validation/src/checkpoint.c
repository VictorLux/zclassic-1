/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "validation/checkpoint.h"
#include "validation/main_constants.h"

bool reorg_is_allowed(int tip_h, int target_fork_h,
                      const char **reason_out)
{
    /* No tip: any "reorg" is just initial download — allow. */
    if (tip_h < 0) {
        if (reason_out) *reason_out = "no_tip";
        return true;
    }
    /* Forward-extension (target above tip) is not a reorg — allow. */
    if (target_fork_h >= tip_h) {
        if (reason_out) *reason_out = "no_disconnect";
        return true;
    }
    int depth = tip_h - target_fork_h;
    if (depth <= MAX_REORG_LENGTH) {
        if (reason_out) *reason_out = "within_max_reorg";
        return true;
    }
    if (reason_out) *reason_out = "below_checkpoint";
    return false;
}

bool height_is_immutable(int tip_h, int h)
{
    if (tip_h < 0) return false;
    return h <= tip_h - MAX_REORG_LENGTH;
}
