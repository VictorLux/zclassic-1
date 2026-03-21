/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block download manager — coordinates parallel block downloads.
 * Lock-free reads where possible, mutex for writes. */

#include "net/download.h"
#include "event/event.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define INITIAL_SLOTS 2048
#define INITIAL_QUEUE 4096

/* FNV-1a hash for uint256 → slot index */
static size_t hash_slot(const struct uint256 *h, size_t mask)
{
    uint64_t fnv = 14695981039346656037ULL;
    for (int i = 0; i < 32; i++) {
        fnv ^= h->data[i];
        fnv *= 1099511628211ULL;
    }
    return (size_t)(fnv & mask);
}

void dl_init(struct download_manager *dm)
{
    memset(dm, 0, sizeof(*dm));
    zcl_mutex_init(&dm->cs);
    dm->num_slots = INITIAL_SLOTS;
    dm->slots = calloc(dm->num_slots, sizeof(struct dl_in_flight));
    dm->queue_cap = INITIAL_QUEUE;
    dm->queue = malloc(dm->queue_cap * sizeof(struct uint256));
    dm->queue_heights = malloc(dm->queue_cap * sizeof(int32_t));
    if (!dm->slots || !dm->queue || !dm->queue_heights) {
        free(dm->slots); free(dm->queue); free(dm->queue_heights);
        dm->slots = NULL; dm->queue = NULL; dm->queue_heights = NULL;
        dm->num_slots = 0; dm->queue_cap = 0;
    }
}

void dl_free(struct download_manager *dm)
{
    free(dm->slots);
    free(dm->queue);
    free(dm->queue_heights);
    dm->slots = NULL;
    dm->queue = NULL;
    dm->queue_heights = NULL;
}

/* Expand queue capacity. Returns true on success. Caller holds mutex. */
static bool dl_queue_grow(struct download_manager *dm)
{
    if (dm->queue_cap >= 65536) return false;
    size_t new_cap = dm->queue_cap * 2;
    struct uint256 *nq = realloc(dm->queue, new_cap * sizeof(struct uint256));
    int32_t *nh = realloc(dm->queue_heights, new_cap * sizeof(int32_t));
    if (!nq || !nh) {
        /* If one succeeded, keep the old pointer valid */
        if (nq) dm->queue = nq;
        if (nh) dm->queue_heights = nh;
        return false;
    }
    dm->queue = nq;
    dm->queue_heights = nh;
    dm->queue_cap = new_cap;
    return true;
}

/* Append a block to the download queue. Caller holds mutex. */
static bool dl_queue_push(struct download_manager *dm,
                           const struct uint256 *hash, int32_t height)
{
    if (dm->queue_len >= dm->queue_cap && !dl_queue_grow(dm))
        return false;
    dm->queue[dm->queue_len] = *hash;
    dm->queue_heights[dm->queue_len] = height;
    dm->queue_len++;
    return true;
}

/* Find or update peer stats. Caller holds mutex. */
static struct dl_peer_stats *dl_find_peer(struct download_manager *dm,
                                           uint32_t peer_id, bool create)
{
    for (size_t i = 0; i < dm->num_peers; i++) {
        if (dm->peers[i].peer_id == peer_id)
            return &dm->peers[i];
    }
    if (!create || dm->num_peers >= 256)
        return NULL;
    struct dl_peer_stats *p = &dm->peers[dm->num_peers++];
    memset(p, 0, sizeof(*p));
    p->peer_id = peer_id;
    p->active = true;
    return p;
}

/* Find slot for hash (open addressing with linear probe).
 * Handles gaps from deletions: inactive slots are NOT probe-chain
 * terminators because dl_mark_received clears slots without
 * rehashing. We must scan through inactive slots to find matches. */
static struct dl_in_flight *find_slot(struct download_manager *dm,
                                       const struct uint256 *hash,
                                       bool find_empty)
{
    if (!dm->slots || dm->num_slots == 0) return NULL;
    size_t mask = dm->num_slots - 1;
    size_t idx = hash_slot(hash, mask);
    struct dl_in_flight *first_empty = NULL;

    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[(idx + i) & mask];
        if (!s->active) {
            if (!first_empty) first_empty = s;
            /* Check if this slot was NEVER used (zero hash = virgin) */
            if (uint256_is_null(&s->hash))
                break; /* end of probe chain */
            continue; /* skip gap from deletion, keep probing */
        }
        if (uint256_eq(&s->hash, hash))
            return s;
    }
    return find_empty ? first_empty : NULL;
}

/* Rehash into a table of given size (must be power of 2). */
static void dl_rehash(struct download_manager *dm, size_t new_size)
{
    struct dl_in_flight *new_slots = calloc(new_size, sizeof(struct dl_in_flight));
    if (!new_slots) return;

    size_t new_mask = new_size - 1;
    for (size_t i = 0; i < dm->num_slots; i++) {
        if (!dm->slots[i].active) continue;
        size_t idx = hash_slot(&dm->slots[i].hash, new_mask);
        for (size_t j = 0; j < new_size; j++) {
            struct dl_in_flight *s = &new_slots[(idx + j) & new_mask];
            if (!s->active) {
                *s = dm->slots[i];
                break;
            }
        }
    }
    free(dm->slots);
    dm->slots = new_slots;
    dm->num_slots = new_size;
}

/* Grow or compact hash table.
 * Grows when load factor > 50%.
 * Compacts (rehash in place) when active entries < 25% of slots
 * and total insertions have left many dead gaps. */
static void maybe_grow(struct download_manager *dm)
{
    if (dm->num_active * 2 >= dm->num_slots) {
        dl_rehash(dm, dm->num_slots * 2);
    } else if (dm->num_slots > INITIAL_SLOTS &&
               dm->num_active * 4 < dm->num_slots) {
        /* Compact: rehash at same size to eliminate dead gaps */
        dl_rehash(dm, dm->num_slots);
    }
}

bool dl_is_in_flight(struct download_manager *dm, const struct uint256 *hash)
{
    zcl_mutex_lock(&dm->cs);
    struct dl_in_flight *s = find_slot(dm, hash, false);
    bool found = (s != NULL && s->active);
    zcl_mutex_unlock(&dm->cs);
    return found;
}

bool dl_mark_requested(struct download_manager *dm,
                       const struct uint256 *hash, int32_t height,
                       uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);

    /* Check if already in-flight */
    struct dl_in_flight *existing = find_slot(dm, hash, false);
    if (existing && existing->active) {
        dm->total_duplicate++;
        zcl_mutex_unlock(&dm->cs);
        return false;
    }

    /* Check global limit */
    if (dm->num_active >= DL_MAX_IN_FLIGHT_TOTAL) {
        zcl_mutex_unlock(&dm->cs);
        return false;
    }

    maybe_grow(dm);

    /* Find empty slot */
    struct dl_in_flight *slot = find_slot(dm, hash, true);
    if (!slot) {
        /* Table full despite grow — shouldn't happen */
        zcl_mutex_unlock(&dm->cs);
        return false;
    }

    slot->hash = *hash;
    slot->height = height;
    slot->peer_id = peer_id;
    slot->request_time = (int64_t)time(NULL);
    slot->active = true;
    dm->num_active++;
    dm->total_requested++;

    /* Update peer stats */
    {
        struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, true);
        if (ps) ps->blocks_requested++;
        dm->num_peers++;
    }

    zcl_mutex_unlock(&dm->cs);
    return true;
}

uint32_t dl_mark_received(struct download_manager *dm,
                          const struct uint256 *hash)
{
    zcl_mutex_lock(&dm->cs);

    struct dl_in_flight *s = find_slot(dm, hash, false);
    if (!s || !s->active) {
        zcl_mutex_unlock(&dm->cs);
        return 0;
    }

    uint32_t peer_id = s->peer_id;
    int64_t delivery = (int64_t)time(NULL) - s->request_time;

    s->active = false;
    /* Don't zero the hash — find_slot needs it to detect "was used" vs "never used"
     * for proper probe chain handling after deletions. */
    dm->num_active--;
    dm->total_received++;

    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    if (ps) {
        ps->blocks_received++;
        ps->last_block_time = (int64_t)time(NULL);
        int64_t delivery_us = delivery * 1000000;
        if (ps->avg_delivery_us == 0)
            ps->avg_delivery_us = delivery_us;
        else
            ps->avg_delivery_us = (ps->avg_delivery_us * 7 + delivery_us) / 8;
    }

    zcl_mutex_unlock(&dm->cs);
    return peer_id;
}

size_t dl_check_timeouts(struct download_manager *dm, int64_t now)
{
    zcl_mutex_lock(&dm->cs);

    size_t reassigned = 0;
    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[i];
        if (!s->active) continue;

        int64_t age = now - s->request_time;
        if (age < DL_REQUEST_TIMEOUT_SECS) continue;

        /* Timed out — move back to queue for reassignment */
        event_emitf(EV_BLOCK_REQUESTED, s->peer_id,
                    "TIMEOUT h=%d age=%llds", s->height, (long long)age);

        struct dl_peer_stats *ps = dl_find_peer(dm, s->peer_id, false);
        if (ps) ps->blocks_timed_out++;

        dl_queue_push(dm, &s->hash, s->height);
        s->active = false;
        dm->num_active--;
        dm->total_timed_out++;
        reassigned++;
    }

    /* Compact hash table if it has many dead gaps */
    maybe_grow(dm);

    zcl_mutex_unlock(&dm->cs);
    return reassigned;
}

size_t dl_peer_in_flight(struct download_manager *dm, uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);
    size_t count = 0;
    for (size_t i = 0; i < dm->num_slots; i++) {
        if (dm->slots[i].active && dm->slots[i].peer_id == peer_id)
            count++;
    }
    zcl_mutex_unlock(&dm->cs);
    return count;
}

size_t dl_peer_disconnected(struct download_manager *dm, uint32_t peer_id)
{
    zcl_mutex_lock(&dm->cs);
    size_t requeued = 0;

    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[i];
        if (!s->active || s->peer_id != peer_id) continue;

        dl_queue_push(dm, &s->hash, s->height);
        s->active = false;
        dm->num_active--;
        requeued++;
    }

    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    if (ps) ps->active = false;

    if (requeued > 0)
        event_emitf(EV_BLOCK_REQUESTED, peer_id,
                    "peer disconnect: %zu blocks requeued", requeued);

    zcl_mutex_unlock(&dm->cs);
    return requeued;
}

size_t dl_queue_blocks(struct download_manager *dm,
                       const struct uint256 *hashes,
                       const int32_t *heights,
                       size_t count)
{
    zcl_mutex_lock(&dm->cs);

    size_t added = 0;
    for (size_t i = 0; i < count; i++) {
        /* Skip if already in-flight */
        struct dl_in_flight *s = find_slot(dm, &hashes[i], false);
        if (s && s->active) continue;

        /* Skip if already in queue (linear scan — acceptable for queue < 65K) */
        bool dup = false;
        for (size_t j = 0; j < dm->queue_len; j++) {
            if (uint256_eq(&dm->queue[j], &hashes[i])) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (dl_queue_push(dm, &hashes[i], heights ? heights[i] : -1))
            added++;
    }

    zcl_mutex_unlock(&dm->cs);
    return added;
}

size_t dl_assign_to_peer(struct download_manager *dm,
                         uint32_t peer_id,
                         struct uint256 *out_hashes,
                         size_t max_assign)
{
    zcl_mutex_lock(&dm->cs);

    /* Check per-peer limit */
    size_t peer_count = 0;
    for (size_t i = 0; i < dm->num_slots; i++) {
        if (dm->slots[i].active && dm->slots[i].peer_id == peer_id)
            peer_count++;
    }
    size_t available = 0;
    if (peer_count < DL_MAX_IN_FLIGHT_PER_PEER)
        available = DL_MAX_IN_FLIGHT_PER_PEER - peer_count;
    if (available > max_assign) available = max_assign;
    if (available > dm->queue_len) available = dm->queue_len;

    /* Also respect global limit */
    if (dm->num_active + available > DL_MAX_IN_FLIGHT_TOTAL)
        available = DL_MAX_IN_FLIGHT_TOTAL - dm->num_active;

    /* Pop from front — batch the memmove after the loop (O(1) per pop) */
    size_t pop_count = 0;
    size_t assigned = 0;
    while (assigned < available && pop_count < dm->queue_len) {
        struct uint256 hash = dm->queue[pop_count];
        int32_t height = dm->queue_heights[pop_count];
        pop_count++;

        maybe_grow(dm);
        struct dl_in_flight *slot = find_slot(dm, &hash, true);
        if (!slot) continue;

        slot->hash = hash;
        slot->height = height;
        slot->peer_id = peer_id;
        slot->request_time = (int64_t)time(NULL);
        slot->active = true;
        dm->num_active++;
        dm->total_requested++;

        out_hashes[assigned++] = hash;
    }

    /* Batch shift: remove all popped entries in one memmove */
    if (pop_count > 0) {
        dm->queue_len -= pop_count;
        if (dm->queue_len > 0) {
            memmove(&dm->queue[0], &dm->queue[pop_count],
                    dm->queue_len * sizeof(struct uint256));
            memmove(&dm->queue_heights[0], &dm->queue_heights[pop_count],
                    dm->queue_len * sizeof(int32_t));
        }
    }

    if (assigned > 0) {
        struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, true);
        if (ps) ps->blocks_requested += (uint32_t)assigned;
    }

    zcl_mutex_unlock(&dm->cs);
    return assigned;
}

void dl_peer_block_received(struct download_manager *dm,
                            uint32_t peer_id, int64_t delivery_us)
{
    zcl_mutex_lock(&dm->cs);
    struct dl_peer_stats *ps = dl_find_peer(dm, peer_id, false);
    if (ps) {
        ps->blocks_received++;
        ps->last_block_time = (int64_t)time(NULL);
        if (ps->avg_delivery_us == 0)
            ps->avg_delivery_us = delivery_us;
        else
            ps->avg_delivery_us = (ps->avg_delivery_us * 7 + delivery_us) / 8;
    }
    zcl_mutex_unlock(&dm->cs);
}

void dl_get_stats(struct download_manager *dm,
                  uint64_t *requested, uint64_t *received,
                  uint64_t *timed_out, uint64_t *in_flight,
                  uint64_t *queued)
{
    zcl_mutex_lock(&dm->cs);
    if (requested)  *requested  = dm->total_requested;
    if (received)   *received   = dm->total_received;
    if (timed_out)  *timed_out  = dm->total_timed_out;
    if (in_flight)  *in_flight  = dm->num_active;
    if (queued)     *queued     = dm->queue_len;
    zcl_mutex_unlock(&dm->cs);
}
