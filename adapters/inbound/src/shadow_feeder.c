/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "adapters/inbound/shadow_feeder.h"

#include "adapters/outbound/persistence/block_log_file.h"
#include "mutator/cmd.h"
#include "mutator/input_queue.h"
#include "mutator/mutator.h"
#include "ports/block_log_port.h"
#include "primitives/block.h"
#include "core/uint256.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct shadow_feeder {
    struct mutator *mutator;
    const struct consensus_params *params;

    struct block_log_file *log_handle;
    struct block_log_port log_port;

    atomic_ulong observed;
    atomic_ulong backpressure_hits;
};

struct zcl_result shadow_feeder_create(const struct shadow_feeder_config *cfg,
                                       struct shadow_feeder **out)
{
    if (!cfg || !cfg->shadow_dir || !cfg->mutator || !cfg->params || !out)
        return ZCL_ERR(SHADOW_FEEDER_ERR_NULL_ARG,
                       "shadow_feeder_create: missing required field(s)");

    struct shadow_feeder *f = calloc(1, sizeof *f);
    if (!f)
        return ZCL_ERR(SHADOW_FEEDER_ERR_NULL_ARG,
                       "shadow_feeder_create: calloc");
    f->mutator = cfg->mutator;
    f->params = cfg->params;
    atomic_init(&f->observed, 0);
    atomic_init(&f->backpressure_hits, 0);

    struct zcl_result r = block_log_file_open(cfg->shadow_dir,
                                              &f->log_handle, &f->log_port);
    if (!r.ok) {
        free(f);
        return ZCL_ERR(SHADOW_FEEDER_ERR_LOG_OPEN,
                       "shadow_feeder_create: log open code=%d %s",
                       r.code, r.message);
    }
    *out = f;
    return ZCL_OK;
}

void shadow_feeder_destroy(struct shadow_feeder *f)
{
    if (!f) return;
    block_log_file_close(f->log_handle);
    free(f);
}

struct zcl_result shadow_feeder_observe_block(
        struct shadow_feeder *f,
        uint32_t height,
        const struct block *block,
        const uint8_t *bytes,
        size_t len)
{
    if (!f || !block)
        return ZCL_ERR(SHADOW_FEEDER_ERR_NULL_ARG,
                       "observe_block: null arg(s)");
    if (len > 0 && !bytes)
        return ZCL_ERR(SHADOW_FEEDER_ERR_NULL_ARG,
                       "observe_block: len>0 but bytes NULL");

    /* Compute the block hash for the log key. */
    struct uint256 hash;
    block_get_hash(block, &hash);
    struct block_hash log_hash;
    memcpy(log_hash.bytes, hash.data, 32);

    /* Append to the shadow log (idempotent on hash + bytes). */
    struct zcl_result r = f->log_port.append(f->log_port.self, height,
                                             &log_hash, bytes, len);
    if (!r.ok)
        return ZCL_ERR(SHADOW_FEEDER_ERR_LOG_APPEND,
                       "observe_block: shadow log append code=%d %s",
                       r.code, r.message);

    /* Fire-and-forget VALIDATE_BLOCK onto the mutator queue. The
     * caller does not wait for a result here — the divergence
     * dashboard will read the consensus_log_port once that lands. */
    struct mutator_cmd cmd = {0};
    cmd.kind = MUTATOR_CMD_VALIDATE_BLOCK;
    cmd.completion = NULL;
    cmd.u.validate_block.block = block;
    cmd.u.validate_block.params = f->params;
    cmd.u.validate_block.utxo = NULL;  /* UTXO snapshot adapter lands in I-7b/I-8 */

    struct zcl_result q = mutator_input_queue_push(
            mutator_input_queue_of(f->mutator), &cmd);
    if (!q.ok) {
        /* MUTATOR_ERR_BACKPRESSURE is the expected drop path; record
         * and surface it. The shadow append above already succeeded,
         * so the log stays a strict superset of dispatch attempts —
         * that's intentional: a recovery scan can re-feed missed
         * blocks. */
        if (q.code == MUTATOR_ERR_BACKPRESSURE)
            atomic_fetch_add_explicit(&f->backpressure_hits, 1,
                                      memory_order_relaxed);
        return q;
    }
    atomic_fetch_add_explicit(&f->observed, 1, memory_order_relaxed);
    return ZCL_OK;
}

unsigned long shadow_feeder_observed_count(struct shadow_feeder *f)
{
    if (!f) return 0;
    return atomic_load_explicit(&f->observed, memory_order_relaxed);
}

unsigned long shadow_feeder_backpressure_count(struct shadow_feeder *f)
{
    if (!f) return 0;
    return atomic_load_explicit(&f->backpressure_hits, memory_order_relaxed);
}
