/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "application/operations/diff_with_legacy_shadow.h"

#include "ports/block_log_port.h"

#include <string.h>

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

struct zcl_result diff_with_legacy_shadow(
        const struct diff_with_legacy_shadow_inputs *in,
        struct diff_with_legacy_shadow_report *out)
{
    if (!in || !out || !in->primary || !in->shadow)
        return ZCL_ERR(DIFF_ERR_NULL_ARG, "diff: null arg(s)");

    memset(out, 0, sizeof *out);
    out->primary_tip = in->primary->tip_height((void *)in->primary->self);
    out->shadow_tip  = in->shadow->tip_height((void *)in->shadow->self);

    /* Empty-on-either-side or empty range is treated as EMPTY_RANGE,
     * not as divergence. The caller decides whether that's a problem
     * (e.g., it may indicate the shadow path is not yet feeding). */
    if (out->primary_tip == UINT32_MAX || out->shadow_tip == UINT32_MAX) {
        out->status = DIFF_STATUS_EMPTY_RANGE;
        return ZCL_OK;
    }

    uint32_t end = in->end_height;
    if (end == UINT32_MAX)
        end = min_u32(out->primary_tip, out->shadow_tip);
    uint32_t start = in->start_height;
    if (start > end) {
        out->status = DIFF_STATUS_EMPTY_RANGE;
        return ZCL_OK;
    }

    for (uint32_t h = start; ; h++) {
        const uint8_t *pbytes = NULL, *sbytes = NULL;
        size_t plen = 0, slen = 0;
        struct zcl_result rp = in->primary->read_at_height(
                (void *)in->primary->self, h, &pbytes, &plen);
        struct zcl_result rs = in->shadow->read_at_height(
                (void *)in->shadow->self, h, &sbytes, &slen);

        bool primary_has = rp.ok;
        bool shadow_has  = rs.ok;
        if (!primary_has && rp.code != BLOCK_LOG_ERR_NOT_FOUND)
            return ZCL_ERR(DIFF_ERR_PORT_READ,
                           "diff: primary read failed at h=%u code=%d %s",
                           h, rp.code, rp.message);
        if (!shadow_has && rs.code != BLOCK_LOG_ERR_NOT_FOUND)
            return ZCL_ERR(DIFF_ERR_PORT_READ,
                           "diff: shadow read failed at h=%u code=%d %s",
                           h, rs.code, rs.message);

        if (primary_has != shadow_has) {
            out->status = primary_has ? DIFF_STATUS_SHADOW_MISSING
                                       : DIFF_STATUS_PRIMARY_MISSING;
            out->first_divergent_height = h;
            return ZCL_OK;
        }
        if (primary_has /* and shadow_has */) {
            if (plen != slen || memcmp(pbytes, sbytes, plen) != 0) {
                out->status = DIFF_STATUS_DIVERGENT;
                out->first_divergent_height = h;
                return ZCL_OK;
            }
            out->checked_count++;
        }
        if (h == end) break;
    }
    out->status = DIFF_STATUS_CONVERGED;
    return ZCL_OK;
}
