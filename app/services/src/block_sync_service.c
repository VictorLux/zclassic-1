/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "services/block_sync_service.h"
#include "services/header_sync_service.h"
#include "net/download.h"
#include "net/net.h"
#include "validation/main_state.h"
#include <stdlib.h>
#include <string.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static int64_t g_last_stall_log = 0;
static int64_t g_last_stall_reset = 0;
static int64_t g_last_stale_warn = 0;

void syncsvc_plan_invalid_block_getheaders(struct sync_getheaders_action *action,
                                           enum sync_state sync_state)
{
    struct sync_getheaders_action empty = {0};

    if (!action) return;
    *action = empty;

    if (sync_state > SYNC_BLOCKS_DOWNLOAD)
        return;

    action->should_send = true;
    action->anchor = SYNC_HEADER_REQUEST_TIP;
    action->should_log = false;
}

void syncsvc_plan_block_assignment(struct sync_block_assignment *plan,
                                   const struct p2p_node *node,
                                   size_t in_flight)
{
    struct sync_block_assignment empty = {0};
    if (!plan) return;
    *plan = empty;

    if (!node || node->state < PEER_HANDSHAKE_COMPLETE)
        return;

    plan->should_assign = true;
    plan->max_assign = 64;
    if (in_flight > DL_MAX_IN_FLIGHT_PER_PEER / 2)
        plan->max_assign = 16;
}

void syncsvc_assign_peer_blocks(struct sync_block_batch *batch,
                                struct download_manager *dm,
                                const struct p2p_node *node,
                                struct uint256 *out_hashes,
                                size_t out_cap)
{
    struct sync_block_batch empty = {0};
    struct sync_block_assignment plan;

    if (!batch) return;
    *batch = empty;

    if (!dm || !node || !out_hashes || out_cap == 0)
        return;

    batch->in_flight_before = dl_peer_in_flight(dm, (uint32_t)node->id);
    syncsvc_plan_block_assignment(&plan, node, batch->in_flight_before);
    batch->should_assign = plan.should_assign;
    if (!plan.should_assign)
        return;

    if (plan.max_assign > out_cap)
        plan.max_assign = out_cap;
    batch->assigned = dl_assign_to_peer(dm, (uint32_t)node->id,
                                        out_hashes, plan.max_assign);
}

void syncsvc_note_valid_block(struct sync_block_acceptance *result,
                              const struct p2p_node *node,
                              enum sync_state sync_state,
                              int new_tip_height,
                              int best_header_height,
                              uint32_t new_tip_time)
{
    struct sync_block_acceptance empty = {0};
    bool headers_caught_up = false;

    if (!result) return;
    *result = empty;
    if (!node) return;

    /* Match ZClassic C++ tip detection: consider "at tip" when EITHER:
     * (a) our height >= peer's starting_height AND headers caught up, OR
     * (b) our tip's block time is within PoWTargetSpacing*2 of now
     *     (tip is recent, we're receiving blocks in real-time).
     *
     * (b) handles the edge case where peer's starting_height from
     * handshake is stale — the peer advanced while we were syncing,
     * so new_tip_height never reaches the old starting_height. */
    bool tip_is_recent = (new_tip_time > 0 &&
        (int64_t)new_tip_time > (int64_t)time(NULL) - 75 * 2);
    bool reached_peer = (node->starting_height > 0 &&
                         new_tip_height >= node->starting_height);

    if (!reached_peer && !tip_is_recent)
        return;

    headers_caught_up =
        (best_header_height >= 0 && best_header_height <= new_tip_height + 1);
    result->reached_peer_tip = true;
    if ((headers_caught_up || tip_is_recent) &&
        (sync_state == SYNC_BLOCKS_DOWNLOAD ||
         sync_state == SYNC_CONNECTING_BLOCKS ||
         sync_state == SYNC_REORG)) {
        result->should_set_sync_state = true;
        result->next_sync_state = SYNC_AT_TIP;
        result->should_set_flush_policy = true;
        result->should_emit_tip_updated = (sync_state != SYNC_REORG);
    }

    if (headers_caught_up &&
        (node->state == PEER_SYNCING_BLOCKS ||
         node->state == PEER_SYNCING_HEADERS)) {
        result->should_update_peer_state = true;
        result->next_peer_state = PEER_ACTIVE;
    }
}

void syncsvc_collect_progress(struct sync_progress_snapshot *snapshot,
                              struct download_manager *dm,
                              enum sync_state sync_state,
                              int chain_height,
                              int header_height,
                              int64_t peer_last_block_time,
                              int64_t now_seconds)
{
    struct sync_progress_snapshot empty;
    if (!snapshot) return;
    memset(&empty, 0, sizeof(empty));
    *snapshot = empty;

    snapshot->sync_state = sync_state;
    snapshot->chain_height = chain_height;
    snapshot->header_height = header_height;

    if (dm) {
        dl_get_stats(dm,
                     &snapshot->requested,
                     &snapshot->received,
                     &snapshot->timed_out,
                     &snapshot->in_flight,
                     &snapshot->queued);
        dl_get_throughput(dm, &snapshot->total_bytes, &snapshot->mbps_avg);
    }

    snapshot->gib_received =
        (double)snapshot->total_bytes / (1024.0 * 1024.0 * 1024.0);
    snapshot->should_log_progress =
        (sync_state != SYNC_IDLE && sync_state != SYNC_AT_TIP);

    if (sync_state == SYNC_AT_TIP && peer_last_block_time > 0 &&
        now_seconds > peer_last_block_time) {
        snapshot->tip_stale_seconds = now_seconds - peer_last_block_time;
        snapshot->tip_stale = snapshot->tip_stale_seconds > 600;
    }
}

bool syncsvc_build_stall_recovery(struct sync_stall_recovery *recovery,
                                  const struct main_state *ms,
                                  const struct p2p_node *node,
                                  uint64_t queued,
                                  uint64_t in_flight,
                                  int64_t now_seconds)
{
    struct sync_stall_recovery empty = {0};
    if (!recovery) LOG_FAIL("block_sync", "build_stall_recovery: null recovery pointer");
    *recovery = empty;

    if (!ms || !node) LOG_FAIL("block_sync", "build_stall_recovery: null ms=%d node=%d", !ms, !node);

    int our_h = active_chain_height(&ms->chain_active);
    if (queued != 0 || in_flight != 0) return false;
    if (node->starting_height <= our_h + 10) return false;
    if (node->state < PEER_HANDSHAKE_COMPLETE) return false;
    if (now_seconds - g_last_stall_log <= 10) return false;

    g_last_stall_log = now_seconds;
    recovery->should_recover = true;
    recovery->should_log = true;
    recovery->chain_height = our_h;
    recovery->next_height = our_h + 1;

    /* Scan a window of heights (not just +1) to find the first gap.
     * This handles cases where height+1 has an orphan block but
     * the real chain continues at height+2..+10. */
    for (int probe = 1; probe <= 10; probe++) {
        int check_h = our_h + probe;
        bool found_data = false;
        size_t pi = 0;
        struct block_index *px;
        while (block_map_next(&ms->map_block_index, &pi, NULL, &px)) {
            if (px && px->nHeight == check_h) {
                if (probe == 1) {
                    recovery->entries_at_next++;
                    if (px->nStatus & BLOCK_FAILED_MASK) recovery->entries_failed++;
                    if (px->nStatus & BLOCK_HAVE_DATA) recovery->entries_with_data++;
                }
                if (px->nStatus & BLOCK_HAVE_DATA)
                    found_data = true;
            }
        }
        if (!found_data) {
            recovery->next_height = check_h;
            break;
        }
    }

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    if (!tip) return true;

    struct uint256 *alt_hashes = zcl_calloc(64, sizeof(struct uint256), "stall recovery hashes");
    int32_t *alt_heights = zcl_calloc(64, sizeof(int32_t), "stall recovery heights");
    if (!alt_hashes || !alt_heights) {
        free(alt_hashes);
        free(alt_heights);
        return true;
    }

    size_t alt_count = 0;
    size_t iter2 = 0;
    struct block_index *alt;
    while (block_map_next(&ms->map_block_index, &iter2, NULL, &alt)) {
        if (!alt || alt_count >= 64) continue;
        if (alt->nHeight <= our_h) continue;
        if (alt->nHeight > our_h + 512) continue;
        if (alt->nStatus & BLOCK_FAILED_MASK) continue;
        if (alt->nStatus & BLOCK_HAVE_DATA) continue;
        if (!alt->phashBlock) continue;

        struct block_index *walk = alt;
        while (walk && walk->nHeight > our_h) walk = walk->pprev;
        if (walk == tip ||
            (walk && tip && walk->phashBlock && tip->phashBlock &&
             uint256_eq(walk->phashBlock, tip->phashBlock))) {
            alt_hashes[alt_count] = *alt->phashBlock;
            alt_heights[alt_count] = alt->nHeight;
            alt_count++;
        }
    }

    recovery->alt_hashes = alt_hashes;
    recovery->alt_heights = alt_heights;
    recovery->alt_count = alt_count;
    recovery->should_request_tip_parent = (tip->pprev != NULL);

    if (alt_count == 0 && now_seconds - g_last_stall_reset > 30) {
        g_last_stall_reset = now_seconds;
        recovery->should_reset_tip_next = true;
    }

    return true;
}

enum sync_header_request_anchor syncsvc_recovery_header_anchor(
    const struct sync_stall_recovery *recovery,
    const struct block_index *tip)
{
    if (!recovery || !recovery->should_recover)
        return SYNC_HEADER_REQUEST_TIP;

    if (recovery->should_request_tip_parent && tip && tip->pprev)
        return SYNC_HEADER_REQUEST_TIP_PARENT;

    return SYNC_HEADER_REQUEST_TIP;
}

void syncsvc_plan_recovery_getheaders(struct sync_getheaders_action *action,
                                      const struct sync_stall_recovery *recovery,
                                      const struct block_index *tip)
{
    struct sync_getheaders_action empty = {0};

    if (!action) return;
    *action = empty;
    if (!recovery || !recovery->should_recover)
        return;

    action->should_send = true;
    action->anchor = syncsvc_recovery_header_anchor(recovery, tip);
    action->should_log = false;
}

void syncsvc_apply_stall_recovery(const struct sync_stall_recovery *recovery,
                                  struct main_state *ms,
                                  struct download_manager *dm,
                                  int *cleared_blocks)
{
    if (cleared_blocks) *cleared_blocks = 0;
    if (!recovery || !ms) return;

    if (recovery->alt_count > 0 && dm) {
        dl_queue_blocks(dm, recovery->alt_hashes,
                        recovery->alt_heights, recovery->alt_count);
        return;
    }

    if (!recovery->should_reset_tip_next)
        return;

    size_t iter = 0;
    struct block_index *bi;
    int cleared = 0;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (!bi) continue;
        if (bi->nHeight == recovery->next_height) {
            bi->nStatus &= ~BLOCK_FAILED_MASK;
            bi->nStatus &= ~BLOCK_HAVE_DATA;
            cleared++;
        }
    }
    if (cleared_blocks) *cleared_blocks = cleared;
}

bool syncsvc_should_warn_tip_stale(
    const struct sync_progress_snapshot *snapshot,
    const struct p2p_node *node,
    int64_t now_seconds)
{
    if (!snapshot || !node || node->inbound || !snapshot->tip_stale)
        return false;
    if (now_seconds - g_last_stale_warn <= 300)
        return false;

    g_last_stale_warn = now_seconds;
    return true;
}

void syncsvc_plan_tip_stale_getheaders(struct sync_getheaders_action *action,
                                       const struct sync_progress_snapshot *snapshot,
                                       const struct p2p_node *node,
                                       int64_t now_seconds)
{
    struct sync_getheaders_action empty = {0};

    if (!action) return;
    *action = empty;

    if (!syncsvc_should_warn_tip_stale(snapshot, node, now_seconds))
        return;

    action->should_send = true;
    action->anchor = SYNC_HEADER_REQUEST_TIP;
    action->should_log = true;
}

void syncsvc_free_stall_recovery(struct sync_stall_recovery *recovery)
{
    if (!recovery) return;
    free(recovery->alt_hashes);
    free(recovery->alt_heights);
    recovery->alt_hashes = NULL;
    recovery->alt_heights = NULL;
    recovery->alt_count = 0;
}
