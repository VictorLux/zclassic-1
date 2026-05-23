/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "validation/process_block_revalidate.h"

#include <stdio.h>

static struct block_index *find_failed_next(struct main_state *ms, int target)
{
    if (!ms) return NULL;
    size_t iter = 0;
    struct block_index *p = NULL;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && p->nHeight == target && (p->nStatus & BLOCK_FAILED_MASK))
            return p;
    }
    return NULL;
}

static int64_t target_height(void)
{
    struct main_state *ms = condition_engine_main_state();
    return ms ? (int64_t)active_chain_height(&ms->chain_active) + 1 : -1;
}

static bool detect_block_failed_mask_at_tip(void)
{
    struct main_state *ms = condition_engine_main_state();
    int64_t target = target_height();
    return target >= 0 && find_failed_next(ms, (int)target) != NULL;
}

static enum condition_remedy_result remedy_block_failed_mask_at_tip(void)
{
    struct main_state *ms = condition_engine_main_state();
    int64_t target = target_height();
    if (!ms || target < 0)
        return COND_REMEDY_SKIP;
    struct uint256 out_hash;
    enum reval_result r =
        process_block_revalidate((int)target, ms, &out_hash);
    fprintf(stderr,  // obs-ok:condition-block-failed-revalidate
            "[condition:block_failed_mask_at_tip] target=%lld result=%s\n",
            (long long)target, reval_result_name(r));
    return (r == REVAL_RECOVERED || r == REVAL_NO_FAILURE)
        ? COND_REMEDY_OK : COND_REMEDY_FAILED;
}

static bool witness_block_failed_mask_at_tip(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct main_state *ms = condition_engine_main_state();
    int64_t target = target_height();
    return ms && target >= 0 && find_failed_next(ms, (int)target) == NULL;
}

static struct condition c_block_failed_mask_at_tip = {
    .name = "block_failed_mask_at_tip",
    .severity = COND_CRITICAL,
    .poll_secs = 5,
    .backoff_secs = 30,
    .max_attempts = 5,
    .detect = detect_block_failed_mask_at_tip,
    .remedy = remedy_block_failed_mask_at_tip,
    .witness = witness_block_failed_mask_at_tip,
    .witness_window_secs = 60,
};

void register_block_failed_mask_at_tip(void)
{
    (void)condition_register(&c_block_failed_mask_at_tip);
}
