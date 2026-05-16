/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Legacy zclassicd chain oracle helpers.
 *
 * These are synchronous, best-effort reads from a local legacy zclassicd RPC
 * endpoint. They are used only as bridge/oracle data while zclassic23 is still
 * building its native indexes, not as consensus acceptance rules.
 */

#ifndef ZCL_RPC_LEGACY_CHAIN_ORACLE_H
#define ZCL_RPC_LEGACY_CHAIN_ORACLE_H

#include <stdbool.h>
#include <stdint.h>

struct mmb_leaf;

bool legacy_chain_rpc_get_block_hash_hex(int height, char out_hex[65]);
bool legacy_chain_rpc_get_mmb_leaf(int height, struct mmb_leaf *leaf);
bool legacy_chain_rpc_get_chainwork(const uint8_t block_hash[32],
                                    uint8_t chain_work[32]);

#endif /* ZCL_RPC_LEGACY_CHAIN_ORACLE_H */
