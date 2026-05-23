/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Framework projection — thin typed sugar over util/projection.h.
 * See docs/FRAMEWORK.md section 1. */

#ifndef ZCL_FRAMEWORK_PROJECTION_H
#define ZCL_FRAMEWORK_PROJECTION_H

#include "util/log_macros.h"
#include "util/projection.h"

#include <stdint.h>
#include <stdio.h>

static inline projection_t *
framework_projection_open(const char *label, const char *path)
{
    projection_t *p = projection_open(path);
    if (!p) {
        fprintf(stderr,  // obs-ok:projection-framework-miss
                "[projection] open miss label=%s path=%s\n",
                label ? label : "(null)", path ? path : "(null)");
    }
    return p;
}

static inline int64_t
framework_projection_query_int64_or(projection_t *p, const char *label,
                                    const char *sql, int64_t dflt)
{
    int64_t v = dflt;
    if (projection_query_int64(p, sql, &v) != 0) {
        LOG_ERR("projection", "query miss label=%s sql=%s",
                label ? label : "(null)", sql ? sql : "(null)");
        return dflt;
    }
    return v;
}

#define FRAMEWORK_PROJECTION_OPEN(label, path) \
    framework_projection_open((label), (path))

#define PROJECTION_QUERY_INT64_OR(p, label, sql, dflt) \
    framework_projection_query_int64_or((p), (label), (sql), (dflt))

#endif /* ZCL_FRAMEWORK_PROJECTION_H */
