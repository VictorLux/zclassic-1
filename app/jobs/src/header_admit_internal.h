/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * header_admit_internal — sibling-private declarations shared between
 * header_admit_stage.c (the Job) and its extracted helper TU
 * (header_admit_stage_diff.c, the S-11 mini-diff harness).
 *
 * NOT a public header. The diff harness was moved out verbatim to keep
 * header_admit_stage.c under the E1 file-size ceiling; these two
 * accessors expose the stage's file-static binding (main_state + stage_t)
 * to the diff TU without widening the public API. */

#ifndef ZCL_JOBS_HEADER_ADMIT_INTERNAL_H
#define ZCL_JOBS_HEADER_ADMIT_INTERNAL_H

struct main_state;
struct stage;

/* The main_state the stage is currently bound to (NULL before init /
 * after shutdown). Read-only for the diff harness's snapshot. */
struct main_state *header_admit_internal_ms(void);

/* The bound stage handle (NULL before init). The diff harness uses it
 * only to read the cursor and as a not-ready sentinel. */
struct stage *header_admit_internal_stage(void);

#endif /* ZCL_JOBS_HEADER_ADMIT_INTERNAL_H */
