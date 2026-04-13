/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Dandelion++ transaction propagation — stem/fluff relay for tx origin
 * privacy. See dandelion.h for protocol overview.
 *
 * Thread safety: all public functions acquire ds->cs. The caller must
 * NOT hold ds->cs when calling these functions. The caller MAY hold
 * net_manager->cs_nodes when calling dandelion_maybe_rotate_epoch()
 * (it acquires cs_nodes internally only if not rotating). */

#include "net/dandelion.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ── Lifecycle ─────────────────────────────────────────────────── */

void dandelion_init(struct dandelion_state *ds)
{
    memset(ds, 0, sizeof(*ds));
    zcl_mutex_init(&ds->cs);
    ds->epoch_start = 0;
    ds->num_stem_peers = 0;
    ds->stempool_count = 0;
    ds->stem_rr_index = 0;
    ds->enabled = true;
    for (int i = 0; i < DANDELION_NUM_STEM_PEERS; i++)
        ds->stem_peers[i] = DANDELION_NODE_ID_NONE;
}

void dandelion_free(struct dandelion_state *ds)
{
    zcl_mutex_destroy(&ds->cs);
    memset(ds, 0, sizeof(*ds));
}

/* ── Simple PRNG (xorshift64) for stem decisions ───────────────── */

static uint64_t s_dandelion_rng_state = 0;

static uint64_t dandelion_rand(void)
{
    if (s_dandelion_rng_state == 0)
        s_dandelion_rng_state = (uint64_t)time(NULL) ^ 0xdeadbeefcafe1234ULL;
    uint64_t x = s_dandelion_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s_dandelion_rng_state = x;
    return x;
}

/* ── Epoch management ──────────────────────────────────────────── */

void dandelion_maybe_rotate_epoch(struct dandelion_state *ds,
                                  struct net_manager *nm)
{
    if (!ds || !nm)
        return;

    int64_t now = (int64_t)time(NULL);

    zcl_mutex_lock(&ds->cs);

    if (ds->epoch_start != 0 &&
        (now - ds->epoch_start) < DANDELION_EPOCH_SECS) {
        zcl_mutex_unlock(&ds->cs);
        return;
    }

    /* New epoch — pick stem relay peers from connected outbound peers */
    ds->epoch_start = now;
    ds->num_stem_peers = 0;
    for (int i = 0; i < DANDELION_NUM_STEM_PEERS; i++)
        ds->stem_peers[i] = DANDELION_NODE_ID_NONE;

    /* Collect eligible peer IDs (outbound, handshake complete, relays tx) */
    node_id_t candidates[MAX_OUTBOUND_CONNECTIONS];
    int num_candidates = 0;

    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes && num_candidates < MAX_OUTBOUND_CONNECTIONS; i++) {
        struct p2p_node *peer = nm->nodes[i];
        if (!peer->inbound &&
            peer->state >= PEER_HANDSHAKE_COMPLETE &&
            !peer->disconnect &&
            peer->relay_txes) {
            candidates[num_candidates++] = peer->id;
        }
    }
    zcl_mutex_unlock(&nm->cs_nodes);

    /* Fisher-Yates shuffle and pick first DANDELION_NUM_STEM_PEERS */
    for (int i = num_candidates - 1; i > 0; i--) {
        int j = (int)(dandelion_rand() % (uint64_t)(i + 1));
        node_id_t tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
    }

    int pick = num_candidates < DANDELION_NUM_STEM_PEERS
             ? num_candidates : DANDELION_NUM_STEM_PEERS;
    for (int i = 0; i < pick; i++)
        ds->stem_peers[i] = candidates[i];
    ds->num_stem_peers = pick;
    ds->stem_rr_index = 0;

    if (pick > 0) {
        fprintf(stderr, "[dandelion] new epoch: %d stem peer(s) selected\n", pick);
    }

    zcl_mutex_unlock(&ds->cs);
}

/* ── Core routing ──────────────────────────────────────────────── */

bool dandelion_should_stem(struct dandelion_state *ds, node_id_t from_peer)
{
    (void)from_peer;

    if (!ds)
        return false;

    zcl_mutex_lock(&ds->cs);

    if (!ds->enabled || ds->num_stem_peers == 0) {
        zcl_mutex_unlock(&ds->cs);
        return false;
    }

    /* Each hop independently decides to fluff with DANDELION_FLUFF_PROB% */
    uint64_t r = dandelion_rand() % 100;
    bool stem = (r >= DANDELION_FLUFF_PROB);

    zcl_mutex_unlock(&ds->cs);
    return stem;
}

node_id_t dandelion_get_stem_peer(struct dandelion_state *ds,
                                  node_id_t from_peer)
{
    if (!ds)
        return DANDELION_NODE_ID_NONE;

    zcl_mutex_lock(&ds->cs);

    if (ds->num_stem_peers == 0) {
        zcl_mutex_unlock(&ds->cs);
        return DANDELION_NODE_ID_NONE;
    }

    /* Round-robin among stem peers, skipping from_peer */
    node_id_t chosen = DANDELION_NODE_ID_NONE;
    for (int attempt = 0; attempt < ds->num_stem_peers; attempt++) {
        int idx = (ds->stem_rr_index + attempt) % ds->num_stem_peers;
        if (ds->stem_peers[idx] != from_peer) {
            chosen = ds->stem_peers[idx];
            ds->stem_rr_index = (idx + 1) % ds->num_stem_peers;
            break;
        }
    }

    zcl_mutex_unlock(&ds->cs);
    return chosen;
}

/* ── Stem pool (embargo queue) ─────────────────────────────────── */

void dandelion_stempool_add(struct dandelion_state *ds,
                            const struct uint256 *txhash,
                            node_id_t from_peer)
{
    if (!ds || !txhash)
        return;

    zcl_mutex_lock(&ds->cs);

    /* Check for duplicate */
    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (ds->stempool[i].active &&
            uint256_eq(&ds->stempool[i].txhash, txhash)) {
            zcl_mutex_unlock(&ds->cs);
            return;
        }
    }

    /* Find empty slot, or evict oldest */
    int slot = -1;
    int64_t oldest_time = INT64_MAX;
    int oldest_slot = 0;

    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (!ds->stempool[i].active) {
            slot = i;
            break;
        }
        if (ds->stempool[i].embargo_time < oldest_time) {
            oldest_time = ds->stempool[i].embargo_time;
            oldest_slot = i;
        }
    }

    if (slot < 0) {
        /* Evict oldest — it should have been fluffed already */
        slot = oldest_slot;
        ds->stempool_count--;
    }

    ds->stempool[slot].txhash = *txhash;
    ds->stempool[slot].embargo_time = (int64_t)time(NULL) + DANDELION_EMBARGO_SECS;
    ds->stempool[slot].from_peer = from_peer;
    ds->stempool[slot].active = true;
    ds->stempool_count++;

    zcl_mutex_unlock(&ds->cs);
}

bool dandelion_stempool_remove(struct dandelion_state *ds,
                               const struct uint256 *txhash)
{
    if (!ds || !txhash)
        return false;

    zcl_mutex_lock(&ds->cs);

    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (ds->stempool[i].active &&
            uint256_eq(&ds->stempool[i].txhash, txhash)) {
            ds->stempool[i].active = false;
            ds->stempool_count--;
            zcl_mutex_unlock(&ds->cs);
            return true;
        }
    }

    zcl_mutex_unlock(&ds->cs);
    return false;
}

int dandelion_stempool_check_embargo(struct dandelion_state *ds,
                                     struct uint256 *out_hashes,
                                     int max_out)
{
    if (!ds || !out_hashes || max_out <= 0)
        return 0;

    int64_t now = (int64_t)time(NULL);
    int count = 0;

    zcl_mutex_lock(&ds->cs);

    for (int i = 0; i < DANDELION_MAX_STEMPOOL && count < max_out; i++) {
        if (ds->stempool[i].active &&
            now >= ds->stempool[i].embargo_time) {
            out_hashes[count++] = ds->stempool[i].txhash;
            ds->stempool[i].active = false;
            ds->stempool_count--;
            ds->stat_embargo_fluff++;
        }
    }

    zcl_mutex_unlock(&ds->cs);
    return count;
}

bool dandelion_stempool_contains(struct dandelion_state *ds,
                                 const struct uint256 *txhash)
{
    if (!ds || !txhash)
        return false;

    zcl_mutex_lock(&ds->cs);

    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (ds->stempool[i].active &&
            uint256_eq(&ds->stempool[i].txhash, txhash)) {
            zcl_mutex_unlock(&ds->cs);
            return true;
        }
    }

    zcl_mutex_unlock(&ds->cs);
    return false;
}
