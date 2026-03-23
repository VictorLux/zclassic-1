/* Copyright (c) 2012 Pieter Wuille
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/addrman.h"
#include "core/hash.h"
#include "core/random.h"
#include "core/serialize.h"
#include "util/timedata.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int addr_info_get_tried_bucket(const struct addr_info *info,
                               const struct uint256 *nKey)
{
    unsigned char key[NET_SERVICE_KEY_SIZE];
    net_service_get_key(&info->addr.svc, key);

    unsigned char buf[32 + NET_SERVICE_KEY_SIZE];
    memcpy(buf, nKey->data, 32);
    memcpy(buf + 32, key, NET_SERVICE_KEY_SIZE);
    struct uint256 hash1;
    hash256(buf, sizeof(buf), hash1.data);
    uint64_t h1;
    memcpy(&h1, hash1.data, sizeof(h1));

    unsigned char group[NET_ADDR_GROUP_MAX];
    size_t glen = net_addr_get_group(&info->addr.svc.addr, group, sizeof(group));

    unsigned char buf2[32 + NET_ADDR_GROUP_MAX + 8];
    memcpy(buf2, nKey->data, 32);
    memcpy(buf2 + 32, group, glen);
    uint64_t bucket_seed = h1 % ADDRMAN_TRIED_BUCKETS_PER_GROUP;
    memcpy(buf2 + 32 + glen, &bucket_seed, sizeof(bucket_seed));
    struct uint256 hash2;
    hash256(buf2, 32 + glen + sizeof(bucket_seed), hash2.data);
    uint64_t h2;
    memcpy(&h2, hash2.data, sizeof(h2));

    return (int)(h2 % ADDRMAN_TRIED_BUCKET_COUNT);
}

int addr_info_get_new_bucket(const struct addr_info *info,
                             const struct uint256 *nKey,
                             const struct net_addr *src)
{
    unsigned char src_group[NET_ADDR_GROUP_MAX];
    size_t sglen = net_addr_get_group(src, src_group, sizeof(src_group));

    unsigned char my_group[NET_ADDR_GROUP_MAX];
    size_t mglen = net_addr_get_group(&info->addr.svc.addr, my_group,
                                       sizeof(my_group));

    unsigned char buf1[32 + NET_ADDR_GROUP_MAX + NET_ADDR_GROUP_MAX];
    memcpy(buf1, nKey->data, 32);
    memcpy(buf1 + 32, my_group, mglen);
    memcpy(buf1 + 32 + mglen, src_group, sglen);
    struct uint256 hash1;
    hash256(buf1, 32 + mglen + sglen, hash1.data);
    uint64_t h1;
    memcpy(&h1, hash1.data, sizeof(h1));

    unsigned char buf2[32 + NET_ADDR_GROUP_MAX + 8];
    memcpy(buf2, nKey->data, 32);
    memcpy(buf2 + 32, src_group, sglen);
    uint64_t bucket_seed = h1 % ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP;
    memcpy(buf2 + 32 + sglen, &bucket_seed, sizeof(bucket_seed));
    struct uint256 hash2;
    hash256(buf2, 32 + sglen + sizeof(bucket_seed), hash2.data);
    uint64_t h2;
    memcpy(&h2, hash2.data, sizeof(h2));

    return (int)(h2 % ADDRMAN_NEW_BUCKET_COUNT);
}

int addr_info_get_bucket_position(const struct addr_info *info,
                                  const struct uint256 *nKey,
                                  bool fNew, int nBucket)
{
    unsigned char key[NET_SERVICE_KEY_SIZE];
    net_service_get_key(&info->addr.svc, key);

    unsigned char buf[32 + 1 + 4 + NET_SERVICE_KEY_SIZE];
    memcpy(buf, nKey->data, 32);
    buf[32] = fNew ? 'N' : 'K';
    memcpy(buf + 33, &nBucket, 4);
    memcpy(buf + 37, key, NET_SERVICE_KEY_SIZE);
    struct uint256 h;
    hash256(buf, sizeof(buf), h.data);
    uint64_t r;
    memcpy(&r, h.data, sizeof(r));
    return (int)(r % ADDRMAN_BUCKET_SIZE);
}

bool addr_info_is_terrible(const struct addr_info *info, int64_t nNow)
{
    if (info->last_try && info->last_try >= nNow - 60)
        return false;
    if ((int64_t)info->addr.nTime > nNow + 10 * 60)
        return true;
    if (info->addr.nTime == 0 ||
        nNow - (int64_t)info->addr.nTime > ADDRMAN_HORIZON_DAYS * 24 * 60 * 60)
        return true;
    if (info->last_success == 0 && info->attempts >= ADDRMAN_RETRIES)
        return true;
    if (nNow - info->last_success > ADDRMAN_MIN_FAIL_DAYS * 24 * 60 * 60 &&
        info->attempts >= ADDRMAN_MAX_FAILURES)
        return true;
    return false;
}

double addr_info_get_chance(const struct addr_info *info, int64_t nNow)
{
    double fChance = 1.0;
    int64_t nSinceLastTry = nNow - info->last_try;
    if (nSinceLastTry < 0) nSinceLastTry = 0;
    if (nSinceLastTry < 60 * 10)
        fChance *= 0.01;
    int n = info->attempts < 8 ? info->attempts : 8;
    fChance *= pow(0.66, n);
    return fChance;
}

void addrman_init(struct addr_man *am)
{
    zcl_mutex_init(&am->cs);
    GetRandBytes(am->nKey.data, 32);
    am->id_count = 0;
    am->tried_count = 0;
    am->new_count = 0;
    am->random_order = NULL;
    am->random_size = 0;
    am->random_cap = 0;
    am->entries_cap = 4096;
    am->entries = calloc(am->entries_cap, sizeof(struct addr_info));
    for (int i = 0; i < ADDRMAN_NEW_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvNew[i][j] = -1;
    for (int i = 0; i < ADDRMAN_TRIED_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvTried[i][j] = -1;
}

void addrman_free(struct addr_man *am)
{
    free(am->random_order);
    free(am->entries);
    am->random_order = NULL;
    am->entries = NULL;
    zcl_mutex_destroy(&am->cs);
}

void addrman_clear(struct addr_man *am)
{
    GetRandBytes(am->nKey.data, 32);
    am->id_count = 0;
    am->tried_count = 0;
    am->new_count = 0;
    free(am->random_order);
    am->random_order = NULL;
    am->random_size = 0;
    am->random_cap = 0;
    if (am->entries)
        memset(am->entries, 0, am->entries_cap * sizeof(struct addr_info));
    for (int i = 0; i < ADDRMAN_NEW_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvNew[i][j] = -1;
    for (int i = 0; i < ADDRMAN_TRIED_BUCKET_COUNT; i++)
        for (int j = 0; j < ADDRMAN_BUCKET_SIZE; j++)
            am->vvTried[i][j] = -1;
}

size_t addrman_size(const struct addr_man *am)
{
    return am->random_size;
}

static void random_push(struct addr_man *am, int id)
{
    if (am->random_size >= am->random_cap) {
        size_t new_cap = am->random_cap ? am->random_cap * 2 : 256;
        int *p = realloc(am->random_order, new_cap * sizeof(int));
        if (!p) return;
        am->random_order = p;
        am->random_cap = new_cap;
    }
    am->random_order[am->random_size++] = id;
}

static void swap_random(struct addr_man *am, unsigned int p1, unsigned int p2)
{
    if (p1 == p2) return;
    int id1 = am->random_order[p1];
    int id2 = am->random_order[p2];
    am->entries[id1].random_pos = (int)p2;
    am->entries[id2].random_pos = (int)p1;
    am->random_order[p1] = id2;
    am->random_order[p2] = id1;
}

static struct addr_info *find_addr(struct addr_man *am,
                                    const struct net_addr *addr, int *pnId)
{
    for (int i = 0; i < am->id_count; i++) {
        if (am->entries[i].used &&
            net_addr_eq(&am->entries[i].addr.svc.addr, addr)) {
            if (pnId) *pnId = i;
            return &am->entries[i];
        }
    }
    return NULL;
}

static struct addr_info *create_entry(struct addr_man *am,
                                       const struct net_address *addr,
                                       const struct net_addr *source,
                                       int *pnId)
{
    int id = am->id_count;
    if ((size_t)id >= am->entries_cap) {
        size_t new_cap = am->entries_cap * 2;
        if (new_cap > ADDRMAN_MAX_ENTRIES) new_cap = ADDRMAN_MAX_ENTRIES;
        if ((size_t)id >= new_cap) return NULL;
        struct addr_info *p = realloc(am->entries,
                                       new_cap * sizeof(struct addr_info));
        if (!p) return NULL;
        memset(p + am->entries_cap, 0,
               (new_cap - am->entries_cap) * sizeof(struct addr_info));
        am->entries = p;
        am->entries_cap = new_cap;
    }
    am->id_count++;

    struct addr_info *info = &am->entries[id];
    memset(info, 0, sizeof(*info));
    info->addr = *addr;
    info->source = *source;
    info->last_success = 0;
    info->last_try = 0;
    info->attempts = 0;
    info->ref_count = 0;
    info->in_tried = false;
    info->random_pos = (int)am->random_size;
    info->used = true;
    random_push(am, id);

    if (pnId) *pnId = id;
    return info;
}

static void delete_entry(struct addr_man *am, int nId)
{
    struct addr_info *info = &am->entries[nId];
    swap_random(am, (unsigned int)info->random_pos,
                (unsigned int)(am->random_size - 1));
    am->random_size--;
    info->used = false;
    am->new_count--;
}

static void clear_new(struct addr_man *am, int nUBucket, int nUBucketPos)
{
    if (am->vvNew[nUBucket][nUBucketPos] != -1) {
        int nIdDelete = am->vvNew[nUBucket][nUBucketPos];
        struct addr_info *info = &am->entries[nIdDelete];
        info->ref_count--;
        am->vvNew[nUBucket][nUBucketPos] = -1;
        if (info->ref_count == 0)
            delete_entry(am, nIdDelete);
    }
}

static void make_tried(struct addr_man *am, struct addr_info *info, int nId)
{
    for (int bucket = 0; bucket < ADDRMAN_NEW_BUCKET_COUNT; bucket++) {
        int pos = addr_info_get_bucket_position(info, &am->nKey, true, bucket);
        if (am->vvNew[bucket][pos] == nId) {
            am->vvNew[bucket][pos] = -1;
            info->ref_count--;
        }
    }
    am->new_count--;

    int nKBucket = addr_info_get_tried_bucket(info, &am->nKey);
    int nKBucketPos = addr_info_get_bucket_position(info, &am->nKey, false,
                                                     nKBucket);

    if (am->vvTried[nKBucket][nKBucketPos] != -1) {
        int nIdEvict = am->vvTried[nKBucket][nKBucketPos];
        struct addr_info *old = &am->entries[nIdEvict];
        old->in_tried = false;
        am->vvTried[nKBucket][nKBucketPos] = -1;
        am->tried_count--;

        int nUBucket = addr_info_get_new_bucket(old, &am->nKey, &old->source);
        int nUBucketPos = addr_info_get_bucket_position(old, &am->nKey, true,
                                                         nUBucket);
        clear_new(am, nUBucket, nUBucketPos);
        old->ref_count = 1;
        am->vvNew[nUBucket][nUBucketPos] = nIdEvict;
        am->new_count++;
    }

    am->vvTried[nKBucket][nKBucketPos] = nId;
    am->tried_count++;
    info->in_tried = true;
}

bool addrman_add(struct addr_man *am, const struct net_address *addr,
                 const struct net_addr *source, int64_t time_penalty)
{
    if (!net_addr_is_routable(&addr->svc.addr))
        return false;

    zcl_mutex_lock(&am->cs);

    bool fNew = false;
    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->svc.addr, &nId);

    if (pinfo) {
        int64_t nUpdateInterval = 24 * 60 * 60;
        bool fCurrentlyOnline = (GetAdjustedTime() - (int64_t)addr->nTime < 24 * 60 * 60);
        if (fCurrentlyOnline)
            nUpdateInterval = 60 * 60;

        if (addr->nTime && (!pinfo->addr.nTime ||
            (int64_t)pinfo->addr.nTime < (int64_t)addr->nTime - nUpdateInterval - time_penalty)) {
            int64_t t = (int64_t)addr->nTime - time_penalty;
            pinfo->addr.nTime = (uint32_t)(t > 0 ? t : 0);
        }

        pinfo->addr.nServices |= addr->nServices;

        if (!addr->nTime || (pinfo->addr.nTime && addr->nTime <= pinfo->addr.nTime)) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
        if (pinfo->in_tried) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
        if (pinfo->ref_count == ADDRMAN_NEW_BUCKETS_PER_ADDRESS) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }

        int nFactor = 1;
        for (int n = 0; n < pinfo->ref_count; n++)
            nFactor *= 2;
        if (nFactor > 1 && (GetRandInt(nFactor) != 0)) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
    } else {
        pinfo = create_entry(am, addr, source, &nId);
        if (!pinfo) {
            zcl_mutex_unlock(&am->cs);
            return false;
        }
        int64_t t = (int64_t)pinfo->addr.nTime - time_penalty;
        pinfo->addr.nTime = (uint32_t)(t > 0 ? t : 0);
        am->new_count++;
        fNew = true;
    }

    int nUBucket = addr_info_get_new_bucket(pinfo, &am->nKey, source);
    int nUBucketPos = addr_info_get_bucket_position(pinfo, &am->nKey, true,
                                                     nUBucket);
    if (am->vvNew[nUBucket][nUBucketPos] != nId) {
        bool fInsert = am->vvNew[nUBucket][nUBucketPos] == -1;
        if (!fInsert) {
            struct addr_info *existing =
                &am->entries[am->vvNew[nUBucket][nUBucketPos]];
            if (addr_info_is_terrible(existing, GetAdjustedTime()) ||
                (existing->ref_count > 1 && pinfo->ref_count == 0))
                fInsert = true;
        }
        if (fInsert) {
            clear_new(am, nUBucket, nUBucketPos);
            pinfo->ref_count++;
            am->vvNew[nUBucket][nUBucketPos] = nId;
        } else if (pinfo->ref_count == 0) {
            delete_entry(am, nId);
        }
    }

    zcl_mutex_unlock(&am->cs);
    return fNew;
}

void addrman_good(struct addr_man *am, const struct net_service *addr,
                  int64_t nTime)
{
    zcl_mutex_lock(&am->cs);

    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->addr, &nId);
    if (!pinfo || !net_service_eq(&pinfo->addr.svc, addr)) {
        zcl_mutex_unlock(&am->cs);
        return;
    }

    pinfo->last_success = nTime;
    pinfo->last_try = nTime;
    pinfo->attempts = 0;

    if (pinfo->in_tried) {
        zcl_mutex_unlock(&am->cs);
        return;
    }

    int nRnd = GetRandInt(ADDRMAN_NEW_BUCKET_COUNT);
    int nUBucket = -1;
    for (int n = 0; n < ADDRMAN_NEW_BUCKET_COUNT; n++) {
        int nB = (n + nRnd) % ADDRMAN_NEW_BUCKET_COUNT;
        int nBpos = addr_info_get_bucket_position(pinfo, &am->nKey, true, nB);
        if (am->vvNew[nB][nBpos] == nId) {
            nUBucket = nB;
            break;
        }
    }

    if (nUBucket == -1) {
        zcl_mutex_unlock(&am->cs);
        return;
    }

    make_tried(am, pinfo, nId);
    zcl_mutex_unlock(&am->cs);
}

void addrman_attempt(struct addr_man *am, const struct net_service *addr,
                     int64_t nTime)
{
    zcl_mutex_lock(&am->cs);
    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->addr, &nId);
    if (!pinfo || !net_service_eq(&pinfo->addr.svc, addr)) {
        zcl_mutex_unlock(&am->cs);
        return;
    }
    pinfo->last_try = nTime;
    pinfo->attempts++;
    zcl_mutex_unlock(&am->cs);
}

bool addrman_select(struct addr_man *am, bool new_only,
                    struct addr_info *result)
{
    zcl_mutex_lock(&am->cs);

    if (am->random_size == 0) {
        zcl_mutex_unlock(&am->cs);
        return false;
    }
    if (new_only && am->new_count == 0) {
        zcl_mutex_unlock(&am->cs);
        return false;
    }

    int64_t nNow = GetAdjustedTime();

    if (!new_only && am->tried_count > 0 &&
        (am->new_count == 0 || GetRandInt(2) == 0)) {
        double fChanceFactor = 1.0;
        for (int i = 0; i < 200000; i++) {
            int nKBucket = GetRandInt(ADDRMAN_TRIED_BUCKET_COUNT);
            int nKBucketPos = GetRandInt(ADDRMAN_BUCKET_SIZE);
            while (am->vvTried[nKBucket][nKBucketPos] == -1) {
                nKBucket = (nKBucket + GetRandInt(ADDRMAN_TRIED_BUCKET_COUNT)) %
                           ADDRMAN_TRIED_BUCKET_COUNT;
                nKBucketPos = (nKBucketPos + GetRandInt(ADDRMAN_BUCKET_SIZE)) %
                              ADDRMAN_BUCKET_SIZE;
                if (++i >= 200000) {
                    zcl_mutex_unlock(&am->cs);
                    return false;
                }
            }
            int nId = am->vvTried[nKBucket][nKBucketPos];
            if (nId < 0 || (size_t)nId >= am->entries_cap) {
                am->vvTried[nKBucket][nKBucketPos] = -1; /* repair */
                fChanceFactor *= 1.2;
                continue;
            }
            struct addr_info *info = &am->entries[nId];
            double chance = fChanceFactor * addr_info_get_chance(info, nNow);
            if (GetRandInt(1 << 30) < chance * (double)(1 << 30)) {
                *result = *info;
                zcl_mutex_unlock(&am->cs);
                return true;
            }
            fChanceFactor *= 1.2;
        }
    } else {
        double fChanceFactor = 1.0;
        for (int i = 0; i < 200000; i++) {
            int nUBucket = GetRandInt(ADDRMAN_NEW_BUCKET_COUNT);
            int nUBucketPos = GetRandInt(ADDRMAN_BUCKET_SIZE);
            while (am->vvNew[nUBucket][nUBucketPos] == -1) {
                nUBucket = (nUBucket + GetRandInt(ADDRMAN_NEW_BUCKET_COUNT)) %
                           ADDRMAN_NEW_BUCKET_COUNT;
                nUBucketPos = (nUBucketPos + GetRandInt(ADDRMAN_BUCKET_SIZE)) %
                              ADDRMAN_BUCKET_SIZE;
                if (++i >= 200000) {
                    zcl_mutex_unlock(&am->cs);
                    return false;
                }
            }
            int nId = am->vvNew[nUBucket][nUBucketPos];
            if (nId < 0 || (size_t)nId >= am->entries_cap) {
                am->vvNew[nUBucket][nUBucketPos] = -1; /* repair */
                fChanceFactor *= 1.2;
                continue;
            }
            struct addr_info *info = &am->entries[nId];
            double chance = fChanceFactor * addr_info_get_chance(info, nNow);
            if (GetRandInt(1 << 30) < chance * (double)(1 << 30)) {
                *result = *info;
                zcl_mutex_unlock(&am->cs);
                return true;
            }
            fChanceFactor *= 1.2;
        }
    }

    zcl_mutex_unlock(&am->cs);
    return false;
}

void addrman_connected(struct addr_man *am, const struct net_service *addr,
                       int64_t nTime)
{
    zcl_mutex_lock(&am->cs);
    int nId;
    struct addr_info *pinfo = find_addr(am, &addr->addr, &nId);
    if (!pinfo || !net_service_eq(&pinfo->addr.svc, addr)) {
        zcl_mutex_unlock(&am->cs);
        return;
    }
    int64_t nUpdateInterval = 20 * 60;
    if (nTime - (int64_t)pinfo->addr.nTime > nUpdateInterval)
        pinfo->addr.nTime = (uint32_t)nTime;
    zcl_mutex_unlock(&am->cs);
}

size_t addrman_get_addr(struct addr_man *am, struct net_address *out,
                        size_t max_out)
{
    zcl_mutex_lock(&am->cs);
    size_t nNodes = ADDRMAN_GETADDR_MAX_PCT * am->random_size / 100;
    if (nNodes > ADDRMAN_GETADDR_MAX) nNodes = ADDRMAN_GETADDR_MAX;
    if (nNodes > max_out) nNodes = max_out;

    size_t count = 0;
    int64_t nNow = GetAdjustedTime();
    for (size_t n = 0; n < am->random_size && count < nNodes; n++) {
        int nRndPos = GetRandInt((int)(am->random_size - n)) + (int)n;
        swap_random(am, (unsigned int)n, (unsigned int)nRndPos);
        struct addr_info *ai = &am->entries[am->random_order[n]];
        if (!addr_info_is_terrible(ai, nNow))
            out[count++] = ai->addr;
    }

    zcl_mutex_unlock(&am->cs);
    return count;
}

bool addrman_serialize(const struct addr_man *am, struct byte_stream *s)
{
    if (!stream_write_u8(s, 1)) return false;
    if (!stream_write_u8(s, 32)) return false;
    if (!stream_write_bytes(s, am->nKey.data, 32)) return false;
    if (!stream_write_i32_le(s, am->new_count)) return false;
    if (!stream_write_i32_le(s, am->tried_count)) return false;

    int nUBuckets = ADDRMAN_NEW_BUCKET_COUNT ^ (1 << 30);
    if (!stream_write_i32_le(s, nUBuckets)) return false;

    int *mapUnkIds = calloc((size_t)am->id_count > 0 ? (size_t)am->id_count : 1, sizeof(int));
    if (!mapUnkIds) return false;

    int nIds = 0;
    for (int i = 0; i < am->id_count; i++) {
        if (am->entries[i].used && am->entries[i].ref_count > 0) {
            mapUnkIds[i] = nIds;
            if (!stream_write_bytes(s, am->entries[i].addr.svc.addr.ip, 16)) { free(mapUnkIds); return false; }
            if (!stream_write_u16_le(s, am->entries[i].addr.svc.port)) { free(mapUnkIds); return false; }
            if (!stream_write_u64_le(s, am->entries[i].addr.nServices)) { free(mapUnkIds); return false; }
            if (!stream_write_u32_le(s, am->entries[i].addr.nTime)) { free(mapUnkIds); return false; }
            if (!stream_write_bytes(s, am->entries[i].source.ip, 16)) { free(mapUnkIds); return false; }
            if (!stream_write_i64_le(s, am->entries[i].last_success)) { free(mapUnkIds); return false; }
            if (!stream_write_i32_le(s, am->entries[i].attempts)) { free(mapUnkIds); return false; }
            nIds++;
        }
    }
    for (int i = 0; i < am->id_count; i++) {
        if (am->entries[i].used && am->entries[i].in_tried) {
            if (!stream_write_bytes(s, am->entries[i].addr.svc.addr.ip, 16)) { free(mapUnkIds); return false; }
            if (!stream_write_u16_le(s, am->entries[i].addr.svc.port)) { free(mapUnkIds); return false; }
            if (!stream_write_u64_le(s, am->entries[i].addr.nServices)) { free(mapUnkIds); return false; }
            if (!stream_write_u32_le(s, am->entries[i].addr.nTime)) { free(mapUnkIds); return false; }
            if (!stream_write_bytes(s, am->entries[i].source.ip, 16)) { free(mapUnkIds); return false; }
            if (!stream_write_i64_le(s, am->entries[i].last_success)) { free(mapUnkIds); return false; }
            if (!stream_write_i32_le(s, am->entries[i].attempts)) { free(mapUnkIds); return false; }
        }
    }

    for (int bucket = 0; bucket < ADDRMAN_NEW_BUCKET_COUNT; bucket++) {
        int nSize = 0;
        for (int i = 0; i < ADDRMAN_BUCKET_SIZE; i++)
            if (am->vvNew[bucket][i] != -1) nSize++;
        if (!stream_write_i32_le(s, nSize)) { free(mapUnkIds); return false; }
        for (int i = 0; i < ADDRMAN_BUCKET_SIZE; i++) {
            if (am->vvNew[bucket][i] != -1) {
                int nIndex = mapUnkIds[am->vvNew[bucket][i]];
                if (!stream_write_i32_le(s, nIndex)) { free(mapUnkIds); return false; }
            }
        }
    }

    free(mapUnkIds);
    return true;
}

bool addrman_deserialize(struct addr_man *am, struct byte_stream *s)
{
    addrman_clear(am);

    uint8_t nVersion;
    if (!stream_read_u8(s, &nVersion)) return false;
    uint8_t nKeySize;
    if (!stream_read_u8(s, &nKeySize)) return false;
    if (nKeySize != 32) return false;
    if (!stream_read_bytes(s, am->nKey.data, 32)) return false;

    int32_t nNew, nTried;
    if (!stream_read_i32_le(s, &nNew)) return false;
    if (!stream_read_i32_le(s, &nTried)) return false;

    int32_t nUBuckets;
    if (!stream_read_i32_le(s, &nUBuckets)) return false;
    if (nVersion != 0) nUBuckets ^= (1 << 30);

    if (nNew > ADDRMAN_NEW_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE) return false;
    if (nTried > ADDRMAN_TRIED_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE) return false;

    size_t need = (size_t)(nNew + nTried);
    if (need > am->entries_cap) {
        struct addr_info *p = realloc(am->entries,
                                       need * sizeof(struct addr_info));
        if (!p) return false;
        memset(p + am->entries_cap, 0,
               (need - am->entries_cap) * sizeof(struct addr_info));
        am->entries = p;
        am->entries_cap = need;
    }

    for (int n = 0; n < nNew; n++) {
        struct addr_info *info = &am->entries[n];
        memset(info, 0, sizeof(*info));
        info->used = true;

        if (!stream_read_bytes(s, info->addr.svc.addr.ip, 16)) return false;
        if (!stream_read_u16_le(s, &info->addr.svc.port)) return false;
        if (!stream_read_u64_le(s, &info->addr.nServices)) return false;
        if (!stream_read_u32_le(s, &info->addr.nTime)) return false;
        if (!stream_read_bytes(s, info->source.ip, 16)) return false;
        if (!stream_read_i64_le(s, &info->last_success)) return false;
        int32_t attempts;
        if (!stream_read_i32_le(s, &attempts)) return false;
        info->attempts = attempts;

        info->random_pos = (int)am->random_size;
        random_push(am, n);

        if (nVersion != 1 || nUBuckets != ADDRMAN_NEW_BUCKET_COUNT) {
            int nUBucket = addr_info_get_new_bucket(info, &am->nKey,
                                                     &info->source);
            int nUBucketPos = addr_info_get_bucket_position(info, &am->nKey,
                                                             true, nUBucket);
            if (am->vvNew[nUBucket][nUBucketPos] == -1) {
                am->vvNew[nUBucket][nUBucketPos] = n;
                info->ref_count++;
            }
        }
    }
    am->id_count = nNew;
    am->new_count = nNew;

    int nLost = 0;
    for (int n = 0; n < nTried; n++) {
        struct addr_info info;
        memset(&info, 0, sizeof(info));
        info.used = true;

        if (!stream_read_bytes(s, info.addr.svc.addr.ip, 16)) return false;
        if (!stream_read_u16_le(s, &info.addr.svc.port)) return false;
        if (!stream_read_u64_le(s, &info.addr.nServices)) return false;
        if (!stream_read_u32_le(s, &info.addr.nTime)) return false;
        if (!stream_read_bytes(s, info.source.ip, 16)) return false;
        if (!stream_read_i64_le(s, &info.last_success)) return false;
        int32_t attempts;
        if (!stream_read_i32_le(s, &attempts)) return false;
        info.attempts = attempts;

        int nKBucket = addr_info_get_tried_bucket(&info, &am->nKey);
        int nKBucketPos = addr_info_get_bucket_position(&info, &am->nKey,
                                                         false, nKBucket);
        if (am->vvTried[nKBucket][nKBucketPos] == -1) {
            info.random_pos = (int)am->random_size;
            info.in_tried = true;
            int id = am->id_count++;
            am->entries[id] = info;
            random_push(am, id);
            am->vvTried[nKBucket][nKBucketPos] = id;
        } else {
            nLost++;
        }
    }
    am->tried_count = nTried - nLost;

    for (int bucket = 0; bucket < nUBuckets; bucket++) {
        int32_t nSize;
        if (!stream_read_i32_le(s, &nSize)) return false;
        for (int n = 0; n < nSize; n++) {
            int32_t nIndex;
            if (!stream_read_i32_le(s, &nIndex)) return false;
            if (nIndex >= 0 && nIndex < nNew && bucket < ADDRMAN_NEW_BUCKET_COUNT) {
                struct addr_info *info = &am->entries[nIndex];
                int nUBucketPos = addr_info_get_bucket_position(
                    info, &am->nKey, true, bucket);
                if (nVersion == 1 && nUBuckets == ADDRMAN_NEW_BUCKET_COUNT &&
                    am->vvNew[bucket][nUBucketPos] == -1 &&
                    info->ref_count < ADDRMAN_NEW_BUCKETS_PER_ADDRESS) {
                    info->ref_count++;
                    am->vvNew[bucket][nUBucketPos] = nIndex;
                }
            }
        }
    }

    return true;
}
