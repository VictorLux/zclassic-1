/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "jobs/stage_anchor.h"

#include "jobs/stage_helpers.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <stddef.h>

bool stage_anchor_upstream_cursors_to(sqlite3 *db, uint64_t target,
                                      const char *owner,
                                      const char *reason)
{
    static const char *const upstream[] = {
        "header_admit",
        "validate_headers",
        "body_fetch",
        "body_persist",
        "script_validate",
        "proof_validate",
        "utxo_apply",
    };
    const char *tag = owner && owner[0] ? owner : "stage_anchor";

    for (size_t i = 0; i < sizeof(upstream) / sizeof(upstream[0]); i++) {
        uint64_t before = stage_cursor_persisted(db, upstream[i], tag);
        if (!stage_set_named_cursor_if_behind(db, upstream[i], target)) {
            LOG_WARN(tag,
                     "[%s] anchor upstream cursor failed "
                     "stage=%s from=%llu to=%llu reason=%s",
                     tag, upstream[i], (unsigned long long)before,
                     (unsigned long long)target, reason ? reason : "");
            return false;
        }
        if (before < target) {
            LOG_INFO(tag,
                     "[%s] anchor upstream cursor stage=%s "
                     "from=%llu to=%llu reason=%s",
                     tag, upstream[i], (unsigned long long)before,
                     (unsigned long long)target, reason ? reason : "");
        }
    }
    return true;
}
