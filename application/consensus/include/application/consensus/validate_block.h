/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * application/consensus/validate_block.h — first use case under the
 * new architecture.
 *
 * Use cases sit between the domain (pure validators, entities) and
 * the adapters (storage, network). They orchestrate domain logic
 * against ports; they themselves contain no I/O.
 *
 * `application_consensus_validate_block()` is the smallest use case
 * that exercises the full hexagonal pattern: it composes a pure
 * domain validator (PoW) with one outbound port (the UTXO snapshot)
 * and returns a typed `zcl_result`.
 *
 * Layering rule:
 *   application/<context>/ may #include from:
 *     - util/                       (zcl_result, log macros)
 *     - core/, primitives/          (entities & value objects)
 *     - consensus/                  (consensus_params)
 *     - domain/<any-context>/       (pure validators)
 *     - ports/                      (interfaces it orchestrates)
 *   It must NOT #include from:
 *     - app/, adapters/, lib/net/, lib/storage/
 *
 * This file is intentionally minimal during Epoch I. Later sub-steps
 * extend it (script verification, sapling proofs, value conservation,
 * merkle root, joinsplits, timestamp invariants, locktime). For now
 * the goal is to wire one end-to-end path through the new layers.
 */

#ifndef ZCL_APPLICATION_CONSENSUS_VALIDATE_BLOCK_H
#define ZCL_APPLICATION_CONSENSUS_VALIDATE_BLOCK_H

#include "util/result.h"

struct block;
struct consensus_params;
struct utxo_snapshot_port;

/* Inputs to the validate_block use case, grouped so the public API
 * remains stable as we add fields (e.g. clock_port, event_emitter_port
 * once contextual checks land). */
struct application_consensus_validate_block_inputs {
    const struct block *block;
    const struct consensus_params *params;
    const struct utxo_snapshot_port *utxo;  /* may be NULL: see below */
};

/* Validate a block against consensus rules.
 *
 * Currently checked:
 *   1. PoW solution (delegated to domain_consensus_verify_pow_solution)
 *   2. At least one transaction is present
 *   3. The first transaction is coinbase, none of the others are
 *   4. Every non-coinbase input refers to a coin present in `utxo`
 *      (skipped when `utxo` is NULL — used by tests that focus on
 *      header-only acceptance)
 *
 * Returns ZCL_OK on success, otherwise a zcl_result whose `code`
 * matches one of `application_consensus_err`.
 *
 * Pure with respect to the ports it is given: no global state,
 * no allocation beyond stack frames. */
struct zcl_result application_consensus_validate_block(
        const struct application_consensus_validate_block_inputs *in);

/* Error codes returned via zcl_result.code. Stable across builds —
 * new codes are appended. Numbering avoids overlap with the domain
 * codes (1000..1999) by starting at 2000. */
enum application_consensus_err {
    APPLICATION_CONSENSUS_ERR_NULL_ARG            = 2001,
    APPLICATION_CONSENSUS_ERR_POW_INVALID         = 2002,
    APPLICATION_CONSENSUS_ERR_NO_TRANSACTIONS     = 2003,
    APPLICATION_CONSENSUS_ERR_MISSING_COINBASE    = 2004,
    APPLICATION_CONSENSUS_ERR_EXTRA_COINBASE      = 2005,
    APPLICATION_CONSENSUS_ERR_INPUT_NOT_FOUND     = 2006,
    APPLICATION_CONSENSUS_ERR_INPUT_LOOKUP_FAILED = 2007,
    APPLICATION_CONSENSUS_ERR_NULL_INPUT_ARRAY    = 2008,
};

#endif /* ZCL_APPLICATION_CONSENSUS_VALIDATE_BLOCK_H */
