/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * job — the uniform Job contract (the fourth of the eight shapes).
 *
 * Why this exists
 * ----------------
 * A Job is a cursor-stamped reducer stage: it consumes the next durable
 * unit, commits its output and the new cursor in one transaction, and
 * returns exactly one of four results. This is the single shape every
 * app/jobs/src reducer (the eight stages) conforms to.
 *
 * The Job result REPLACES the old kernel-local `stage_result_t`. The
 * enumerator integer values are preserved byte-for-byte so the mapping
 * is a pure rename:
 *
 *   STAGE_ADVANCED (0) -> JOB_ADVANCED
 *   STAGE_BLOCKED  (1) -> JOB_BLOCKED
 *   STAGE_IDLE     (2) -> JOB_IDLE
 *   STAGE_ERROR    (3) -> JOB_FATAL
 *
 * The generic stage runner (`lib/util/stage.h`) consumes this type, so
 * the Job contract is the one source of truth for "what a step returns."
 *
 * Why the type lives in app/jobs and not lib/util
 * ------------------------------------------------
 * The shape is an application concept (one of the eight). The kernel
 * stage runner is the mechanical executor; it depends on the contract,
 * not the other way round. Every build target that pulls the stage
 * runner already has `-Iapp/jobs/include` on its include path. */

#ifndef ZCL_JOB_H
#define ZCL_JOB_H

typedef enum {
    JOB_ADVANCED = 0, /* cursor moved; output committed */
    JOB_BLOCKED  = 1, /* typed blocker preventing progress; cursor unchanged */
    JOB_IDLE     = 2, /* no work available right now; cursor unchanged */
    JOB_FATAL    = 3, /* unexpected failure; cursor unchanged */
} job_result_t;

#endif /* ZCL_JOB_H */
