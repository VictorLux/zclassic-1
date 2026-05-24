/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/snapshot_manifest.h"

#include "core/serialize.h"
#include "net/fast_sync.h"
#include "validation/sync_evidence_policy.h"

#include <string.h>

static bool bytes32_nonzero(const uint8_t bytes[32])
{
    return !zcl_chainwork_is_zero(bytes);
}

bool snapshot_manifest_parse(struct snapshot_manifest *out,
                                struct byte_stream *s,
                                enum snapshot_manifest_result *result)
{
    if (result)
        *result = SNAPSHOT_MANIFEST_OK;
    if (!out || !s) {
        if (result)
            *result = SNAPSHOT_MANIFEST_NULL_ARG;
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (!stream_read_i32_le(s, &out->height) ||
        !stream_read_bytes(s, out->block_hash, 32) ||
        !stream_read_bytes(s, out->utxo_root, 32) ||
        !stream_read_bytes(s, out->mmr_root, 32) ||
        !stream_read_u64_le(s, &out->num_utxos) ||
        !stream_read_u64_le(s, &out->total_bytes) ||
        !stream_read_bytes(s, out->mmb_root, 32) ||
        !stream_read_u32_le(s, &out->protocol_version) ||
        !stream_read_u32_le(s, &out->snapshot_schema_version) ||
        !stream_read_i32_le(s, &out->peer_tip_height) ||
        !stream_read_bytes(s, out->chain_work, 32)) {
        if (result)
            *result = SNAPSHOT_MANIFEST_TRUNCATED;
        return false;
    }

    if (s->read_pos != s->size) {
        if (result)
            *result = SNAPSHOT_MANIFEST_TRAILING_BYTES;
        return false;
    }
    return true;
}

static enum snapshot_manifest_result snapshot_manifest_validate_common(
    const struct snapshot_manifest *m)
{
    if (!m)
        return SNAPSHOT_MANIFEST_NULL_ARG;
    if (m->height < 0 ||
        m->num_utxos == 0 ||
        m->total_bytes == 0 ||
        m->num_utxos > 100000000ULL ||
        m->total_bytes > 100ULL * 1024 * 1024 * 1024)
        return SNAPSHOT_MANIFEST_RANGE;
    if (m->protocol_version != FAST_SYNC_PROTOCOL_VERSION ||
        m->snapshot_schema_version != FAST_SYNC_SNAPSHOT_SCHEMA_VERSION)
        return SNAPSHOT_MANIFEST_STALE_SCHEMA;
    if (!zcl_is_snapshot_anchor_acceptable(m->height, m->peer_tip_height))
        return SNAPSHOT_MANIFEST_UNFINAL;
    if (zcl_chainwork_is_zero(m->chain_work))
        return SNAPSHOT_MANIFEST_WEAK_WORK;
    if (!bytes32_nonzero(m->mmr_root))
        return SNAPSHOT_MANIFEST_NO_MMR;
    if (!bytes32_nonzero(m->mmb_root))
        return SNAPSHOT_MANIFEST_NO_MMB;
    return SNAPSHOT_MANIFEST_OK;
}

enum snapshot_manifest_result snapshot_manifest_validate_offer(
    const struct snapshot_manifest *m,
    int32_t our_height)
{
    enum snapshot_manifest_result common =
        snapshot_manifest_validate_common(m);
    if (common != SNAPSHOT_MANIFEST_OK)
        return common;
    if (m->height <= our_height + 5000)
        return SNAPSHOT_MANIFEST_NOT_AHEAD;
    return SNAPSHOT_MANIFEST_OK;
}

enum snapshot_manifest_result snapshot_manifest_validate_recovery(
    const struct snapshot_manifest *m,
    int32_t target_height)
{
    enum snapshot_manifest_result common =
        snapshot_manifest_validate_common(m);
    if (common != SNAPSHOT_MANIFEST_OK)
        return common;
    if (target_height <= 0)
        return SNAPSHOT_MANIFEST_RANGE;
    if (m->height < target_height)
        return SNAPSHOT_MANIFEST_NOT_AHEAD;
    return SNAPSHOT_MANIFEST_OK;
}

const char *snapshot_manifest_result_name(
    enum snapshot_manifest_result result)
{
    switch (result) {
    case SNAPSHOT_MANIFEST_OK:             return "ok";
    case SNAPSHOT_MANIFEST_NULL_ARG:       return "null_arg";
    case SNAPSHOT_MANIFEST_TRUNCATED:      return "truncated";
    case SNAPSHOT_MANIFEST_TRAILING_BYTES: return "trailing_bytes";
    case SNAPSHOT_MANIFEST_RANGE:          return "range";
    case SNAPSHOT_MANIFEST_STALE_SCHEMA:   return "stale_schema";
    case SNAPSHOT_MANIFEST_UNFINAL:        return "unfinal";
    case SNAPSHOT_MANIFEST_WEAK_WORK:      return "weak_work";
    case SNAPSHOT_MANIFEST_NO_MMR:         return "no_mmr";
    case SNAPSHOT_MANIFEST_NO_MMB:         return "no_mmb";
    case SNAPSHOT_MANIFEST_NOT_AHEAD:      return "not_ahead";
    default:                                  return "unknown";
    }
}
