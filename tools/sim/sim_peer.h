/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_SIM_SIM_PEER_H
#define ZCL_TOOLS_SIM_SIM_PEER_H

#include <stdbool.h>

#define SIM_PEER_MAX 1024u

struct sim_peer {
    unsigned id;
    bool connected;
};

struct sim_peer_set {
    struct sim_peer peers[SIM_PEER_MAX];
    unsigned count;
    unsigned active_count;
    unsigned killed_count;
};

void sim_peer_set_init(struct sim_peer_set *set);
int sim_peer_set_resize(struct sim_peer_set *set, unsigned count);
int sim_peer_kill(struct sim_peer_set *set, unsigned id);
const struct sim_peer *sim_peer_get(const struct sim_peer_set *set,
                                    unsigned id);

#endif /* ZCL_TOOLS_SIM_SIM_PEER_H */
