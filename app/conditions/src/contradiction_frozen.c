/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"
#include "util/log_macros.h"

#include "config/runtime.h"
#include "services/chain_evidence_authority_service.h"
#include "services/chain_state_service.h"

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
    /* No automatic repair: a frozen contradiction means the evidence
     * sources disagree and needs operator review, not a blind restore.
     * The reconstruction primitive (cec_reconstruct_active_tip_evidence,
     * app/services/src/chain_evidence_reconstruct.c) is intentionally
     * not invoked here. */
    LOG_WARN("condition", "[condition:contradiction_frozen] repair hook unavailable; " "operator follow-up required");
    return COND_REMEDY_SKIP;
}

static bool witness_contradiction_frozen(int64_t target_at_detect)
{
    (void)target_at_detect;
    return current_state() != CEC_CONTRADICTION_FROZEN;
}

static struct condition c_contradiction_frozen = {
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
