/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Chain-sync message family:
 *   headers, getheaders, getblocks — header/block locator exchange,
 *   block                          — full block delivery,
 *   sendcmpct, cmpctblock,
 *   getblocktxn, blocktxn          — BIP152 compact blocks.
 *
 * The actual handlers live in msg_headers.c, msg_blocks.c,
 * msg_compact.c — these dispatch-table wrappers exist so the table
 * in msgprocessor.c can be entirely uniform. */

#include "msgprocessor_internal.h"

bool mp_handle_getheaders(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    return process_getheaders(mp, node, s);
}

bool mp_handle_headers(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    return process_headers(mp, node, s);
}

bool mp_handle_getblocks(struct msg_processor *mp, struct p2p_node *node,
                         struct byte_stream *s)
{
    return process_getblocks(mp, node, s);
}

bool mp_handle_block_msg(struct msg_processor *mp, struct p2p_node *node,
                         struct byte_stream *s)
{
    return process_block_msg(mp, node, s);
}

bool mp_handle_sendcmpct(struct msg_processor *mp, struct p2p_node *node,
                         struct byte_stream *s)
{
    (void)mp;
    return process_sendcmpct(node, s);
}

bool mp_handle_cmpctblock(struct msg_processor *mp, struct p2p_node *node,
                          struct byte_stream *s)
{
    return process_cmpctblock(mp, node, s);
}

bool mp_handle_getblocktxn(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    return process_getblocktxn(mp, node, s);
}

bool mp_handle_blocktxn(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    return process_blocktxn(mp, node, s);
}
