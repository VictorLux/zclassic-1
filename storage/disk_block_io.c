/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/disk_block_io.h"
#include "core/serialize.h"
#include "core/hash.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

void get_block_pos_filename(char *buf, size_t buflen,
                            const char *datadir,
                            const struct disk_block_pos *pos,
                            const char *prefix)
{
    snprintf(buf, buflen, "%s/blocks/%s%05d.dat", datadir, prefix, pos->nFile);
}

static bool ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(path, 0755) == 0;
}

FILE *open_disk_file(const char *datadir,
                     const struct disk_block_pos *pos,
                     const char *prefix, bool read_only)
{
    if (pos->nFile < 0)
        return NULL;

    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    ensure_directory(blocks_dir);

    char path[512];
    get_block_pos_filename(path, sizeof(path), datadir, pos, prefix);

    FILE *file = fopen(path, "rb+");
    if (!file && !read_only)
        file = fopen(path, "wb+");
    if (!file)
        return NULL;

    if (pos->nPos) {
        if (fseek(file, (long)pos->nPos, SEEK_SET)) {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

bool write_block_to_disk(struct block *b, struct disk_block_pos *pos,
                         const char *datadir,
                         const unsigned char message_start[4])
{
    FILE *file = open_block_file(datadir, pos, false);
    if (!file)
        return false;

    struct byte_stream s;
    stream_init(&s, 4096);
    if (!block_serialize(b, &s)) {
        stream_free(&s);
        fclose(file);
        return false;
    }

    uint32_t nSize = (uint32_t)s.size;

    long file_pos = ftell(file);
    if (file_pos < 0) {
        stream_free(&s);
        fclose(file);
        return false;
    }

    if (fwrite(message_start, 1, 4, file) != 4 ||
        fwrite(&nSize, sizeof(nSize), 1, file) != 1) {
        stream_free(&s);
        fclose(file);
        return false;
    }

    long data_pos = ftell(file);
    if (data_pos < 0) {
        stream_free(&s);
        fclose(file);
        return false;
    }
    pos->nPos = (unsigned int)data_pos;

    if (fwrite(s.data, 1, s.size, file) != s.size) {
        stream_free(&s);
        fclose(file);
        return false;
    }

    stream_free(&s);
    fclose(file);
    return true;
}

bool read_block_from_disk(struct block *b, const struct disk_block_pos *pos,
                          const char *datadir)
{
    block_init(b);

    FILE *file = open_block_file(datadir, pos, true);
    if (!file)
        return false;

    size_t bufsize = 4 * 1024 * 1024;
    unsigned char *buf = malloc(bufsize);
    if (!buf) {
        fclose(file);
        return false;
    }

    size_t nread = fread(buf, 1, bufsize, file);
    fclose(file);

    if (nread == 0) {
        free(buf);
        return false;
    }

    struct byte_stream s;
    stream_init_from_data(&s, buf, nread);
    bool ok = block_deserialize(b, &s);
    stream_free(&s);
    free(buf);

    return ok;
}

bool read_block_from_disk_index(struct block *b,
                                const struct block_index *pindex,
                                const char *datadir)
{
    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        pos.nFile = pindex->nFile;
        pos.nPos = pindex->nDataPos;
    }

    if (!read_block_from_disk(b, &pos, datadir)) {
        printf("read_block_from_disk_index: FAILED height=%d nFile=%d nDataPos=%u nStatus=0x%x\n",
               pindex->nHeight, pindex->nFile, pindex->nDataPos, pindex->nStatus);
        return false;
    }

    struct uint256 block_hash;
    block_get_hash(b, &block_hash);
    if (pindex->phashBlock &&
        uint256_cmp(&block_hash, pindex->phashBlock) != 0) {
        block_free(b);
        return false;
    }
    return true;
}
