/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_BLOCK_SYNC_SERVICE_H
#define ZCL_BLOCK_SYNC_SERVICE_H

#include "services/header_sync_service.h"
#include "event/event.h"
#include <stdbool.h>
#include <stdint.h>

struct download_manager;
struct main_state;
struct block_index;
struct uint256;
struct p2p_node;

struct sync_progress_snapshot {
    enum sync_state sync_state;
    int chain_height;
    int header_height;
    uint64_t requested;
    uint64_t received;
    uint64_t timed_out;
    uint64_t in_flight;
    uint64_t queued;
    uint64_t total_bytes;
    double mbps_avg;
    double gib_received;
    bool should_log_progress;
    bool tip_stale;
    int64_t tip_stale_seconds;
};

struct sync_stall_recovery {
    bool should_recover;
    bool should_log;
    bool should_reset_tip_next;
    bool should_request_tip_parent;
    int chain_height;
    int next_height;
    size_t entries_at_next;
    size_t entries_with_data;
    size_t entries_failed;
    struct uint256 *alt_hashes;
    int32_t *alt_heights;
    size_t alt_count;
};

struct sync_block_assignment {
    bool should_assign;
    size_t max_assign;
};

struct sync_block_batch {
    bool should_assign;
    size_t in_flight_before;
    size_t assigned;
};

struct sync_block_acceptance {
    bool should_request_headers_retry;
    bool reached_peer_tip;
    bool should_emit_tip_updated;
    bool should_set_sync_state;
    enum sync_state next_sync_state;
    bool should_set_flush_policy;
    bool should_update_peer_state;
    enum peer_state next_peer_state;
};

void syncsvc_plan_invalid_block_getheaders(struct sync_getheaders_action *action,
                                           enum sync_state sync_state);
void syncsvc_plan_block_assignment(struct sync_block_assignment *plan,
                                   const struct p2p_node *node,
                                   size_t in_flight);
void syncsvc_assign_peer_blocks(struct sync_block_batch *batch,
                                struct download_manager *dm,
                                const struct p2p_node *node,
                                struct uint256 *out_hashes,
                                size_t out_cap);
void syncsvc_note_valid_block(struct sync_block_acceptance *result,
                              const struct p2p_node *node,
                              enum sync_state sync_state,
                              int new_tip_height,
                              int best_header_height);
void syncsvc_collect_progress(struct sync_progress_snapshot *snapshot,
                              struct download_manager *dm,
                              enum sync_state sync_state,
                              int chain_height,
                              int header_height,
                              int64_t peer_last_block_time,
                              int64_t now_seconds);
bool syncsvc_build_stall_recovery(struct sync_stall_recovery *recovery,
                                  const struct main_state *ms,
                                  const struct p2p_node *node,
                                  uint64_t queued,
                                  uint64_t in_flight,
                                  int64_t now_seconds);
enum sync_header_request_anchor syncsvc_recovery_header_anchor(
    const struct sync_stall_recovery *recovery,
    const struct block_index *tip);
void syncsvc_plan_recovery_getheaders(struct sync_getheaders_action *action,
                                      const struct sync_stall_recovery *recovery,
                                      const struct block_index *tip);
void syncsvc_apply_stall_recovery(const struct sync_stall_recovery *recovery,
                                  struct main_state *ms,
                                  struct download_manager *dm,
                                  int *cleared_blocks);
bool syncsvc_should_warn_tip_stale(
    const struct sync_progress_snapshot *snapshot,
    const struct p2p_node *node,
    int64_t now_seconds);
void syncsvc_plan_tip_stale_getheaders(struct sync_getheaders_action *action,
                                       const struct sync_progress_snapshot *snapshot,
                                       const struct p2p_node *node,
                                       int64_t now_seconds);
void syncsvc_free_stall_recovery(struct sync_stall_recovery *recovery);

#endif
