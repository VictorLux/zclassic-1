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

#endif /* ZCL_STORAGE_EVENT_LOG_PAYLOADS_H */
