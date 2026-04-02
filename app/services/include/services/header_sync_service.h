/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_HEADER_SYNC_SERVICE_H
#define ZCL_HEADER_SYNC_SERVICE_H

#include "event/event.h"
#include "primitives/block.h"
#include <stdbool.h>
#include <stdint.h>

struct active_chain;
struct block_index;
struct p2p_node;
struct uint256;

enum sync_header_request_anchor {
    SYNC_HEADER_REQUEST_TIP = 0,
    SYNC_HEADER_REQUEST_TIP_PARENT = 1,
    SYNC_HEADER_REQUEST_EXPLICIT = 2,
};

enum sync_header_log_mode {
    SYNC_HEADER_LOG_NONE = 0,
    SYNC_HEADER_LOG_IBD = 1,
    SYNC_HEADER_LOG_TIP = 2,
};

struct sync_needed_blocks {
    bool chains_from_tip;
    bool should_activate_chain;
    size_t count;
};

struct sync_header_batch {
    bool should_warn_all_rejected;
    bool should_emit_received;
    bool should_request_more_headers;
};

struct sync_header_download_plan {
    bool has_candidate;
    bool should_begin_blocks_download;
    struct sync_needed_blocks needed_blocks;
};

struct sync_header_processing_plan {
    struct sync_header_batch batch;
    bool should_scan_block_files;
    struct sync_header_download_plan download;
    bool should_set_sync_state;
    enum sync_state next_sync_state;
    bool should_queue_needed_blocks;
    size_t queue_count;
    bool should_activate_chain;
};

struct sync_chain_activation {
    bool should_activate;
};

struct sync_getheaders_action {
    bool should_send;
    enum sync_header_request_anchor anchor;
    bool should_log;
};

bool syncsvc_begin_peer_sync(struct p2p_node *node);
void syncsvc_collect_needed_blocks(struct sync_needed_blocks *result,
                                   const struct block_index *candidate,
                                   const struct block_index *tip,
                                   int our_height,
                                   struct uint256 *hashes,
                                   int32_t *heights,
                                   size_t max_collect);
void syncsvc_evaluate_header_batch(struct sync_header_batch *result,
                                   size_t accepted,
                                   uint64_t total_count,
                                   const struct block_index *last_header);
void syncsvc_plan_header_download(struct sync_header_download_plan *plan,
                                  enum sync_state sync_state,
                                  const struct block_index *candidate,
                                  const struct block_index *tip,
                                  int our_height,
                                  struct uint256 *hashes,
                                  int32_t *heights,
                                  size_t max_collect);
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
                                    size_t max_collect);
void syncsvc_build_block_file_scan_activation(
    struct sync_chain_activation *result,
    int scanned_blocks);
void syncsvc_build_header_processing_activation(
    struct sync_chain_activation *result,
    const struct sync_header_processing_plan *plan);
bool syncsvc_should_log_accepted_headers(const struct p2p_node *node,
                                         const struct block_index *header_tip);
bool syncsvc_is_initial_block_download(const struct p2p_node *node,
                                       int our_height);
bool syncsvc_should_request_headers(const struct p2p_node *node,
                                    int our_height,
                                    int64_t now_seconds);
void syncsvc_plan_periodic_getheaders(struct sync_getheaders_action *action,
                                      const struct p2p_node *node,
                                      int our_height,
                                      int64_t now_seconds);
void syncsvc_note_headers_requested(struct p2p_node *node,
                                    int64_t now_seconds);
bool syncsvc_should_scan_block_files_after_headers(size_t accepted,
                                                   const struct block_index *header_tip);
enum sync_header_log_mode syncsvc_header_log_mode(
    const struct p2p_node *node,
    const struct block_index *tip,
    bool in_ibd);
bool syncsvc_should_activate_after_block_file_scan(int scanned_blocks);
bool syncsvc_should_activate_after_header_processing(
    const struct sync_header_processing_plan *plan);
bool syncsvc_should_begin_blocks_download(enum sync_state sync_state,
                                          const struct block_index *candidate,
                                          int our_height);
bool syncsvc_headers_chain_from_tip(const struct block_index *candidate,
                                    const struct block_index *tip,
                                    int our_height);
bool syncsvc_build_getheaders_locator(struct block_locator *loc,
                                      const struct active_chain *chain,
                                      const struct block_index *from,
                                      const struct uint256 *genesis_hash);

#endif
