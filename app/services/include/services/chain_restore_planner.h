/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain restore planner — pure planning slice extracted from
 * chain_restore_service.c. */

#ifndef ZCL_CHAIN_RESTORE_PLANNER_H
#define ZCL_CHAIN_RESTORE_PLANNER_H

struct chain_restore_input;
struct chain_restore_plan;

void chain_restore_plan(struct chain_restore_plan *out,
                        const struct chain_restore_input *in);

void chain_restore_record_plan_result(const struct chain_restore_plan *p);

#endif /* ZCL_CHAIN_RESTORE_PLANNER_H */
