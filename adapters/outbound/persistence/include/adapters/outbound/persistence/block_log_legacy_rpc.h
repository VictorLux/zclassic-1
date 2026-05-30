/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * block_log_legacy_rpc — read-only block_log_port adapter backed by the
 * co-located legacy zclassicd's JSON-RPC getblock path.
 *
 * WHY THIS EXISTS (vs block_log_legacy)
 * -------------------------------------
 * `block_log_legacy` opens zclassicd's `blocks/index/` LevelDB and mmaps
 * its `blk*.dat` files. That requires the LevelDB to be unlocked — in
 * practice a *snapshot copy* of a 12.7 GB data dir — and a sibling reader
 * process. For a long-lived background conservation diff Job that just
 * needs "the canonical block at height H", that is far too heavy.
 *
 * This adapter instead reads the canonical serialized block straight from
 * the running zclassicd over its already-shared RPC transport
 * (`legacy_chain_rpc_get_block_hex`: getblockhash → getblock verbose=0).
 * No LevelDB lock, no byte-copy of the data dir, no second process. The
 * transport finds zclassicd's credentials in `$HOME/.zclassic/zclassic.conf`
 * (see lib/rpc/src/legacy_rpc_client.c) — it is self-sufficient and does
 * NOT depend on mcp_rpc_client_datadir() (which returns "" inside the
 * node).
 *
 * CONTRACT (block_log_port)
 * -------------------------
 *   - append()      -> always BLOCK_LOG_ERR_NOT_SUPPORTED (read-only).
 *   - read_at_height(h) -> fetch + hex-decode the canonical block at h.
 *       On a successful fetch, *bytes_out points into a buffer owned by
 *       the handle, valid until the NEXT port call on the same handle
 *       (mirrors block_log_file / block_log_legacy ownership). Callers
 *       that must keep the bytes copy them.
 *       On an RPC failure (zclassicd unreachable, oversize, parse error)
 *       returns BLOCK_LOG_ERR_IO — distinct from BLOCK_LOG_ERR_NOT_FOUND
 *       so the caller can tell "transport down" from "height past tip".
 *       A height strictly above the legacy tip returns
 *       BLOCK_LOG_ERR_NOT_FOUND.
 *   - read_by_hash() -> BLOCK_LOG_ERR_NOT_SUPPORTED. The RPC path is
 *       height-addressed; the Job that drives this only needs height
 *       lookups, and a hash→height map would require an extra getblock.
 *   - tip_height()   -> legacy getblockcount, or UINT32_MAX on RPC error.
 *   - iter_from()    -> walks heights forward via read_at_height.
 *
 * CONCURRENCY
 * -----------
 * NOT thread-safe. Drive from a single thread (the conservation diff Job
 * runs one step at a time under the staged-sync supervisor). The owned
 * read buffer is overwritten on every read.
 */

#ifndef ZCL_ADAPTERS_OUTBOUND_PERSISTENCE_BLOCK_LOG_LEGACY_RPC_H
#define ZCL_ADAPTERS_OUTBOUND_PERSISTENCE_BLOCK_LOG_LEGACY_RPC_H

#include "ports/block_log_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block_log_legacy_rpc;

/* Open a read-only RPC-backed legacy block_log view. This does NOT
 * contact zclassicd at open time — it only allocates the handle and
 * populates the port vtable, so a temporarily-down daemon is surfaced
 * per-read (as BLOCK_LOG_ERR_IO) rather than at open. Always returns
 * ZCL_OK unless allocation fails.
 *
 * *out_handle owns the read buffer; *out_port is a populated
 * block_log_port whose `self` remains owned by the handle. Close with
 * block_log_legacy_rpc_close(). */
struct zcl_result block_log_legacy_rpc_open(
        struct block_log_legacy_rpc **out_handle,
        struct block_log_port *out_port);

void block_log_legacy_rpc_close(struct block_log_legacy_rpc *h);

#endif /* ZCL_ADAPTERS_OUTBOUND_PERSISTENCE_BLOCK_LOG_LEGACY_RPC_H */
