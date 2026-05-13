/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * blocks_index_legacy_reader.c — see header.
 *
 * Bitcoin Core 0.8+ blocks/index/ LevelDB schema (used unchanged by
 * zclassicd):
 *   key:   'b' || <32-byte block hash, raw byte order>
 *   value: serialized CDiskBlockIndex — same wire format we use
 *          ourselves (see disk_block_index_serialize/deserialize in
 *          lib/storage/src/block_index_db.c).
 *
 * Other keys ('f' for file-info, 't' for txindex, etc.) are skipped.
 */

#include "storage/blocks_index_legacy_reader.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "storage/block_index_db.h"
#include "storage/dbwrapper.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bilr {
    struct db_wrapper db;
};

bool bilr_open(const char *blocks_index_dir, struct bilr **out)
{
    if (!blocks_index_dir || !out)
        return false;
    *out = NULL;

    struct bilr *r = zcl_malloc(sizeof(*r), "bilr");
    if (!r)
        return false;
    memset(r, 0, sizeof(*r));

    /* 16 MB cache — plenty for a one-shot scan; cache hit-rate is
     * effectively zero since we read every record once. */
    if (!db_wrapper_open(&r->db, blocks_index_dir,
                         16u << 20, false, false)) {
        fprintf(stderr,
                "[bilr] open failed: %s "
                "(LOCK held? stop zclassicd or snapshot the dir first)\n",
                blocks_index_dir);
        free(r);
        return false;
    }

    *out = r;
    return true;
}

void bilr_close(struct bilr *r)
{
    if (!r) return;
    db_wrapper_close(&r->db);
    free(r);
}

bool bilr_load_height_map(struct bilr *r,
                          struct legacy_block_loc **out_array,
                          size_t *out_count)
{
    if (!r || !out_array || !out_count)
        return false;
    *out_array = NULL;
    *out_count = 0;

    /* Two-pass to avoid wasting an entire 200 MB allocation on a
     * sparse / corrupt LevelDB. Pass 1 finds max_height + counts
     * usable records; pass 2 fills the array.
     *
     * In practice this doubles the LevelDB scan cost; if it becomes
     * a problem we can switch to a doubling-strategy single-pass. */

    int max_height = -1;
    int64_t scanned = 0;
    int64_t usable  = 0;

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (max_height < 0) {
                fprintf(stderr,
                        "[bilr] no usable block-index records found\n");
                return false;
            }
            size_t count = (size_t)max_height + 1;
            struct legacy_block_loc *arr =
                zcl_malloc(count * sizeof(*arr), "bilr_height_map");
            if (!arr)
                return false;
            for (size_t i = 0; i < count; i++) {
                arr[i].height = -1;
                arr[i].nFile = -1;
            }
            *out_array = arr;
            *out_count = count;
        }

        struct db_iterator it;
        db_iter_init(&it, &r->db);
        const char seek_key = 'b';
        db_iter_seek(&it, &seek_key, 1);

        while (db_iter_valid(&it)) {
            size_t klen = 0;
            const char *k = db_iter_key(&it, &klen);
            if (klen < 1 || k[0] != 'b')
                break;  /* end of 'b' keyspace */
            if (klen != 33) {
                db_iter_next(&it);
                continue;
            }
            if (pass == 0) scanned++;

            size_t vlen = 0;
            const char *v = db_iter_value(&it, &vlen);
            if (!v || vlen == 0) {
                db_iter_next(&it);
                continue;
            }

            struct disk_block_index dbi;
            disk_block_index_init(&dbi);
            struct byte_stream s;
            stream_init_from_data(&s, (unsigned char *)v, vlen);

            if (!disk_block_index_deserialize(&dbi, &s)) {
                stream_free(&s);
                db_iter_next(&it);
                continue;
            }
            stream_free(&s);

            /* Only keep entries that are usable for body-pull:
             * BLOCK_HAVE_DATA must be set (we need an nFile/nDataPos),
             * and the entry must not be FAILED. */
            bool have_data = (dbi.nStatus & BLOCK_HAVE_DATA) != 0;
            bool failed    = (dbi.nStatus & BLOCK_FAILED_MASK) != 0;
            if (!have_data || failed || dbi.nHeight < 0) {
                db_iter_next(&it);
                continue;
            }

            if (pass == 0) {
                if (dbi.nHeight > max_height)
                    max_height = dbi.nHeight;
                usable++;
            } else {
                struct legacy_block_loc *slot =
                    &(*out_array)[(size_t)dbi.nHeight];
                /* If two HAVE_DATA entries land at the same height
                 * (rare: ancient pre-checkpointed fork), keep the one
                 * with BLOCK_VALID_CHAIN — that's the active-chain
                 * member by Bitcoin Core's convention. */
                bool already_set = (slot->height >= 0);
                if (already_set) {
                    bool slot_chain = (slot->nStatus & BLOCK_VALID_CHAIN) != 0;
                    bool new_chain  = (dbi.nStatus & BLOCK_VALID_CHAIN) != 0;
                    if (slot_chain && !new_chain) {
                        db_iter_next(&it);
                        continue;
                    }
                    /* else overwrite */
                }
                slot->height   = dbi.nHeight;
                slot->nFile    = dbi.nFile;
                slot->nDataPos = dbi.nDataPos;
                slot->nUndoPos = dbi.nUndoPos;
                slot->nStatus  = dbi.nStatus;
                /* Hash is encoded in the LevelDB key bytes 1..32 in
                 * native byte order; copy direct to avoid recomputing
                 * the double-SHA256 from the deserialized header. */
                memcpy(slot->hash.data, k + 1, 32);
            }

            db_iter_next(&it);
        }

        db_iter_free(&it);
    }

    fprintf(stderr,
            "[bilr] scanned=%lld usable=%lld max_height=%d\n",
            (long long)scanned, (long long)usable, max_height);
    return true;
}

void bilr_free_height_map(struct legacy_block_loc *array)
{
    free(array);
}
