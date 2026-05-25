/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "sim/sim_peer.h"

#include <errno.h>
#include <stddef.h>

void sim_peer_set_init(struct sim_peer_set *set)
{
    if (!set) return;
    set->count = 0;
    set->active_count = 0;
    set->killed_count = 0;
}

int sim_peer_set_resize(struct sim_peer_set *set, unsigned count)
{
    if (!set) return -EINVAL;
    if (count > SIM_PEER_MAX) return -ERANGE;

    set->count = count;
    set->active_count = count;
    set->killed_count = 0;
    for (unsigned i = 0; i < count; i++) {
        set->peers[i].id = i;
        set->peers[i].connected = true;
    }
    for (unsigned i = count; i < SIM_PEER_MAX; i++) {
        set->peers[i].id = i;
        set->peers[i].connected = false;
    }
    return 0;
}

int sim_peer_kill(struct sim_peer_set *set, unsigned id)
{
    if (!set) return -EINVAL;
    if (id >= set->count) return -ENOENT;
    if (!set->peers[id].connected) return -EALREADY;

    set->peers[id].connected = false;
    if (set->active_count > 0)
        set->active_count--;
    set->killed_count++;
    return 0;
}

const struct sim_peer *sim_peer_get(const struct sim_peer_set *set,
                                    unsigned id)
{
    if (!set || id >= set->count) return NULL;
    return &set->peers[id];
}
