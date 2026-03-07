/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_STORAGE_DISK_BLOCK_IO_H
#define ZCL_STORAGE_DISK_BLOCK_IO_H

#include "chain/chain.h"
#include "primitives/block.h"
#include <stdbool.h>
#include <stdio.h>

void get_block_pos_filename(char *buf, size_t buflen,
                            const char *datadir,
                            const struct disk_block_pos *pos,
                            const char *prefix);

FILE *open_disk_file(const char *datadir,
                     const struct disk_block_pos *pos,
                     const char *prefix, bool read_only);

static inline FILE *open_block_file(const char *datadir,
                                    const struct disk_block_pos *pos,
                                    bool read_only)
{
    return open_disk_file(datadir, pos, "blk", read_only);
}

static inline FILE *open_undo_file(const char *datadir,
                                   const struct disk_block_pos *pos,
                                   bool read_only)
{
    return open_disk_file(datadir, pos, "rev", read_only);
}

bool write_block_to_disk(struct block *b, struct disk_block_pos *pos,
                         const char *datadir,
                         const unsigned char message_start[4]);

bool read_block_from_disk(struct block *b, const struct disk_block_pos *pos,
                          const char *datadir);

bool read_block_from_disk_index(struct block *b,
                                const struct block_index *pindex,
                                const char *datadir);

#endif
