/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2013 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_PROTOCOL_C_H
#define ZCL_PROTOCOL_C_H

#include "netaddr.h"
#include "uint256.h"
#include <stdbool.h>
#include <stdint.h>

#define MESSAGE_START_SIZE 4
#define COMMAND_SIZE 12
#define MAX_SIZE 0x02000000

enum {
    NODE_NETWORK = (1 << 0),
    NODE_BLOOM = (1 << 2),
};

enum {
    MSG_TX = 1,
    MSG_BLOCK,
    MSG_FILTERED_BLOCK,
};

enum {
    MSG_HEADER_SIZE = MESSAGE_START_SIZE + COMMAND_SIZE +
                      (int)sizeof(unsigned int) + (int)sizeof(unsigned int)
};

struct msg_header {
    char pchMessageStart[MESSAGE_START_SIZE];
    char pchCommand[COMMAND_SIZE];
    unsigned int nMessageSize;
    unsigned int nChecksum;
};

struct inv_item {
    int type;
    struct uint256 hash;
};

void msg_header_init(struct msg_header *h,
                     const unsigned char msgstart[MESSAGE_START_SIZE]);

void msg_header_init_full(struct msg_header *h,
                          const unsigned char msgstart[MESSAGE_START_SIZE],
                          const char *command, unsigned int msg_size);

int msg_header_get_command(const struct msg_header *h,
                           char *out, size_t out_size);

bool msg_header_is_valid(const struct msg_header *h,
                         const unsigned char msgstart[MESSAGE_START_SIZE]);

void inv_item_init(struct inv_item *inv);
void inv_item_init_typed(struct inv_item *inv, int type,
                         const struct uint256 *hash);
int inv_item_init_by_name(struct inv_item *inv, const char *type_name,
                          const struct uint256 *hash);
bool inv_item_is_known_type(const struct inv_item *inv);
const char *inv_item_get_command(const struct inv_item *inv);
int inv_item_to_string(const struct inv_item *inv, char *out, size_t out_size);
bool inv_item_less(const struct inv_item *a, const struct inv_item *b);

#endif
