/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * shadow_feeder — inbound observer that mirrors blocks accepted by
 * the legacy path into the new architecture, *without* affecting the
 * live chain state.
 *
 * What this gives us:
 *   - Every time the legacy validation path accepts a block, the
 *     feeder posts a VALIDATE_BLOCK command onto the mutator's input
 *     queue and appends the block bytes to a *shadow* block_log_port
 *     (a separate file at <datadir>/blocks.shadow/). The legacy
 *     storage is never touched.
 *   - Once shadow validation has run divergence-free under live load
 *     for a sustained window (see I-9), the feature flag at I-10
 *     promotes the mutator to primary and the legacy path becomes
 *     the secondary observer.
 *
 * What this does NOT do (yet):
 *   - Wire to the live event bus. The legacy path emits
 *     EV_BLOCK_CONNECTED with a height-only payload; we cannot
 *     reconstruct block bytes from that. Wiring the feeder into the
 *     live ingest path requires either a new event carrying the block
 *     or a direct hook in accept_block — both invasive enough that we
 *     do them as a separate sub-step (I-7b) once the contract here is
 *     proven by tests.
 *
 * The contract surface is `shadow_feeder_observe_block(...)` — any
 * future hook can call it, including a test that submits synthetic
 * blocks. That decoupling is the whole point of hexagonal: the
 * adapter exposes a function; the wiring decides who calls it.
 */

#ifndef ZCL_ADAPTERS_INBOUND_SHADOW_FEEDER_H
#define ZCL_ADAPTERS_INBOUND_SHADOW_FEEDER_H

#include <stddef.h>
#include <stdint.h>

#include "util/result.h"

struct block;
struct consensus_params;
struct mutator;
struct shadow_feeder;

struct shadow_feeder_config {
    /* Directory for the shadow block log. The feeder appends every
     * observed block here via a block_log_file adapter; the directory
     * is created if missing. */
    const char *shadow_dir;

    /* Mutator to post VALIDATE_BLOCK commands to. The feeder does not
     * own it; the caller manages its lifecycle. */
    struct mutator *mutator;

    /* Consensus parameters passed into the validate_block use case.
     * The feeder does not own this; it must outlive the feeder. */
    const struct consensus_params *params;
};

struct zcl_result shadow_feeder_create(const struct shadow_feeder_config *cfg,
                                       struct shadow_feeder **out);
void shadow_feeder_destroy(struct shadow_feeder *f);

/* Observe a block that the legacy path accepted. Idempotent on the
 * shadow log (same block bytes -> no-op append). Posts a VALIDATE_BLOCK
 * command onto the mutator queue; the command is fire-and-forget at
 * this layer (no completion attached). On queue backpressure the
 * function returns the BACKPRESSURE result so the caller can decide
 * whether to drop, retry, or escalate — never block.
 *
 * `bytes`/`len` carry the serialized block as it would go on the
 * wire; the feeder uses this to write to the shadow log. `block` is
 * the parsed view passed into the use case. Both are owned by the
 * caller; the feeder copies what it needs synchronously before
 * returning. */
struct zcl_result shadow_feeder_observe_block(
        struct shadow_feeder *f,
        uint32_t height,
        const struct block *block,
        const uint8_t *bytes,
        size_t len);

/* Stats observable for tests and the eventual divergence dashboard. */
unsigned long shadow_feeder_observed_count(struct shadow_feeder *f);
unsigned long shadow_feeder_backpressure_count(struct shadow_feeder *f);

enum shadow_feeder_err {
    SHADOW_FEEDER_ERR_NULL_ARG       = 4001,
    SHADOW_FEEDER_ERR_LOG_OPEN       = 4002,
    SHADOW_FEEDER_ERR_LOG_APPEND     = 4003,
    SHADOW_FEEDER_ERR_HASH_FAILED    = 4004,
};

#endif /* ZCL_ADAPTERS_INBOUND_SHADOW_FEEDER_H */
