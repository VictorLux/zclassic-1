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

/* Find slot for hash (open addressing with linear probe) */
static struct dl_in_flight *find_slot(struct download_manager *dm,
                                       const struct uint256 *hash,
                                       bool find_empty)
{
    size_t mask = dm->num_slots - 1;
    size_t idx = hash_slot(hash, mask);

    for (size_t i = 0; i < dm->num_slots; i++) {
        struct dl_in_flight *s = &dm->slots[(idx + i) & mask];
        if (!s->active) {
            return find_empty ? s : NULL;
        }
        if (uint256_eq(&s->hash, hash))
            return s;
    }
    return NULL;
}

/* Grow hash table when load factor > 50% */
static void maybe_grow(struct download_manager *dm)
{
    if (dm->num_active * 2 < dm->num_slots)
        return;

    size_t new_size = dm->num_slots * 2;
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
    for (size_t i = 0; i < dm->num_peers; i++) {
        if (dm->peers[i].peer_id == peer_id) {
            dm->peers[i].blocks_requested++;
            goto done;
        }
    }
    /* New peer */
    if (dm->num_peers < 256) {
        dm->peers[dm->num_peers].peer_id = peer_id;
        dm->peers[dm->num_peers].blocks_requested = 1;
        dm->peers[dm->num_peers].active = true;
        dm->num_peers++;
    }

done:
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
    memset(&s->hash, 0, sizeof(s->hash));
    dm->num_active--;
    dm->total_received++;

    /* Update peer stats */
    for (size_t i = 0; i < dm->num_peers; i++) {
        if (dm->peers[i].peer_id == peer_id) {
            dm->peers[i].blocks_received++;
            dm->peers[i].last_block_time = (int64_t)time(NULL);
            /* Rolling average delivery time (seconds → microseconds) */
            int64_t delivery_us = delivery * 1000000;
            if (dm->peers[i].avg_delivery_us == 0)
                dm->peers[i].avg_delivery_us = delivery_us;
            else
                dm->peers[i].avg_delivery_us =
                    (dm->peers[i].avg_delivery_us * 7 + delivery_us) / 8;
            break;
        }
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

        /* Update peer timeout stats */
        for (size_t j = 0; j < dm->num_peers; j++) {
            if (dm->peers[j].peer_id == s->peer_id) {
                dm->peers[j].blocks_timed_out++;
                break;
            }
        }

        /* Re-queue the block */
        if (dm->queue_len < dm->queue_cap) {
            dm->queue[dm->queue_len] = s->hash;
            dm->queue_heights[dm->queue_len] = s->height;
            dm->queue_len++;
        } else if (dm->queue_cap < 65536) {
            size_t new_cap = dm->queue_cap * 2;
            struct uint256 *nq = realloc(dm->queue,
                                          new_cap * sizeof(struct uint256));
            int32_t *nh = realloc(dm->queue_heights,
                                   new_cap * sizeof(int32_t));
            if (nq && nh) {
                dm->queue = nq;
                dm->queue_heights = nh;
                dm->queue_cap = new_cap;
                dm->queue[dm->queue_len] = s->hash;
                dm->queue_heights[dm->queue_len] = s->height;
                dm->queue_len++;
            }
        }

        s->active = false;
        memset(&s->hash, 0, sizeof(s->hash));
        dm->num_active--;
        dm->total_timed_out++;
        reassigned++;
    }

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

        /* Re-queue this block for another peer */
        if (dm->queue_len < dm->queue_cap) {
            dm->queue[dm->queue_len] = s->hash;
            dm->queue_heights[dm->queue_len] = s->height;
            dm->queue_len++;
        } else if (dm->queue_cap < 65536) {
            size_t new_cap = dm->queue_cap * 2;
            struct uint256 *nq = realloc(dm->queue,
                                          new_cap * sizeof(struct uint256));
            int32_t *nh = realloc(dm->queue_heights,
                                   new_cap * sizeof(int32_t));
            if (nq && nh) {
                dm->queue = nq;
                dm->queue_heights = nh;
                dm->queue_cap = new_cap;
                dm->queue[dm->queue_len] = s->hash;
                dm->queue_heights[dm->queue_len] = s->height;
                dm->queue_len++;
            }
        }

        s->active = false;
        memset(&s->hash, 0, sizeof(s->hash));
        dm->num_active--;
        requeued++;
    }

    /* Mark peer as inactive */
    for (size_t i = 0; i < dm->num_peers; i++) {
        if (dm->peers[i].peer_id == peer_id) {
            dm->peers[i].active = false;
            break;
        }
    }

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

        /* Skip if already in queue */
        bool dup = false;
        for (size_t j = 0; j < dm->queue_len; j++) {
            if (uint256_eq(&dm->queue[j], &hashes[i])) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        /* Grow queue if needed */
        if (dm->queue_len >= dm->queue_cap) {
            size_t new_cap = dm->queue_cap * 2;
            if (new_cap > 65536) new_cap = 65536;
            if (new_cap <= dm->queue_cap) continue; /* at limit */
            struct uint256 *nq = realloc(dm->queue,
                                          new_cap * sizeof(struct uint256));
            int32_t *nh = realloc(dm->queue_heights,
                                   new_cap * sizeof(int32_t));
            if (!nq || !nh) continue;
            dm->queue = nq;
            dm->queue_heights = nh;
            dm->queue_cap = new_cap;
        }

        dm->queue[dm->queue_len] = hashes[i];
        dm->queue_heights[dm->queue_len] = heights ? heights[i] : -1;
        dm->queue_len++;
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

    size_t assigned = 0;
    for (size_t i = 0; i < available && dm->queue_len > 0; i++) {
        struct uint256 hash = dm->queue[0];
        int32_t height = dm->queue_heights[0];

        /* Pop from front of queue */
        dm->queue_len--;
        if (dm->queue_len > 0) {
            memmove(&dm->queue[0], &dm->queue[1],
                    dm->queue_len * sizeof(struct uint256));
            memmove(&dm->queue_heights[0], &dm->queue_heights[1],
                    dm->queue_len * sizeof(int32_t));
        }

        /* Mark as in-flight */
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

    /* Update peer stats */
    if (assigned > 0) {
        for (size_t i = 0; i < dm->num_peers; i++) {
            if (dm->peers[i].peer_id == peer_id) {
                dm->peers[i].blocks_requested += (uint32_t)assigned;
                goto done;
            }
        }
        if (dm->num_peers < 256) {
            dm->peers[dm->num_peers].peer_id = peer_id;
            dm->peers[dm->num_peers].blocks_requested = (uint32_t)assigned;
            dm->peers[dm->num_peers].active = true;
            dm->num_peers++;
        }
    }

done:
    zcl_mutex_unlock(&dm->cs);
    return assigned;
}

void dl_peer_block_received(struct download_manager *dm,
                            uint32_t peer_id, int64_t delivery_us)
{
    zcl_mutex_lock(&dm->cs);
    for (size_t i = 0; i < dm->num_peers; i++) {
        if (dm->peers[i].peer_id == peer_id) {
            dm->peers[i].blocks_received++;
            dm->peers[i].last_block_time = (int64_t)time(NULL);
            if (dm->peers[i].avg_delivery_us == 0)
                dm->peers[i].avg_delivery_us = delivery_us;
            else
                dm->peers[i].avg_delivery_us =
                    (dm->peers[i].avg_delivery_us * 7 + delivery_us) / 8;
            break;
        }
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
