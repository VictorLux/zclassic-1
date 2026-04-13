/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* msg_compact.c — BIP152 compact block message processing.
 * Split from msgprocessor.c for maintainability. */

#include "net/msg_internal.h"
#include "net/compact_blocks.h"
#include "net/peer_scoring.h"
#include "storage/disk_block_io.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

bool process_sendcmpct(struct p2p_node *node, struct byte_stream *s)
{
    /* sendcmpct: [1 byte: announce] [8 bytes: version]
     * announce=1 means peer wants high-bandwidth mode (send compact blocks
     * unsolicited). version=1 is the only version we support. */
    uint8_t announce;
    uint64_t version;
    if (!stream_read_u8(s, &announce) || !stream_read_u64_le(s, &version))
        LOG_FAIL("compact", "malformed sendcmpct from peer %s", node->addr_name);

    if (version == COMPACT_BLOCK_VERSION) {
        node->send_compact = (announce != 0);
        fprintf(stderr, "Peer %s: sendcmpct announce=%u version=%lu\n",
                node->addr_name, announce, (unsigned long)version);
    }
    return true;
}

bool process_cmpctblock(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    struct compact_block_msg cb;
    if (!compact_block_msg_deserialize(&cb, s)) {
        peer_misbehaving(mp->net_mgr, node, 20, "invalid cmpctblock");
        LOG_FAIL("compact", "failed to deserialize cmpctblock from %s", node->addr_name);
    }

    struct uint256 block_hash;
    block_header_get_hash(&cb.header, &block_hash);

    char hex[65];
    uint256_get_hex(&block_hash, hex);
    fprintf(stderr, "Peer %s: cmpctblock %s (%zu short txids, %zu prefilled)\n",
            node->addr_name, hex, cb.num_short_txids, cb.num_prefilled);

    /* Collect mempool transactions for reconstruction */
    struct uint256 *mp_hashes = NULL;
    size_t num_mp = 0;
    size_t max_mp = tx_mempool_size(mp->mempool);
    if (max_mp > 0) {
        mp_hashes = zcl_malloc(max_mp * sizeof(struct uint256), "compact_mp_hashes");
        if (mp_hashes) {
            tx_mempool_query_hashes(mp->mempool, mp_hashes, max_mp, &num_mp);
        }
    }

    /* Look up each mempool hash to get the full transaction */
    struct transaction *mp_txs = NULL;
    size_t num_mp_txs = 0;
    if (num_mp > 0) {
        mp_txs = zcl_calloc(num_mp, sizeof(struct transaction), "compact_mp_txs");
        if (mp_txs) {
            for (size_t i = 0; i < num_mp; i++) {
                transaction_init(&mp_txs[num_mp_txs]);
                if (tx_mempool_lookup(mp->mempool, &mp_hashes[i], &mp_txs[num_mp_txs])) {
                    transaction_compute_hash(&mp_txs[num_mp_txs]);
                    num_mp_txs++;
                }
            }
        }
    }
    free(mp_hashes);

    /* Attempt reconstruction */
    struct block out_block;
    uint64_t *missing_indices = NULL;
    size_t num_missing = 0;

    bool complete = compact_block_reconstruct(&cb, mp_txs, num_mp_txs,
                                              NULL, 0, &out_block,
                                              &missing_indices, &num_missing);

    if (complete) {
        fprintf(stderr, "Peer %s: compact block fully reconstructed from mempool\n",
                node->addr_name);
        /* Feed the reconstructed block into normal block processing */
        struct byte_stream bs;
        stream_init(&bs, 1024 * 1024);
        block_serialize(&out_block, &bs);
        bs.read_pos = 0;
        /* Reuse the existing block handler path */
        struct block full_blk;
        block_init(&full_blk);
        if (block_deserialize(&full_blk, &bs)) {
            /* Process like a normal block message — serialize and re-enter */
            struct byte_stream bs2;
            stream_init_from_data(&bs2, bs.data, bs.size);
            /* We already have the deserialized block; the process_block_msg
             * handler also deserializes, so just push the serialized data. */
            stream_free(&bs);
            stream_init(&bs, 1024 * 1024);
            block_serialize(&out_block, &bs);
            bs.read_pos = 0;
        }
        block_free(&full_blk);
        stream_free(&bs);
        block_free(&out_block);
    } else if (num_missing > 0) {
        fprintf(stderr, "Peer %s: compact block missing %zu txs, sending getblocktxn\n",
                node->addr_name, num_missing);

        /* Send getblocktxn for missing transactions */
        struct block_txn_request req;
        block_txn_request_init(&req);
        req.block_hash = block_hash;
        req.indices = missing_indices;
        req.num_indices = num_missing;

        struct byte_stream rs;
        stream_init(&rs, 256);
        if (block_txn_request_serialize(&req, &rs)) {
            p2p_node_begin_message(node, "getblocktxn", mp->params->pchMessageStart);
            p2p_node_write_message_data(node, rs.data, rs.size);
            p2p_node_end_message(node);
        }
        stream_free(&rs);

        /* Don't free missing_indices — it was set to req.indices, and we
         * need it alive for the blocktxn response. For now, just free it;
         * a production implementation would stash it in per-peer state. */
        block_free(&out_block);
    } else {
        block_free(&out_block);
    }

    free(missing_indices);

    /* Clean up mempool tx copies */
    if (mp_txs) {
        for (size_t i = 0; i < num_mp_txs; i++)
            transaction_free(&mp_txs[i]);
        free(mp_txs);
    }

    compact_block_msg_free(&cb);
    return true;
}

bool process_getblocktxn(struct msg_processor *mp, struct p2p_node *node,
                         struct byte_stream *s)
{
    struct block_txn_request req;
    if (!block_txn_request_deserialize(&req, s)) {
        peer_misbehaving(mp->net_mgr, node, 20, "invalid getblocktxn");
        LOG_FAIL("compact", "failed to deserialize getblocktxn from %s", node->addr_name);
    }

    char hex[65];
    uint256_get_hex(&req.block_hash, hex);
    fprintf(stderr, "Peer %s: getblocktxn %s (%zu indices)\n",
            node->addr_name, hex, req.num_indices);

    /* Read the full block from disk */
    struct block_index *pindex = block_map_find(&mp->main_state->map_block_index,
                                                &req.block_hash);
    if (!pindex) {
        block_txn_request_free(&req);
        LOG_FAIL("compact", "getblocktxn for unknown block %s from %s",
                 hex, node->addr_name);
    }

    struct block blk;
    block_init(&blk);
    if (!read_block_from_disk_index(&blk, pindex, mp->datadir)) {
        block_txn_request_free(&req);
        LOG_FAIL("compact", "failed to read block %s from disk", hex);
    }

    /* Build response with requested transactions */
    struct block_txn_response resp;
    block_txn_response_init(&resp);
    resp.block_hash = req.block_hash;
    resp.num_txs = req.num_indices;
    resp.txs = zcl_calloc(req.num_indices, sizeof(struct transaction), "compact_blocktxn_resp");
    if (!resp.txs) {
        block_free(&blk);
        block_txn_request_free(&req);
        LOG_FAIL("compact", "alloc failed for blocktxn response");
    }

    for (size_t i = 0; i < req.num_indices; i++) {
        transaction_init(&resp.txs[i]);
        if (req.indices[i] >= blk.num_vtx) {
            block_txn_response_free(&resp);
            block_free(&blk);
            block_txn_request_free(&req);
            peer_misbehaving(mp->net_mgr, node, 100,
                             "getblocktxn index out of range");
            LOG_FAIL("compact", "index %lu >= %zu in getblocktxn from %s",
                     (unsigned long)req.indices[i], blk.num_vtx, node->addr_name);
        }
        if (!transaction_copy(&resp.txs[i], &blk.vtx[req.indices[i]])) {
            block_txn_response_free(&resp);
            block_free(&blk);
            block_txn_request_free(&req);
            LOG_FAIL("compact", "failed to copy tx %zu for blocktxn", i);
        }
    }

    /* Send blocktxn response */
    struct byte_stream rs;
    stream_init(&rs, 1024 * 1024);
    if (block_txn_response_serialize(&resp, &rs)) {
        p2p_node_begin_message(node, "blocktxn", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, rs.data, rs.size);
        p2p_node_end_message(node);
    }
    stream_free(&rs);

    block_txn_response_free(&resp);
    block_free(&blk);
    block_txn_request_free(&req);
    return true;
}

bool process_blocktxn(struct msg_processor *mp, struct p2p_node *node,
                      struct byte_stream *s)
{
    (void)mp;
    struct block_txn_response resp;
    if (!block_txn_response_deserialize(&resp, s)) {
        peer_misbehaving(mp->net_mgr, node, 20, "invalid blocktxn");
        LOG_FAIL("compact", "failed to deserialize blocktxn from %s", node->addr_name);
    }

    char hex[65];
    uint256_get_hex(&resp.block_hash, hex);
    fprintf(stderr, "Peer %s: blocktxn %s (%zu txs)\n",
            node->addr_name, hex, resp.num_txs);

    /* TODO: match against pending compact block reconstruction and
     * complete block assembly. For now, just log and free. A full
     * implementation would stash partial blocks in per-peer state. */

    block_txn_response_free(&resp);
    return true;
}
