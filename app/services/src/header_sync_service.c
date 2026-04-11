/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "services/header_sync_service.h"
#include "services/snapshot_sync_service.h"
#include "net/net.h"
#include "validation/chainstate.h"
#include <stdlib.h>
#include <time.h>

static int g_getheaders_log_count = 0;
static bool g_block_file_scan_triggered = false;

bool syncsvc_begin_peer_sync(struct p2p_node *node)
{
    if (!node || node->inbound || node->state != PEER_ACTIVE)
        return false;

    peer_set_state_checked((uint32_t)node->id, &node->state,
                           PEER_SYNCING_HEADERS, "IBD start");
    if (sync_get_state() == SYNC_IDLE ||
        sync_get_state() == SYNC_FINDING_PEERS) {
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "first outbound peer");
    }
    return true;
}

static void syncsvc_build_locator_from_chain(struct block_locator *loc,
                                             const struct active_chain *chain)
{
    const struct block_index *tip;
    struct block_index *walk;
    size_t alloc = 0;
    size_t idx = 0;
    int step = 1;
    int counter = 0;
    if (!loc || !chain)
        return;

    tip = active_chain_tip(chain);
    if (!tip || !tip->phashBlock)
        return;

    alloc = 32;
    loc->vhave = malloc(alloc * sizeof(struct uint256));
    if (!loc->vhave)
        return;

    walk = (struct block_index *)tip;
    while (walk && walk->phashBlock) {
        if (idx == alloc) {
            struct uint256 *nv = realloc(loc->vhave,
                                         alloc * 2 * sizeof(struct uint256));
            if (!nv)
                break;
            loc->vhave = nv;
            alloc *= 2;
        }
        loc->vhave[idx++] = *walk->phashBlock;

        for (int i = 0; i < step && walk; i++)
            walk = walk->pprev;

        if (++counter > 10)
            step *= 2;
    }

    loc->num_hashes = idx;
}

static void syncsvc_build_locator_from_index(struct block_locator *loc,
                                             const struct block_index *from)
{
    size_t alloc = 0;
    size_t idx = 0;
    int step = 1;
    int counter = 0;
    const struct block_index *walk = from;

    if (!loc || !from || !from->phashBlock)
        return;

    alloc = 32;
    loc->vhave = malloc(alloc * sizeof(struct uint256));
    if (!loc->vhave)
        return;

    while (walk && walk->phashBlock) {
        if (idx == alloc) {
            struct uint256 *nv = realloc(loc->vhave,
                                         alloc * 2 * sizeof(struct uint256));
            if (!nv)
                break;
            loc->vhave = nv;
            alloc *= 2;
        }
        loc->vhave[idx++] = *walk->phashBlock;

        for (int i = 0; i < step && walk; i++)
            walk = walk->pprev;

        if (++counter > 10)
            step *= 2;
    }

    loc->num_hashes = idx;
}

void syncsvc_evaluate_header_batch(struct sync_header_batch *result,
                                   size_t accepted,
                                   uint64_t total_count,
                                   const struct block_index *last_header)
{
    struct sync_header_batch empty = {0};

    if (!result) return;
    *result = empty;

    result->should_warn_all_rejected =
        (accepted == 0 && total_count > 0);
    result->should_emit_received = (accepted > 0);
    /* ZClassic/Zcash MAX_HEADERS_RESULTS is 160, not Bitcoin's 2000.
     * Request more headers if the batch was full (peer likely has more). */
    result->should_request_more_headers =
        (accepted > 0 && total_count >= 160 &&
         last_header && last_header->phashBlock);
}

void syncsvc_plan_header_download(struct sync_header_download_plan *plan,
                                  enum sync_state sync_state,
                                  const struct block_index *candidate,
                                  const struct block_index *tip,
                                  int our_height,
                                  struct uint256 *hashes,
                                  int32_t *heights,
                                  size_t max_collect)
{
    struct sync_header_download_plan empty = {0};

    if (!plan) return;
    *plan = empty;

    if (!candidate || candidate->nHeight <= our_height)
        return;

    plan->has_candidate = true;
    plan->should_begin_blocks_download =
        syncsvc_should_begin_blocks_download(sync_state, candidate, our_height);
    syncsvc_collect_needed_blocks(&plan->needed_blocks, candidate, tip,
                                  our_height, hashes, heights, max_collect);
}

void syncsvc_plan_header_processing(struct sync_header_processing_plan *plan,
                                    size_t accepted,
                                    uint64_t total_count,
                                    const struct block_index *last_header,
                                    enum sync_state sync_state,
                                    const struct block_index *candidate,
                                    const struct block_index *tip,
                                    int our_height,
                                    struct uint256 *hashes,
                                    int32_t *heights,
                                    size_t max_collect)
{
    struct sync_header_processing_plan empty = {0};

    if (!plan) return;
    *plan = empty;

    syncsvc_evaluate_header_batch(&plan->batch, accepted, total_count,
                                  last_header);
    plan->should_scan_block_files =
        syncsvc_should_scan_block_files_after_headers(accepted, last_header);
    syncsvc_plan_header_download(&plan->download, sync_state, candidate, tip,
                                 our_height, hashes, heights, max_collect);
    plan->should_set_sync_state =
        plan->download.should_begin_blocks_download;
    if (plan->should_set_sync_state)
        plan->next_sync_state = SYNC_BLOCKS_DOWNLOAD;
    plan->should_queue_needed_blocks =
        (hashes && heights && plan->download.has_candidate &&
         plan->download.needed_blocks.count > 0);
    plan->queue_count = plan->download.needed_blocks.count;
    plan->should_activate_chain =
        plan->download.needed_blocks.should_activate_chain;
}

void syncsvc_build_block_file_scan_activation(
    struct sync_chain_activation *result,
    int scanned_blocks)
{
    struct sync_chain_activation empty = {0};

    if (!result) return;
    *result = empty;
    result->should_activate =
        syncsvc_should_activate_after_block_file_scan(scanned_blocks);
}

void syncsvc_build_header_processing_activation(
    struct sync_chain_activation *result,
    const struct sync_header_processing_plan *plan)
{
    struct sync_chain_activation empty = {0};

    if (!result) return;
    *result = empty;
    result->should_activate =
        syncsvc_should_activate_after_header_processing(plan);
}

bool syncsvc_should_log_accepted_headers(const struct p2p_node *node,
                                         const struct block_index *header_tip)
{
    static int headers_log_count = 0;

    if (!header_tip)
        return true;

    if (node && node->starting_height > 0 &&
        header_tip->nHeight < node->starting_height - 2000) {
        return (headers_log_count++ % 10 == 0);
    }

    return true;
}

bool syncsvc_is_initial_block_download(const struct p2p_node *node,
                                       int our_height)
{
    if (!node) return false;
    return (node->starting_height > 0 &&
            our_height < node->starting_height - 144);
}

bool syncsvc_should_request_headers(const struct p2p_node *node,
                                    int our_height,
                                    int64_t now_seconds)
{
    if (!node || node->inbound) return false;
    if (node->state < PEER_SYNCING_HEADERS) return false;

    /* Interval: 10s during IBD, 60s while catching up, 120s at tip.
     * Never stop requesting headers entirely — starting_height is frozen
     * at handshake time and the peer's chain keeps growing. */
    int64_t interval;
    if (syncsvc_is_initial_block_download(node, our_height))
        interval = 10;
    else if (node->starting_height > 0 && our_height < node->starting_height)
        interval = 60;
    else
        interval = 120;
    return (now_seconds - node->last_getheaders_time) > interval;
}

void syncsvc_plan_periodic_getheaders(struct sync_getheaders_action *action,
                                      const struct p2p_node *node,
                                      int our_height,
                                      int64_t now_seconds)
{
    struct sync_getheaders_action empty = {0};

    if (!action) return;
    *action = empty;

    if (!syncsvc_should_request_headers(node, our_height, now_seconds))
        return;

    action->should_send = true;
    action->anchor = SYNC_HEADER_REQUEST_TIP;
    action->should_log = true;
}

void syncsvc_note_headers_requested(struct p2p_node *node,
                                    int64_t now_seconds)
{
    if (!node) return;
    node->last_getheaders_time = now_seconds;
}

bool syncsvc_should_scan_block_files_after_headers(size_t accepted,
                                                   const struct block_index *header_tip)
{
    if (g_block_file_scan_triggered)
        return false;
    if (accepted == 0 || !header_tip)
        return false;
    if (header_tip->nHeight <= 1000)
        return false;

    g_block_file_scan_triggered = true;
    return true;
}

bool syncsvc_build_getheaders_locator(struct block_locator *loc,
                                      const struct active_chain *chain,
                                      const struct block_index *from,
                                      const struct uint256 *genesis_hash)
{
    bool has_genesis = false;
    size_t i;

    if (!loc || !genesis_hash)
        return false;

    block_locator_init(loc);
    if (from)
        syncsvc_build_locator_from_index(loc, from);
    else
        syncsvc_build_locator_from_chain(loc, chain);

    if (loc->num_hashes == 0) {
        loc->vhave = malloc(sizeof(struct uint256));
        if (!loc->vhave)
            return false;
        loc->vhave[0] = *genesis_hash;
        loc->num_hashes = 1;
        return true;
    }

    for (i = 0; i < loc->num_hashes; i++) {
        if (uint256_eq(&loc->vhave[i], genesis_hash)) {
            has_genesis = true;
            break;
        }
    }

    if (!has_genesis) {
        struct uint256 *new_vhave = realloc(loc->vhave,
            (loc->num_hashes + 1) * sizeof(struct uint256));
        if (!new_vhave) {
            block_locator_free(loc);
            return false;
        }
        loc->vhave = new_vhave;
        loc->vhave[loc->num_hashes] = *genesis_hash;
        loc->num_hashes++;
    }

    return true;
}

enum sync_header_log_mode syncsvc_header_log_mode(
    const struct p2p_node *node,
    const struct block_index *tip,
    bool in_ibd)
{
    if (!node || !tip || !tip->phashBlock)
        return SYNC_HEADER_LOG_NONE;

    if (in_ibd) {
        if (g_getheaders_log_count++ % 10 == 0)
            return SYNC_HEADER_LOG_IBD;
        return SYNC_HEADER_LOG_NONE;
    }

    return SYNC_HEADER_LOG_TIP;
}

bool syncsvc_should_activate_after_block_file_scan(int scanned_blocks)
{
    return scanned_blocks > 0;
}

bool syncsvc_should_activate_after_header_processing(
    const struct sync_header_processing_plan *plan)
{
    if (!plan)
        return false;

    return plan->should_activate_chain;
}

bool syncsvc_should_begin_blocks_download(enum sync_state sync_state,
                                          const struct block_index *candidate,
                                          int our_height)
{
    return candidate && candidate->nHeight > our_height &&
           sync_state == SYNC_HEADERS_DOWNLOAD;
}

bool syncsvc_headers_chain_from_tip(const struct block_index *candidate,
                                    const struct block_index *tip,
                                    int our_height)
{
    const struct block_index *verify = candidate;
    const struct block_index *last_valid = NULL;

    while (verify && verify->nHeight > our_height) {
        last_valid = verify;
        verify = verify->pprev;
    }

    if (verify == tip)
        return true;
    if (verify && tip && verify->nHeight == tip->nHeight &&
        verify->phashBlock && tip->phashBlock &&
        uint256_eq(verify->phashBlock, tip->phashBlock)) {
        return true;
    }
    if (verify && tip && verify->nHeight == tip->nHeight)
        return true;

    /* After snapshot sync, the chain walks back to the snapshot anchor
     * (pprev=NULL at high height). If the walk stopped at a verified
     * anchor (non-null last_valid with NULL pprev above our_height),
     * accept this as a valid chain root. The anchor was verified by
     * FlyClient + SHA3 and represents a trusted chain point. */
    struct block_index *anchor = snapsync_get_anchor();
    if (!verify && anchor && last_valid) {
        const struct block_index *check = last_valid;
        while (check && check != anchor)
            check = check->pprev;
        if (check == anchor)
            return true;
    }

    return false;
}
void syncsvc_collect_needed_blocks(struct sync_needed_blocks *result,
                                   const struct block_index *candidate,
                                   const struct block_index *tip,
                                   int our_height,
                                   struct uint256 *hashes,
                                   int32_t *heights,
                                   size_t max_collect)
{
    struct sync_needed_blocks empty = {0};
    size_t walk_steps = 0;
    struct block_index *walk;
    size_t i;

    if (!result) return;
    *result = empty;

    if (!candidate || !hashes || !heights || max_collect == 0)
        return;

    result->chains_from_tip =
        syncsvc_headers_chain_from_tip(candidate, tip, our_height);
    if (!result->chains_from_tip)
        return;

    walk = (struct block_index *)candidate;
    while (walk && walk->pprev && walk->nHeight > our_height &&
           result->count < max_collect && walk_steps < 2048) {
        if (!(walk->nStatus & BLOCK_HAVE_DATA) &&
            !(walk->nStatus & BLOCK_FAILED_MASK) &&
            walk->phashBlock) {
            hashes[result->count] = *walk->phashBlock;
            heights[result->count] = walk->nHeight;
            result->count++;
        }
        walk = walk->pprev;
        walk_steps++;
    }

    for (i = 0; i < result->count / 2; i++) {
        struct uint256 th = hashes[i];
        int32_t ti = heights[i];

        hashes[i] = hashes[result->count - 1 - i];
        heights[i] = heights[result->count - 1 - i];
        hashes[result->count - 1 - i] = th;
        heights[result->count - 1 - i] = ti;
    }

    result->should_activate_chain = (result->count == 0);
}
