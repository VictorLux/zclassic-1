/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "protocol_c.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

static const char *ppszTypeName[] = {
    "ERROR",
    "tx",
    "block",
    "filtered block"
};

#define TYPE_NAME_COUNT (sizeof(ppszTypeName) / sizeof(ppszTypeName[0]))

void msg_header_init(struct msg_header *h,
                     const unsigned char msgstart[MESSAGE_START_SIZE])
{
    memcpy(h->pchMessageStart, msgstart, MESSAGE_START_SIZE);
    memset(h->pchCommand, 0, sizeof(h->pchCommand));
    h->nMessageSize = (unsigned int)-1;
    h->nChecksum = 0;
}

void msg_header_init_full(struct msg_header *h,
                          const unsigned char msgstart[MESSAGE_START_SIZE],
                          const char *command, unsigned int msg_size)
{
    memcpy(h->pchMessageStart, msgstart, MESSAGE_START_SIZE);
    memset(h->pchCommand, 0, sizeof(h->pchCommand));
    strncpy(h->pchCommand, command, COMMAND_SIZE);
    h->nMessageSize = msg_size;
    h->nChecksum = 0;
}

int msg_header_get_command(const struct msg_header *h,
                           char *out, size_t out_size)
{
    size_t len = strnlen(h->pchCommand, COMMAND_SIZE);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, h->pchCommand, len);
    out[len] = '\0';
    return (int)len;
}

bool msg_header_is_valid(const struct msg_header *h,
                         const unsigned char msgstart[MESSAGE_START_SIZE])
{
    if (memcmp(h->pchMessageStart, msgstart, MESSAGE_START_SIZE) != 0)
        return false;

    for (const char *p = h->pchCommand; p < h->pchCommand + COMMAND_SIZE; p++) {
        if (*p == 0) {
            for (; p < h->pchCommand + COMMAND_SIZE; p++)
                if (*p != 0)
                    return false;
            break;
        }
        if (*p < ' ' || *p > 0x7E)
            return false;
    }

    if (h->nMessageSize > MAX_SIZE) {
        char cmd[COMMAND_SIZE + 1];
        msg_header_get_command(h, cmd, sizeof(cmd));
        LogPrintf("msg_header_is_valid: (%s, %u bytes) nMessageSize > MAX_SIZE\n",
                  cmd, h->nMessageSize);
        return false;
    }

    return true;
}

void inv_item_init(struct inv_item *inv)
{
    inv->type = 0;
    uint256_set_null(&inv->hash);
}

void inv_item_init_typed(struct inv_item *inv, int type,
                         const struct uint256 *hash)
{
    inv->type = type;
    inv->hash = *hash;
}

int inv_item_init_by_name(struct inv_item *inv, const char *type_name,
                          const struct uint256 *hash)
{
    for (unsigned int i = 1; i < TYPE_NAME_COUNT; i++) {
        if (strcmp(type_name, ppszTypeName[i]) == 0) {
            inv->type = (int)i;
            inv->hash = *hash;
            return 0;
        }
    }
    return -1;
}

bool inv_item_is_known_type(const struct inv_item *inv)
{
    return inv->type >= 1 && (unsigned int)inv->type < TYPE_NAME_COUNT;
}

const char *inv_item_get_command(const struct inv_item *inv)
{
    if (!inv_item_is_known_type(inv))
        return "UNKNOWN";
    return ppszTypeName[inv->type];
}

int inv_item_to_string(const struct inv_item *inv, char *out, size_t out_size)
{
    char hash_str[65];
    uint256_get_hex(&inv->hash, hash_str);
    return snprintf(out, out_size, "%s %s",
                    inv_item_get_command(inv), hash_str);
}

bool inv_item_less(const struct inv_item *a, const struct inv_item *b)
{
    if (a->type != b->type)
        return a->type < b->type;
    return uint256_cmp(&a->hash, &b->hash) < 0;
}
