/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "config/runtime.h"
#include "services/chain_evidence_controller.h"
#include "services/chain_state_repository.h"

#include <stdio.h>

static enum chain_evidence_controller_state current_state(void)
{
    struct chain_evidence_controller cec;
    chain_evidence_controller_init(&cec, app_runtime_node_db(), csr_instance());
    return chain_evidence_controller_load_state(&cec);
}

static bool detect_contradiction_frozen(void)
{
    return current_state() == CEC_CONTRADICTION_FROZEN;
}

static enum condition_remedy_result remedy_contradiction_frozen(void)
{
    /* The private reconstruction helper lives inside the existing
     * chain_evidence_controller mega-module. Phase 0 keeps this condition
     * scoped to detection/escalation rather than editing that module. */
    fprintf(stderr,  // obs-ok:condition-contradiction-frozen
            "[condition:contradiction_frozen] repair hook unavailable; "
            "operator follow-up required\n");
    return COND_REMEDY_SKIP;
}

static bool witness_contradiction_frozen(int64_t target_at_detect)
{
    (void)target_at_detect;
    return current_state() != CEC_CONTRADICTION_FROZEN;
}

static const struct condition c_contradiction_frozen = {
    .name = "contradiction_frozen",
    .severity = COND_CRITICAL,
    .poll_secs = 10,
    .backoff_secs = 60,
    .max_attempts = 1,
    .detect = detect_contradiction_frozen,
    .remedy = remedy_contradiction_frozen,
    .witness = witness_contradiction_frozen,
    .witness_window_secs = 60,
};

void register_contradiction_frozen(void)
{
    (void)condition_register(&c_contradiction_frozen);
}
