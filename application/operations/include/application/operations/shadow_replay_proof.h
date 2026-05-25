/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * shadow_replay_proof — offline cutover proof skeleton.
 *
 * Replays a primary block_log_port range into a shadow block_log_port, then
 * diffs the same range. A proof only passes when every fed block is diffed
 * and the byte-level diff converges.
 */

#ifndef ZCL_APPLICATION_OPERATIONS_SHADOW_REPLAY_PROOF_H
#define ZCL_APPLICATION_OPERATIONS_SHADOW_REPLAY_PROOF_H

#include "application/operations/diff_with_legacy_shadow.h"
#include "util/result.h"

#include <stdbool.h>
#include <stdint.h>

struct block_log_port;

enum shadow_replay_proof_status {
    SHADOW_REPLAY_PROOF_OK = 0,
    SHADOW_REPLAY_PROOF_EMPTY = 1,
    SHADOW_REPLAY_PROOF_SHADOW_NOT_EMPTY = 2,
    SHADOW_REPLAY_PROOF_DIVERGED = 3,
};

struct shadow_replay_proof_inputs {
    const struct block_log_port *primary;
    const struct block_log_port *shadow;
    uint32_t start_height;
    uint32_t end_height;  /* inclusive; UINT32_MAX = primary tip */
};

struct shadow_replay_proof_report {
    enum shadow_replay_proof_status status;
    bool proof_ok;
    uint32_t start_height;
    uint32_t end_height;
    uint32_t primary_tip_before;
    uint32_t shadow_tip_before;
    uint32_t shadow_tip_after;
    uint32_t blocks_fed;
    uint32_t blocks_diffed;
    struct diff_with_legacy_shadow_report diff;
};

struct zcl_result shadow_replay_proof_run(
        const struct shadow_replay_proof_inputs *in,
        struct shadow_replay_proof_report *out);

enum shadow_replay_proof_err {
    SHADOW_REPLAY_ERR_NULL_ARG = 5101,
    SHADOW_REPLAY_ERR_PRIMARY_READ = 5102,
    SHADOW_REPLAY_ERR_SHADOW_APPEND = 5103,
    SHADOW_REPLAY_ERR_DIFF = 5104,
};

#endif /* ZCL_APPLICATION_OPERATIONS_SHADOW_REPLAY_PROOF_H */
