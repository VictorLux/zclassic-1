/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "netaddr.h"
#include <stdio.h>

int net_addr_to_string(const struct net_addr *a, char *out, size_t out_size)
{
    if (net_addr_is_ipv4(a)) {
        return snprintf(out, out_size, "%u.%u.%u.%u",
                        a->ip[12], a->ip[13], a->ip[14], a->ip[15]);
    }

    if (a->has_torv3) {
        return snprintf(out, out_size, "[torv3]");
    }

    return snprintf(out, out_size,
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                    a->ip[0], a->ip[1], a->ip[2], a->ip[3],
                    a->ip[4], a->ip[5], a->ip[6], a->ip[7],
                    a->ip[8], a->ip[9], a->ip[10], a->ip[11],
                    a->ip[12], a->ip[13], a->ip[14], a->ip[15]);
}

int net_service_to_string(const struct net_service *s, char *out, size_t out_size)
{
    char addr_str[128];
    net_addr_to_string(&s->addr, addr_str, sizeof(addr_str));

    if (net_addr_is_ipv6(&s->addr) && !s->addr.has_torv3) {
        return snprintf(out, out_size, "[%s]:%u", addr_str, s->port);
    }
    return snprintf(out, out_size, "%s:%u", addr_str, s->port);
}
