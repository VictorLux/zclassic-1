/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "jobs/stage_repair.h"
#include "services/header_probe.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>

static _Atomic int g_target_at_detect = -1;
static _Atomic int g_remedy_calls = 0;
static _Atomic int g_mode_at_detect = STAGE_REPAIR_POISON_NONE;

static int repair_target_height(void)
{
    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return -1; // raw-return-ok:engine-not-ready
    int tip = active_chain_height(&ms->chain_active);
    return tip >= 0 ? tip + 1 : -1;
}

static bool detect_stale_validate_headers_repair(void)
{
    sqlite3 *db = progress_store_db();
    int target = repair_target_height();
    if (!db || target < 0)
        return false;

    enum stage_repair_header_solution_poison mode =
        stage_repair_header_solution_poison_mode(db, target);
    if (mode == STAGE_REPAIR_POISON_NONE)
        return false;

    atomic_store(&g_target_at_detect, target);
    atomic_store(&g_mode_at_detect, (int)mode);
    return true;
}

static enum condition_remedy_result remedy_stale_validate_headers_repair(void)
{
    sqlite3 *db = progress_store_db();
    int target = atomic_load(&g_target_at_detect);
    if (!db || target < 0)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);

    enum stage_repair_header_solution_poison mode =
        stage_repair_header_solution_poison_mode(db, target);
    if (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS) {
        if (!stage_repair_header_solution_available(db, target)) {
            int added = 0;
            struct zcl_result r = header_probe_pull_range(target, 128, &added);
            if (!r.ok) {
                LOG_WARN("condition",
                         "[condition:stale_validate_headers_repair] "
                         "header probe failed h=%d code=%d msg=%s",
                         target, r.code, r.message);
                return COND_REMEDY_FAILED;
            }
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "header probe h=%d added=%d",
                     target, added);
        }
        if (!stage_repair_header_solution_available(db, target)) {
            LOG_WARN("condition",
                     "[condition:stale_validate_headers_repair] "
                     "no durable repair header available h=%d",
                     target);
            return COND_REMEDY_FAILED;
        }
    }

    struct stage_repair_header_solution_result rr;
    struct main_state *ms = condition_engine_main_state();
    int active_tip = ms ? active_chain_height(&ms->chain_active) : -2;
    if (!stage_repair_header_solution_poison_rewind(db, target,
                                                    active_tip, &rr))
        return COND_REMEDY_FAILED;

    LOG_WARN("condition",
             "[condition:stale_validate_headers_repair] h=%d mode=%d "
             "deleted=%d rewound=%d",
             target, rr.mode, rr.deleted_rows, rr.rewound_cursors);
    return rr.repaired ? COND_REMEDY_OK : COND_REMEDY_SKIP;
}

static bool witness_stale_validate_headers_repair(int64_t target_at_detect)
{
    (void)target_at_detect;
    sqlite3 *db = progress_store_db();
    int target = atomic_load(&g_target_at_detect);
    if (!db || target < 0)
        return false;

    struct main_state *ms = condition_engine_main_state();
    if (ms && active_chain_height(&ms->chain_active) >= target)
        return true;

    if (stage_repair_header_solution_poison_present(db, target))
        return false;

    int mode = atomic_load(&g_mode_at_detect);
    if (mode == STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS)
        return stage_repair_header_solution_available(db, target);
    return true;
}

static struct condition c_stale_validate_headers_repair = {
    .name = "stale_validate_headers_repair",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_stale_validate_headers_repair,
    .remedy = remedy_stale_validate_headers_repair,
    .witness = witness_stale_validate_headers_repair,
    .witness_window_secs = 60,
};

void register_stale_validate_headers_repair(void)
{
    (void)condition_register(&c_stale_validate_headers_repair);
}

#ifdef ZCL_TESTING
void stale_validate_headers_repair_test_reset(void)
{
    struct condition_state *s = &c_stale_validate_headers_repair.state;
    atomic_store(&g_target_at_detect, -1);
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_mode_at_detect, STAGE_REPAIR_POISON_NONE);
    atomic_store(&s->attempts, 0);
    atomic_store(&s->last_outcome, COND_REMEDY_SKIP);
    atomic_store(&s->currently_active, false);
    atomic_store(&s->operator_needed_emitted, false);
}

int stale_validate_headers_repair_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif
