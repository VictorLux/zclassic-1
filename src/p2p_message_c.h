/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_P2P_MESSAGE_C_H
#define ZCL_P2P_MESSAGE_C_H

#include "protocol_c.h"
#include "serialize_c.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_SUBVER_LENGTH 256

struct version_message {
    int32_t protocol_version;
    uint64_t services;
    int64_t timestamp;
    struct net_address addr_recv;
    struct net_address addr_from;
    uint64_t nonce;
    char sub_version[MAX_SUBVER_LENGTH];
    int32_t start_height;
    bool relay;
};

static inline void version_message_init(struct version_message *v)
{
    v->protocol_version = 0;
    v->services = 0;
    v->timestamp = 0;
    net_address_init(&v->addr_recv);
    net_address_init(&v->addr_from);
    v->nonce = 0;
    v->sub_version[0] = '\0';
    v->start_height = 0;
    v->relay = true;
}

bool version_message_serialize(const struct version_message *v,
                               struct byte_stream *s);
bool version_message_deserialize(struct version_message *v,
                                 struct byte_stream *s);

#endif
