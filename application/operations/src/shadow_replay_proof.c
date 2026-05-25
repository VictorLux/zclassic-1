/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "application/operations/shadow_replay_proof.h"

#include "ports/block_log_port.h"

#include <string.h>

struct replay_ctx {
    const struct block_log_port *shadow;
    uint32_t end_height;
    uint32_t blocks_fed;
    bool failed;
    struct zcl_result failure;
};

static bool replay_one(uint32_t height,
                       const struct block_hash *hash,
                       const uint8_t *bytes,
                       size_t len,
                       void *user_data)
{
    struct replay_ctx *ctx = user_data;
    if (ctx->end_height != UINT32_MAX && height > ctx->end_height)
        return false;

    struct zcl_result r = ctx->shadow->append(ctx->shadow->self, height,
                                              hash, bytes, len);
    if (!r.ok) {
        ctx->failed = true;
        ctx->failure = ZCL_ERR(SHADOW_REPLAY_ERR_SHADOW_APPEND,
                               "shadow replay append failed at h=%u: "
                               "code=%d %s",
                               height, r.code, r.message);
        return false;
    }
    ctx->blocks_fed++;
    return true;
}

struct zcl_result shadow_replay_proof_run(
        const struct shadow_replay_proof_inputs *in,
        struct shadow_replay_proof_report *out)
{
    if (!in || !out || !in->primary || !in->shadow)
        return ZCL_ERR(SHADOW_REPLAY_ERR_NULL_ARG,
                       "shadow_replay_proof: null arg(s)");

    memset(out, 0, sizeof(*out));
    out->start_height = in->start_height;
    out->primary_tip_before = in->primary->tip_height(in->primary->self);
    out->shadow_tip_before = in->shadow->tip_height(in->shadow->self);
    out->end_height = in->end_height == UINT32_MAX
        ? out->primary_tip_before : in->end_height;

    if (out->primary_tip_before == UINT32_MAX ||
        in->start_height > out->end_height) {
        out->status = SHADOW_REPLAY_PROOF_EMPTY;
        return ZCL_OK;
    }

    if (out->shadow_tip_before != UINT32_MAX &&
        out->shadow_tip_before >= in->start_height) {
        out->status = SHADOW_REPLAY_PROOF_SHADOW_NOT_EMPTY;
        return ZCL_OK;
    }

    struct replay_ctx ctx = {
        .shadow = in->shadow,
        .end_height = out->end_height,
    };
    struct zcl_result r = in->primary->iter_from(in->primary->self,
                                                 in->start_height,
                                                 replay_one, &ctx);
    if (!r.ok)
        return ZCL_ERR(SHADOW_REPLAY_ERR_PRIMARY_READ,
                       "shadow replay primary iter failed: code=%d %s",
                       r.code, r.message);
    if (ctx.failed)
        return ctx.failure;

    out->blocks_fed = ctx.blocks_fed;
    out->shadow_tip_after = in->shadow->tip_height(in->shadow->self);

    struct diff_with_legacy_shadow_inputs diff_in = {
        .primary = in->primary,
        .shadow = in->shadow,
        .start_height = in->start_height,
        .end_height = out->end_height,
    };
    r = diff_with_legacy_shadow(&diff_in, &out->diff);
    if (!r.ok)
        return ZCL_ERR(SHADOW_REPLAY_ERR_DIFF,
                       "shadow replay diff failed: code=%d %s",
                       r.code, r.message);

    out->blocks_diffed = out->diff.checked_count;
    out->proof_ok = out->blocks_fed > 0 &&
                    out->diff.status == DIFF_STATUS_CONVERGED &&
                    out->blocks_diffed == out->blocks_fed;
    out->status = out->proof_ok ? SHADOW_REPLAY_PROOF_OK
                                : SHADOW_REPLAY_PROOF_DIVERGED;
    return ZCL_OK;
}
