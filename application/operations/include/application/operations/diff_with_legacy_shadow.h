/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * application/operations/diff_with_legacy_shadow.h
 *
 * Compare two block_log_port views height-by-height. Used to gate
 * the Epoch I-10 feature-flag flip: the shadow path (driven by the
 * new mutator) must agree with the legacy path for a sustained
 * window before we trust the mutator to drive primary state.
 *
 * The use case is generic in the two ports — it does NOT know which
 * is "shadow" and which is "legacy". A future MCP route plugs one
 * `block_log_file` (the shadow) and one legacy-table adapter into
 * this function and reports the result.
 */

#ifndef ZCL_APPLICATION_OPERATIONS_DIFF_WITH_LEGACY_SHADOW_H
#define ZCL_APPLICATION_OPERATIONS_DIFF_WITH_LEGACY_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#include "util/result.h"

struct block_log_port;

struct diff_with_legacy_shadow_inputs {
    const struct block_log_port *primary;     /* typically the legacy block table */
    const struct block_log_port *shadow;      /* typically blocks.shadow */
    uint32_t start_height;                    /* inclusive */
    uint32_t end_height;                      /* inclusive; UINT32_MAX = "min(primary tip, shadow tip)" */
};

enum diff_with_legacy_shadow_status {
    DIFF_STATUS_CONVERGED          = 0,   /* every height matches; no divergence */
    DIFF_STATUS_DIVERGENT          = 1,   /* a height has different bytes on each side */
    DIFF_STATUS_SHADOW_MISSING     = 2,   /* primary has a block at H that shadow does not */
    DIFF_STATUS_PRIMARY_MISSING    = 3,   /* shadow has a block at H that primary does not */
    DIFF_STATUS_EMPTY_RANGE        = 4,   /* nothing to compare (both empty or invalid range) */
};

struct diff_with_legacy_shadow_report {
    enum diff_with_legacy_shadow_status status;
    uint32_t first_divergent_height;          /* meaningful when status != CONVERGED && != EMPTY_RANGE */
    uint32_t checked_count;                   /* heights actually compared */
    uint32_t primary_tip;                     /* observed at call time */
    uint32_t shadow_tip;
};

/* Pure function: no I/O beyond the supplied ports, no globals, no
 * allocation beyond port internals. */
struct zcl_result diff_with_legacy_shadow(
        const struct diff_with_legacy_shadow_inputs *in,
        struct diff_with_legacy_shadow_report *out);

enum diff_with_legacy_shadow_err {
    DIFF_ERR_NULL_ARG  = 5001,
    DIFF_ERR_PORT_READ = 5002,
};

#endif /* ZCL_APPLICATION_OPERATIONS_DIFF_WITH_LEGACY_SHADOW_H */
