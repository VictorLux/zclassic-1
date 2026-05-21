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
#include "util/blocker.h"
#include <stdbool.h>
#include <stdint.h>

struct connman;
struct main_state;
struct node_db;

enum cac_source {
    CAC_SOURCE_NONE = 0,
    CAC_SOURCE_P2P,
    CAC_SOURCE_SNAPSHOT,
    CAC_SOURCE_LOCAL_IMPORT,
    CAC_SOURCE_ZCLASSICD_MIRROR,
    CAC_SOURCE_NUM
};

enum cac_decision_result {
    CAC_DECISION_WAIT = 0,
    CAC_DECISION_USE_SOURCE,
    CAC_DECISION_BLOCKED,
    CAC_DECISION_RECOVER,
    CAC_DECISION_NUM
};

struct cac_source_status {
    enum cac_source source;
    bool available;
    bool healthy;
    bool blocked;
    /* Round 6 C3 — typed classification of `blocked` for the
     * force-window bypass at score_source(). The force window (Round 5
     * C4, see chain_advance_coordinator.c:74) may bypass `blocked` ONLY
     * for TRANSIENT/DEPENDENCY/RESOURCE classes; PERMANENT blockers
     * (e.g. mirror hash-disagreement, body-hash-mismatch) are never
     * bypassed regardless of window state. Default is BLOCKER_TRANSIENT
     * so today's behavior is preserved for sources that haven't been
     * classified yet — the typed knob is opt-in per source. */
    enum blocker_class blocked_class;
    bool authorized;
    bool selectable;
    int height;
    int score;
    int score_base;
    int score_health;
    int score_height;
    int score_authorized;
    int score_target_lag_penalty;
    int score_failure_penalty;
    int score_mirror_gate_penalty;
    int64_t failures;
    int64_t timeouts;
    int64_t outbound_total;
    int64_t inbound_total;
    int64_t healthy_peers;
    int64_t inbound_healthy_peers;
    int64_t total_healthy_peers;
    int64_t connecting_peers;
    int64_t handshake_incomplete;
    int64_t inbound_handshake_incomplete;
    int64_t peer_groups;
    int64_t max_peer_group_size;
    int64_t healthy_peer_groups;
    int64_t healthy_max_peer_group_size;
    int64_t addnode_count;
    int64_t addnode_backoff_active;
    int64_t addnode_backoff_max_sec;
    int64_t addnode_tcp_failures;
    int64_t addnode_protocol_failures;
    int64_t progress_current;
    int64_t progress_total;
    int64_t lag;
    int64_t retry_count;
    int64_t distinct_peer_count;
    int64_t serving_peer_id;
    char state[32];
    char selection_blocker[64];
    char reason[384];
    char blocker[128];
};

struct cac_plan_input {
    int local_height;
    int best_header_height;
    int target_height;
    int projection_height;
    int64_t projection_lag;
    bool projection_deferred;
    bool local_recovery_active;
    bool local_retries_exhausted;
    /* Concurrent-redundancy override: when mirror source is reachable
     * AND its lag ≥ mirror_lag_sla_breach_blocks, mirror_fallback_allowed
     * is true regardless of local_recovery_active / retries state. The
     * "wait for local exhaustion" gate must not block redundant catchup
     * once we've fallen demonstrably behind zclassicd. */
    int mirror_lag_sla_breach_blocks;
    char projection_state[32];
    struct cac_source_status sources[CAC_SOURCE_NUM];
};

struct cac_decision {
    enum cac_decision_result result;
    enum cac_source selected_source;
    bool activation_allowed;
    bool mirror_fallback_allowed;
    int local_height;
    int best_header_height;
    int target_height;
    int projection_height;
    int64_t projection_lag;
    bool projection_deferred;
    int64_t projection_deferred_total;
    int last_projection_deferred_height;
    int64_t last_projection_deferred_time;
    int selected_score;
    char projection_state[32];
    char last_projection_deferred_reason[64];
    char reason[128];
    char blocker[128];
    struct cac_source_status sources[CAC_SOURCE_NUM];
};

const char *cac_source_name(enum cac_source source);
const char *cac_source_trust_name(enum cac_source source);
const char *cac_decision_result_name(enum cac_decision_result result);

void chain_advance_coordinator_init(struct connman *cm,
                                    struct main_state *ms,
                                    struct node_db *ndb);
void chain_advance_coordinator_plan(const struct cac_plan_input *in,
                                    struct cac_decision *out);
bool chain_advance_coordinator_mirror_repair_allowed(
    int local_height,
    int target_height,
    bool local_retries_exhausted,
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

/* Round 5 C4: time-based mirror promotion bypass.
 *
 * When zclassicd mirror has been at "fatal" lag-SLO breach for a
 * sustained period AND the local chain has not advanced, the
 * supervisor calls this function to authorize the coordinator to
 * promote the mirror source despite the `mir->blocked` flag.
 *
 * Effect: for the next 300 s, `mirror_fallback_allowed()` returns
 * true unconditionally and `score_source()` bypasses the
 * `if (s->blocked)` short-circuit. Bodies still validate through
 * local consensus — the bypass only changes *which source feeds the
 * queue*, not whether validation runs.
 *
 * Emits EV_COORDINATOR_FORCE_PROMOTION with the audit reason. The
 * bounded 300 s window expires automatically; supervisor decides
 * whether to re-extend on the next on_stall edge.
 *
 * Idempotent — repeated calls reset the expiration timestamp. */
void chain_advance_coordinator_force_mirror_promotion(const char *audit_reason);

/* Returns true if a force-promotion window is currently active (i.e.
 * now < g_force_mirror_until_us). Used by tests + introspection. */
bool chain_advance_coordinator_force_mirror_active(void);

#endif /* ZCL_SERVICES_CHAIN_ADVANCE_COORDINATOR_H */
