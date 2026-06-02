/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "supervisors/self_heal.h"
#include "util/log_macros.h"

#include "framework/condition.h"
#include "services/service_state_driver.h"
#include "supervisors/domains.h"
#include "util/supervisor.h"

#include <stdatomic.h>
#include <stdio.h>

static struct main_state *g_ms;
static struct liveness_contract g_contract;
static _Atomic supervisor_child_id g_id = SUPERVISOR_INVALID_ID;

static void self_heal_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_ms) return;
    condition_engine_tick();
    /* Drive the canonical operational mode from real progress (sync gap +
     * active repair Conditions) right after the conditions run. Pure
     * observability/state — never touches the chain or a consensus gate. */
    service_state_driver_tick();
    supervisor_progress(atomic_load(&g_id),
                        (int64_t)condition_engine_get_active_count());
}

void self_heal_register(struct main_state *ms)
{
    if (!ms) return;
    if (atomic_load(&g_id) != SUPERVISOR_INVALID_ID) return;
    g_ms = ms;
    condition_engine_set_main_state(ms);
    liveness_contract_init(&g_contract, "self_heal.engine");
    atomic_store(&g_contract.period_secs, (int64_t)5);
    atomic_store(&g_contract.deadline_secs, (int64_t)0);
    atomic_store(&g_contract.progress_max_quiet_us, (int64_t)0);
    g_contract.on_tick = self_heal_tick;
    g_contract.on_stall = NULL;
    supervisor_domains_init();
    atomic_store(&g_id, supervisor_register_in_domain(g_op_sup, &g_contract));
    if (atomic_load(&g_id) == SUPERVISOR_INVALID_ID) {
        LOG_WARN("self_heal", "[self_heal] WARN register failed");
    }
}
