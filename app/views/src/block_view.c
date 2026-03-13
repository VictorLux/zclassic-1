/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "views/block_view.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "json/json.h"
#include <stdio.h>
#include <stdint.h>

void block_view_brief(struct json_value *out,
                      const struct block_index *bi,
                      int tip_height)
{
    json_set_object(out);

    char hex[65];
    if (bi->phashBlock) {
        uint256_get_hex(bi->phashBlock, hex);
        json_push_kv_str(out, "hash", hex);
    }

    json_push_kv_int(out, "height", bi->nHeight);
    json_push_kv_int(out, "confirmations",
                     (int64_t)(tip_height - bi->nHeight + 1));
    json_push_kv_int(out, "time", (int64_t)bi->nTime);
}

void block_view_full(struct json_value *out,
                     const struct block_index *bi,
                     int tip_height,
                     const char *datadir)
{
    (void)datadir;
    json_set_object(out);

    char hex[65];
    if (bi->phashBlock) {
        uint256_get_hex(bi->phashBlock, hex);
        json_push_kv_str(out, "hash", hex);
    }

    json_push_kv_int(out, "confirmations",
                     (int64_t)(tip_height - bi->nHeight + 1));
    json_push_kv_int(out, "height", bi->nHeight);
    json_push_kv_int(out, "version", bi->nVersion);
    json_push_kv_int(out, "time", (int64_t)bi->nTime);
    json_push_kv_int(out, "bits", (int64_t)bi->nBits);

    uint256_get_hex(&bi->nNonce, hex);
    json_push_kv_str(out, "nonce", hex);

    uint256_get_hex(&bi->hashMerkleRoot, hex);
    json_push_kv_str(out, "merkleroot", hex);

    if (bi->pprev && bi->pprev->phashBlock) {
        uint256_get_hex(bi->pprev->phashBlock, hex);
        json_push_kv_str(out, "previousblockhash", hex);
    }
}
