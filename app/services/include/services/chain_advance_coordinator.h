/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chain_advance_coordinator — canonical source-selection policy.
 *
 * This layer does not connect blocks. It scores candidate advance sources
 * (native P2P, snapshots, local import, zclassicd mirror) and makes the
 * trust/fallback decision explicit before the lower-level chain_advance()
 * path applies anything through local consensus validation.
 */

#ifndef ZCL_SERVICES_CHAIN_ADVANCE_COORDINATOR_H
#define ZCL_SERVICES_CHAIN_ADVANCE_COORDINATOR_H

#include "json/json.h"
#include "services/block_source_policy.h"
#include <stdbool.h>

struct connman;
struct main_state;
struct node_db;

void chain_advance_coordinator_init(struct connman *cm,
                                    struct main_state *ms,
                                    struct node_db *ndb);
void chain_advance_coordinator_plan(const struct cac_plan_input *in,
                                    struct cac_decision *out);
bool chain_advance_coordinator_peer_floor_recovery_needed(
    int healthy_outbound,
    int min_healthy,
    int local_height,
    int peer_height,
    struct cac_decision *out);
bool chain_advance_coordinator_snapshot_offer_allowed(
    int local_height,
    int snapshot_height,
    int peer_tip_height,
    bool offer_valid,
    const char *reason,
    struct cac_decision *out);
bool chain_advance_coordinator_local_header_refill_needed(
    int local_height,
    int missing_height,
    int peer_height,
    int eligible_peers,
    int retry_count,
    bool retries_exhausted,
    struct cac_decision *out);
void chain_advance_coordinator_note_projection_deferred(int height,
                                                        const char *reason);
void chain_advance_coordinator_get_status(struct cac_decision *out);
bool chain_advance_coordinator_dump_state_json(struct json_value *out,
                                               const char *key);
void chain_advance_coordinator_reset_for_test(void);

#endif /* ZCL_SERVICES_CHAIN_ADVANCE_COORDINATOR_H */
