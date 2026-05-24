/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Stable event-log payload schemas.
 *
 * These helpers serialize protocol payloads in little-endian byte order.
 * The structs are an in-process convenience only; never fwrite them
 * directly because C padding is not a wire format.
 */

#ifndef ZCL_STORAGE_EVENT_LOG_PAYLOADS_H
#define ZCL_STORAGE_EVENT_LOG_PAYLOADS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EV_PEER_ONION_MAX 62u
#define EV_PEER_OBSERVED_FIXED_LEN 40u
#define EV_PEER_DROPPED_LEN 24u

struct ev_peer_observed {
    uint8_t  ip_v4_or_v6[16];
    uint16_t port;
    uint8_t  is_onion;
    uint8_t  reserved;
    uint64_t services_bitmap;
    uint32_t observed_unix;
    int32_t  height_hint;
    uint8_t  onion_len;
    char     onion[EV_PEER_ONION_MAX];
};

struct ev_peer_dropped {
    uint8_t  ip_v4_or_v6[16];
    uint16_t port;
    uint8_t  reason;
    uint8_t  reserved[5];
};

static inline void ev_put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t ev_get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static inline void ev_put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t ev_get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0]
        | ((uint32_t)src[1] << 8)
        | ((uint32_t)src[2] << 16)
        | ((uint32_t)src[3] << 24);
}

static inline void ev_put_u64_le(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        dst[i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
}

static inline uint64_t ev_get_u64_le(const uint8_t *src)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)src[i] << (i * 8);
    return v;
}

static inline size_t
ev_peer_observed_serialized_len(const struct ev_peer_observed *ev)
{
    if (!ev) return 0;
    return EV_PEER_OBSERVED_FIXED_LEN + (size_t)ev->onion_len;
}

static inline bool
ev_peer_observed_serialize(const struct ev_peer_observed *ev,
                           uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!ev || !buf || !out_len) return false;
    if (ev->onion_len > EV_PEER_ONION_MAX) return false;
    size_t need = ev_peer_observed_serialized_len(ev);
    if (cap < need) return false;

    memcpy(buf, ev->ip_v4_or_v6, 16);
    ev_put_u16_le(buf + 16, ev->port);
    buf[18] = ev->is_onion ? 1u : 0u;
    buf[19] = 0u;
    ev_put_u64_le(buf + 20, ev->services_bitmap);
    ev_put_u32_le(buf + 28, ev->observed_unix);
    ev_put_u32_le(buf + 32, (uint32_t)ev->height_hint);
    buf[36] = ev->onion_len;
    buf[37] = buf[38] = buf[39] = 0u;
    if (ev->onion_len)
        memcpy(buf + EV_PEER_OBSERVED_FIXED_LEN, ev->onion,
               ev->onion_len);
    *out_len = need;
    return true;
}

static inline bool
ev_peer_observed_parse(const void *payload, size_t len,
                       struct ev_peer_observed *out)
{
    if (!payload || !out || len < EV_PEER_OBSERVED_FIXED_LEN)
        return false;
    const uint8_t *buf = (const uint8_t *)payload;
    uint8_t onion_len = buf[36];
    if (onion_len > EV_PEER_ONION_MAX)
        return false;
    if (len != EV_PEER_OBSERVED_FIXED_LEN + (size_t)onion_len)
        return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->ip_v4_or_v6, buf, 16);
    out->port = ev_get_u16_le(buf + 16);
    out->is_onion = buf[18] ? 1u : 0u;
    out->services_bitmap = ev_get_u64_le(buf + 20);
    out->observed_unix = ev_get_u32_le(buf + 28);
    out->height_hint = (int32_t)ev_get_u32_le(buf + 32);
    out->onion_len = onion_len;
    if (onion_len)
        memcpy(out->onion, buf + EV_PEER_OBSERVED_FIXED_LEN, onion_len);
    return true;
}

static inline bool
ev_peer_dropped_serialize(const struct ev_peer_dropped *ev,
                          uint8_t buf[EV_PEER_DROPPED_LEN])
{
    if (!ev || !buf) return false;
    memcpy(buf, ev->ip_v4_or_v6, 16);
    ev_put_u16_le(buf + 16, ev->port);
    buf[18] = ev->reason;
    memset(buf + 19, 0, 5);
    return true;
}

static inline bool
ev_peer_dropped_parse(const void *payload, size_t len,
                      struct ev_peer_dropped *out)
{
    if (!payload || !out || len != EV_PEER_DROPPED_LEN)
        return false;
    const uint8_t *buf = (const uint8_t *)payload;
    memset(out, 0, sizeof(*out));
    memcpy(out->ip_v4_or_v6, buf, 16);
    out->port = ev_get_u16_le(buf + 16);
    out->reason = buf[18];
    return true;
}

/* ── EV_UTXO_ADD / EV_UTXO_SPEND (Phase 4b — utxo_projection) ──────
 *
 * Frozen wire formats. Extending requires a new event_log_type id, not
 * an in-place change.
 *
 *   EV_UTXO_ADD (variable length, 56 + script_len bytes):
 *      [ 32B  txid                 ]
 *      [  4B  vout (LE)            ]
 *      [  8B  value, zatoshis (LE) ]
 *      [  4B  height (LE)          ]
 *      [  1B  is_coinbase (0/1)    ]
 *      [  3B  reserved (zero)      ]
 *      [  4B  script_len (LE)      ]
 *      [ NB   script bytes         ]
 *
 *   EV_UTXO_SPEND (fixed 36 bytes):
 *      [ 32B  prevout_txid         ]
 *      [  4B  prevout_vout (LE)    ]
 */

/* Header portion (everything but the trailing script). */
#define EV_UTXO_ADD_HDR_WIRE_LEN 56u
#define EV_UTXO_SPEND_WIRE_LEN   36u

struct ev_utxo_add_hdr {
    uint8_t  txid[32];
    uint32_t vout;
    int64_t  value;
    uint32_t height;
    uint8_t  is_coinbase;   /* 0 or 1 */
    uint8_t  reserved[3];   /* zero on emit; ignored on parse */
    uint32_t script_len;
};

struct ev_utxo_spend {
    uint8_t  txid[32];
    uint32_t vout;
};

/* Total wire length of an EV_UTXO_ADD payload given its header. */
static inline size_t
ev_utxo_add_serialized_len(const struct ev_utxo_add_hdr *hdr)
{
    if (!hdr) return 0;
    return (size_t)EV_UTXO_ADD_HDR_WIRE_LEN + (size_t)hdr->script_len;
}

/* Serialise an EV_UTXO_ADD payload into `out`. `out_cap` must hold
 * EV_UTXO_ADD_HDR_WIRE_LEN + hdr->script_len bytes. Writes the produced
 * length into *out_len. Returns false on truncation / NULL args /
 * (script_len > 0 && script_bytes == NULL).
 *
 * The `reserved` field in `hdr` is ignored — we always emit zero bytes
 * regardless of what's in the struct, so the wire form is deterministic. */
static inline bool
ev_utxo_add_serialize(const struct ev_utxo_add_hdr *hdr,
                      const uint8_t *script_bytes,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!hdr || !out || !out_len) return false;
    if (hdr->script_len > 0 && !script_bytes) return false;
    size_t need = ev_utxo_add_serialized_len(hdr);
    if (out_cap < need) return false;

    memcpy(out + 0, hdr->txid, 32);
    ev_put_u32_le(out + 32, hdr->vout);
    ev_put_u64_le(out + 36, (uint64_t)hdr->value);
    ev_put_u32_le(out + 44, hdr->height);
    out[48] = hdr->is_coinbase ? 1u : 0u;
    out[49] = 0u;
    out[50] = 0u;
    out[51] = 0u;
    ev_put_u32_le(out + 52, hdr->script_len);
    if (hdr->script_len > 0)
        memcpy(out + EV_UTXO_ADD_HDR_WIRE_LEN, script_bytes, hdr->script_len);

    *out_len = need;
    return true;
}

/* Parse an EV_UTXO_ADD payload. On success fills `hdr`; *script_out
 * (if non-NULL) is set to point INTO `payload` (zero-copy; valid for
 * the lifetime of `payload`). Returns false on truncation, bogus
 * script_len, or NULL `payload`/`hdr`. */
static inline bool
ev_utxo_add_parse(const uint8_t *payload, size_t payload_len,
                  struct ev_utxo_add_hdr *hdr,
                  const uint8_t **script_out,
                  size_t *script_len_out)
{
    if (!payload || !hdr) return false;
    if (payload_len < EV_UTXO_ADD_HDR_WIRE_LEN) return false;

    memcpy(hdr->txid, payload + 0, 32);
    hdr->vout        = ev_get_u32_le(payload + 32);
    hdr->value       = (int64_t)ev_get_u64_le(payload + 36);
    hdr->height      = ev_get_u32_le(payload + 44);
    hdr->is_coinbase = payload[48] ? 1u : 0u;
    hdr->reserved[0] = 0u;
    hdr->reserved[1] = 0u;
    hdr->reserved[2] = 0u;
    hdr->script_len  = ev_get_u32_le(payload + 52);

    size_t need = (size_t)EV_UTXO_ADD_HDR_WIRE_LEN + (size_t)hdr->script_len;
    if (payload_len < need) return false;

    if (script_out) {
        *script_out = (hdr->script_len > 0)
                    ? (payload + EV_UTXO_ADD_HDR_WIRE_LEN)
                    : NULL;
    }
    if (script_len_out)
        *script_len_out = hdr->script_len;
    return true;
}

static inline bool
ev_utxo_spend_serialize(const struct ev_utxo_spend *spend,
                        uint8_t out[EV_UTXO_SPEND_WIRE_LEN])
{
    if (!spend || !out) return false;
    memcpy(out + 0, spend->txid, 32);
    ev_put_u32_le(out + 32, spend->vout);
    return true;
}

static inline bool
ev_utxo_spend_parse(const void *payload, size_t payload_len,
                    struct ev_utxo_spend *spend_out)
{
    if (!payload || !spend_out) return false;
    if (payload_len != EV_UTXO_SPEND_WIRE_LEN) return false;
    const uint8_t *buf = (const uint8_t *)payload;
    memcpy(spend_out->txid, buf + 0, 32);
    spend_out->vout = ev_get_u32_le(buf + 32);
    return true;
}

/* ── EV_BLOCK_HEADER ─────────────────────────────────────────────────
 *
 * Per-block-index entry. Emitted alongside the legacy LevelDB write in
 * block_index_db.c (shadow mode for Phase 4c). The block_index_projection
 * consumes these to materialize a SQLite-backed replacement for the
 * LevelDB `b` keyspace.
 *
 * Wire layout (little-endian, no padding):
 *   bytes  0..31    hash                  (block hash)
 *   bytes 32..63    hashPrev              (previous block hash)
 *   bytes 64..67    height                (int32 LE)
 *   bytes 68..71    nStatus               (uint32 LE)
 *   bytes 72..75    nFile                 (int32 LE)
 *   bytes 76..79    nDataPos              (uint32 LE)
 *   bytes 80..83    nUndoPos              (uint32 LE)
 *   bytes 84..87    nTime                 (uint32 LE)
 *   bytes 88..91    nBits                 (uint32 LE)
 *   bytes 92..123   nNonce                (32 bytes)
 *   bytes 124..155  hashMerkleRoot        (32 bytes)
 *   bytes 156..187  hashFinalSaplingRoot  (32 bytes)
 *   bytes 188..191  nVersion              (int32 LE)
 *   bytes 192..195  nTx                   (uint32 LE)
 *   bytes 196..197  nSolutionSize         (uint16 LE)
 *   bytes 198..199  reserved              (2 bytes, MBZ)
 *   bytes 200..     nSolution             (nSolutionSize bytes)
 *
 * Total fixed prefix: 200 bytes. nSolution follows. */
#define EV_BLOCK_HEADER_FIXED_BYTES  200u
#define EV_BLOCK_HEADER_MAX_SOLUTION 1344u   /* Equihash 200,9 = 1344 B */

struct ev_block_header {
    uint8_t  hash[32];
    uint8_t  hashPrev[32];
    int32_t  height;
    uint32_t nStatus;
    int32_t  nFile;
    uint32_t nDataPos;
    uint32_t nUndoPos;
    uint32_t nTime;
    uint32_t nBits;
    uint8_t  nNonce[32];
    uint8_t  hashMerkleRoot[32];
    uint8_t  hashFinalSaplingRoot[32];
    int32_t  nVersion;
    uint32_t nTx;
    uint16_t nSolutionSize;
    uint8_t  reserved[2];
    /* nSolution bytes follow on the wire (nSolutionSize bytes). In the
     * in-memory struct, the caller passes the solution pointer
     * separately to ev_block_header_serialize() / receives it back from
     * ev_block_header_parse(). */
};

/* Returns the on-disk size of the serialization for the given solution
 * size. Pass `nSolutionSize` from the struct. */
static inline size_t ev_block_header_wire_size(uint16_t nSolutionSize)
{
    return (size_t)EV_BLOCK_HEADER_FIXED_BYTES + (size_t)nSolutionSize;
}

/* Serialize a header into `out` (must have at least
 * ev_block_header_wire_size(h->nSolutionSize) bytes). The trailing
 * solution comes from `solution` (may be NULL iff nSolutionSize == 0).
 * Returns true on success, false on bad input. */
bool ev_block_header_serialize(const struct ev_block_header *h,
                               const uint8_t *solution,
                               uint8_t *out, size_t out_cap,
                               size_t *out_written);

/* Parse a serialized header from `in`. The fixed-size fields populate
 * `*h_out`. The pointer `*solution_out` is set to the trailing solution
 * bytes inside `in` (no copy) — valid only while `in` remains alive.
 * Returns true on success, false on truncation / size mismatch. */
bool ev_block_header_parse(const uint8_t *in, size_t in_len,
                           struct ev_block_header *h_out,
                           const uint8_t **solution_out);

/* ── ZNAM events (Phase 4d-4 — znam_projection) ───────────────────
 *
 * Frozen wire formats. Variable-length strings carry an explicit u8
 * length prefix so the projection can validate every byte. All names
 * are bounded by ZNAM_NAME_MAX (63). Owner addresses are text (P2PKH
 * t-address style strings <= 64 chars). Target / value strings are
 * bounded by ZNAM_VALUE_MAX (128) and ZNAM_TEXT_VAL_MAX (128).
 *
 *   EV_ZNAM_REGISTER (variable):
 *     [ 1B  name_len               ]
 *     [ NB  name                   ]
 *     [ 1B  owner_len              ]
 *     [ NB  owner_address          ]
 *     [ 1B  target_type            ]
 *     [ 1B  target_value_len       ]
 *     [ NB  target_value           ]
 *     [ 32B reg_txid               ]
 *     [  4B reg_height (LE, i32)   ]
 *     [  4B registered_unix (LE)   ]
 *     [  4B expiry_height (LE,i32) ]
 *
 *   EV_ZNAM_UPDATE (variable):
 *     [ 1B  name_len               ]
 *     [ NB  name                   ]
 *     [ 1B  action_type            ]   // 0=addr_record,1=text_record,2=primary
 *     [ 1B  key_or_coin_type       ]   // coin_type for action 0/2; ignored if 1
 *     [ 1B  key_len                ]   // text_record key len; 0 for action 0/2
 *     [ NB  key                    ]
 *     [ 1B  value_len              ]
 *     [ NB  value                  ]
 *     [ 32B update_txid            ]
 *
 *   EV_ZNAM_TRANSFER (variable):
 *     [ 1B  name_len               ]
 *     [ NB  name                   ]
 *     [ 1B  new_owner_len          ]
 *     [ NB  new_owner              ]
 *     [ 32B update_txid            ]
 *
 *   EV_ZNAM_RENEW (variable):
 *     [ 1B  name_len               ]
 *     [ NB  name                   ]
 *     [  4B new_expiry_height(LE)  ]
 *     [ 32B update_txid            ]
 *
 *   EV_ZNAM_EXPIRE (variable):
 *     [ 1B  name_len               ]
 *     [ NB  name                   ]
 *     [  4B expired_at_height(LE)  ]
 */

#define EV_ZNAM_NAME_MAX     63u
#define EV_ZNAM_OWNER_MAX    64u
#define EV_ZNAM_VALUE_MAX   128u
#define EV_ZNAM_KEY_MAX      32u

#define EV_ZNAM_UPDATE_ACTION_ADDR        0u
#define EV_ZNAM_UPDATE_ACTION_TEXT        1u
#define EV_ZNAM_UPDATE_ACTION_PRIMARY     2u

struct ev_znam_register {
    uint8_t  name_len;
    char     name[EV_ZNAM_NAME_MAX + 1];
    uint8_t  owner_len;
    char     owner_address[EV_ZNAM_OWNER_MAX + 1];
    uint8_t  target_type;
    uint8_t  target_value_len;
    char     target_value[EV_ZNAM_VALUE_MAX + 1];
    uint8_t  reg_txid[32];
    int32_t  reg_height;
    uint32_t registered_unix;
    int32_t  expiry_height;
};

struct ev_znam_update {
    uint8_t  name_len;
    char     name[EV_ZNAM_NAME_MAX + 1];
    uint8_t  action_type;            /* 0=addr,1=text,2=primary */
    uint8_t  key_or_coin_type;       /* coin_type for action 0/2, ignored if 1 */
    uint8_t  key_len;
    char     key[EV_ZNAM_KEY_MAX + 1];
    uint8_t  value_len;
    char     value[EV_ZNAM_VALUE_MAX + 1];
    uint8_t  update_txid[32];
};

struct ev_znam_transfer {
    uint8_t  name_len;
    char     name[EV_ZNAM_NAME_MAX + 1];
    uint8_t  new_owner_len;
    char     new_owner[EV_ZNAM_OWNER_MAX + 1];
    uint8_t  update_txid[32];
};

struct ev_znam_renew {
    uint8_t  name_len;
    char     name[EV_ZNAM_NAME_MAX + 1];
    int32_t  new_expiry_height;
    uint8_t  update_txid[32];
};

struct ev_znam_expire {
    uint8_t  name_len;
    char     name[EV_ZNAM_NAME_MAX + 1];
    int32_t  expired_at_height;
};

/* ── EV_ZNAM_REGISTER ──────────────────────────────────────────── */

static inline size_t
ev_znam_register_serialized_len(const struct ev_znam_register *ev)
{
    if (!ev) return 0;
    return (size_t)1 + ev->name_len + 1 + ev->owner_len + 1 + 1 +
           ev->target_value_len + 32 + 4 + 4 + 4;
}

static inline bool
ev_znam_register_serialize(const struct ev_znam_register *ev,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ev || !out || !out_len) return false;
    if (ev->name_len == 0 || ev->name_len > EV_ZNAM_NAME_MAX) return false;
    if (ev->owner_len == 0 || ev->owner_len > EV_ZNAM_OWNER_MAX) return false;
    if (ev->target_value_len > EV_ZNAM_VALUE_MAX) return false;
    size_t need = ev_znam_register_serialized_len(ev);
    if (out_cap < need) return false;
    size_t off = 0;
    out[off++] = ev->name_len;
    memcpy(out + off, ev->name, ev->name_len); off += ev->name_len;
    out[off++] = ev->owner_len;
    memcpy(out + off, ev->owner_address, ev->owner_len); off += ev->owner_len;
    out[off++] = ev->target_type;
    out[off++] = ev->target_value_len;
    if (ev->target_value_len)
        memcpy(out + off, ev->target_value, ev->target_value_len);
    off += ev->target_value_len;
    memcpy(out + off, ev->reg_txid, 32); off += 32;
    ev_put_u32_le(out + off, (uint32_t)ev->reg_height); off += 4;
    ev_put_u32_le(out + off, ev->registered_unix); off += 4;
    ev_put_u32_le(out + off, (uint32_t)ev->expiry_height); off += 4;
    *out_len = off;
    return true;
}

static inline bool
ev_znam_register_parse(const void *payload, size_t len,
                       struct ev_znam_register *out)
{
    if (!payload || !out) return false;
    if (len < 1) return false;
    const uint8_t *buf = (const uint8_t *)payload;
    size_t off = 0;
    memset(out, 0, sizeof(*out));
    out->name_len = buf[off++];
    if (out->name_len == 0 || out->name_len > EV_ZNAM_NAME_MAX) return false;
    if (off + out->name_len + 1 > len) return false;
    memcpy(out->name, buf + off, out->name_len); off += out->name_len;
    out->owner_len = buf[off++];
    if (out->owner_len == 0 || out->owner_len > EV_ZNAM_OWNER_MAX) return false;
    if (off + out->owner_len + 2 > len) return false;
    memcpy(out->owner_address, buf + off, out->owner_len); off += out->owner_len;
    out->target_type = buf[off++];
    out->target_value_len = buf[off++];
    if (out->target_value_len > EV_ZNAM_VALUE_MAX) return false;
    if (off + out->target_value_len + 32 + 12 > len) return false;
    if (out->target_value_len)
        memcpy(out->target_value, buf + off, out->target_value_len);
    off += out->target_value_len;
    memcpy(out->reg_txid, buf + off, 32); off += 32;
    out->reg_height     = (int32_t)ev_get_u32_le(buf + off); off += 4;
    out->registered_unix = ev_get_u32_le(buf + off); off += 4;
    out->expiry_height  = (int32_t)ev_get_u32_le(buf + off); off += 4;
    if (off != len) return false;
    return true;
}

/* ── EV_ZNAM_UPDATE ─────────────────────────────────────────────── */

static inline size_t
ev_znam_update_serialized_len(const struct ev_znam_update *ev)
{
    if (!ev) return 0;
    return (size_t)1 + ev->name_len + 1 + 1 + 1 + ev->key_len + 1 +
           ev->value_len + 32;
}

static inline bool
ev_znam_update_serialize(const struct ev_znam_update *ev,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ev || !out || !out_len) return false;
    if (ev->name_len == 0 || ev->name_len > EV_ZNAM_NAME_MAX) return false;
    if (ev->key_len > EV_ZNAM_KEY_MAX) return false;
    if (ev->value_len > EV_ZNAM_VALUE_MAX) return false;
    size_t need = ev_znam_update_serialized_len(ev);
    if (out_cap < need) return false;
    size_t off = 0;
    out[off++] = ev->name_len;
    memcpy(out + off, ev->name, ev->name_len); off += ev->name_len;
    out[off++] = ev->action_type;
    out[off++] = ev->key_or_coin_type;
    out[off++] = ev->key_len;
    if (ev->key_len)
        memcpy(out + off, ev->key, ev->key_len);
    off += ev->key_len;
    out[off++] = ev->value_len;
    if (ev->value_len)
        memcpy(out + off, ev->value, ev->value_len);
    off += ev->value_len;
    memcpy(out + off, ev->update_txid, 32); off += 32;
    *out_len = off;
    return true;
}

static inline bool
ev_znam_update_parse(const void *payload, size_t len,
                     struct ev_znam_update *out)
{
    if (!payload || !out) return false;
    if (len < 1) return false;
    const uint8_t *buf = (const uint8_t *)payload;
    size_t off = 0;
    memset(out, 0, sizeof(*out));
    out->name_len = buf[off++];
    if (out->name_len == 0 || out->name_len > EV_ZNAM_NAME_MAX) return false;
    if (off + out->name_len + 3 > len) return false;
    memcpy(out->name, buf + off, out->name_len); off += out->name_len;
    out->action_type      = buf[off++];
    out->key_or_coin_type = buf[off++];
    out->key_len          = buf[off++];
    if (out->key_len > EV_ZNAM_KEY_MAX) return false;
    if (off + out->key_len + 1 > len) return false;
    if (out->key_len)
        memcpy(out->key, buf + off, out->key_len);
    off += out->key_len;
    out->value_len = buf[off++];
    if (out->value_len > EV_ZNAM_VALUE_MAX) return false;
    if (off + out->value_len + 32 > len) return false;
    if (out->value_len)
        memcpy(out->value, buf + off, out->value_len);
    off += out->value_len;
    memcpy(out->update_txid, buf + off, 32); off += 32;
    if (off != len) return false;
    return true;
}

/* ── EV_ZNAM_TRANSFER ──────────────────────────────────────────── */

static inline size_t
ev_znam_transfer_serialized_len(const struct ev_znam_transfer *ev)
{
    if (!ev) return 0;
    return (size_t)1 + ev->name_len + 1 + ev->new_owner_len + 32;
}

static inline bool
ev_znam_transfer_serialize(const struct ev_znam_transfer *ev,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ev || !out || !out_len) return false;
    if (ev->name_len == 0 || ev->name_len > EV_ZNAM_NAME_MAX) return false;
    if (ev->new_owner_len == 0 || ev->new_owner_len > EV_ZNAM_OWNER_MAX)
        return false;
    size_t need = ev_znam_transfer_serialized_len(ev);
    if (out_cap < need) return false;
    size_t off = 0;
    out[off++] = ev->name_len;
    memcpy(out + off, ev->name, ev->name_len); off += ev->name_len;
    out[off++] = ev->new_owner_len;
    memcpy(out + off, ev->new_owner, ev->new_owner_len); off += ev->new_owner_len;
    memcpy(out + off, ev->update_txid, 32); off += 32;
    *out_len = off;
    return true;
}

static inline bool
ev_znam_transfer_parse(const void *payload, size_t len,
                       struct ev_znam_transfer *out)
{
    if (!payload || !out) return false;
    if (len < 1) return false;
    const uint8_t *buf = (const uint8_t *)payload;
    size_t off = 0;
    memset(out, 0, sizeof(*out));
    out->name_len = buf[off++];
    if (out->name_len == 0 || out->name_len > EV_ZNAM_NAME_MAX) return false;
    if (off + out->name_len + 1 > len) return false;
    memcpy(out->name, buf + off, out->name_len); off += out->name_len;
    out->new_owner_len = buf[off++];
    if (out->new_owner_len == 0 || out->new_owner_len > EV_ZNAM_OWNER_MAX)
        return false;
    if (off + out->new_owner_len + 32 > len) return false;
    memcpy(out->new_owner, buf + off, out->new_owner_len);
    off += out->new_owner_len;
    memcpy(out->update_txid, buf + off, 32); off += 32;
    if (off != len) return false;
    return true;
}

/* ── EV_ZNAM_RENEW ─────────────────────────────────────────────── */

static inline size_t
ev_znam_renew_serialized_len(const struct ev_znam_renew *ev)
{
    if (!ev) return 0;
    return (size_t)1 + ev->name_len + 4 + 32;
}

static inline bool
ev_znam_renew_serialize(const struct ev_znam_renew *ev,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ev || !out || !out_len) return false;
    if (ev->name_len == 0 || ev->name_len > EV_ZNAM_NAME_MAX) return false;
    size_t need = ev_znam_renew_serialized_len(ev);
    if (out_cap < need) return false;
    size_t off = 0;
    out[off++] = ev->name_len;
    memcpy(out + off, ev->name, ev->name_len); off += ev->name_len;
    ev_put_u32_le(out + off, (uint32_t)ev->new_expiry_height); off += 4;
    memcpy(out + off, ev->update_txid, 32); off += 32;
    *out_len = off;
    return true;
}

static inline bool
ev_znam_renew_parse(const void *payload, size_t len,
                    struct ev_znam_renew *out)
{
    if (!payload || !out) return false;
    if (len < 1) return false;
    const uint8_t *buf = (const uint8_t *)payload;
    size_t off = 0;
    memset(out, 0, sizeof(*out));
    out->name_len = buf[off++];
    if (out->name_len == 0 || out->name_len > EV_ZNAM_NAME_MAX) return false;
    if (off + out->name_len + 4 + 32 != len) return false;
    memcpy(out->name, buf + off, out->name_len); off += out->name_len;
    out->new_expiry_height = (int32_t)ev_get_u32_le(buf + off); off += 4;
    memcpy(out->update_txid, buf + off, 32); off += 32;
    return true;
}

/* ── EV_ZNAM_EXPIRE ────────────────────────────────────────────── */

static inline size_t
ev_znam_expire_serialized_len(const struct ev_znam_expire *ev)
{
    if (!ev) return 0;
    return (size_t)1 + ev->name_len + 4;
}

static inline bool
ev_znam_expire_serialize(const struct ev_znam_expire *ev,
                         uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ev || !out || !out_len) return false;
    if (ev->name_len == 0 || ev->name_len > EV_ZNAM_NAME_MAX) return false;
    size_t need = ev_znam_expire_serialized_len(ev);
    if (out_cap < need) return false;
    size_t off = 0;
    out[off++] = ev->name_len;
    memcpy(out + off, ev->name, ev->name_len); off += ev->name_len;
    ev_put_u32_le(out + off, (uint32_t)ev->expired_at_height); off += 4;
    *out_len = off;
    return true;
}

static inline bool
ev_znam_expire_parse(const void *payload, size_t len,
                     struct ev_znam_expire *out)
{
    if (!payload || !out) return false;
    if (len < 1) return false;
    const uint8_t *buf = (const uint8_t *)payload;
    size_t off = 0;
    memset(out, 0, sizeof(*out));
    out->name_len = buf[off++];
    if (out->name_len == 0 || out->name_len > EV_ZNAM_NAME_MAX) return false;
    if (off + out->name_len + 4 != len) return false;
    memcpy(out->name, buf + off, out->name_len); off += out->name_len;
    out->expired_at_height = (int32_t)ev_get_u32_le(buf + off); off += 4;
    return true;
}

#endif /* ZCL_STORAGE_EVENT_LOG_PAYLOADS_H */
