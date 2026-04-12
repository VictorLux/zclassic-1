/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Import wallet data from a legacy (C++) ZClassic node's block files.
 *
 * No LevelDB, no chain index, no RPC. Reads block files directly:
 *   Pass 1: mmap raw byte scan for P2PKH/P2SH wallet patterns (~1.7s)
 *   Pass 2: Walk matched files, deserialize blocks, extract BIP34 height
 *   Pass 3: Same walk for Sapling trial decryption (skip non-v4 txns)
 *
 * The legacy node should be stopped to avoid partial block reads. */

#include "views/format_helpers.h"
#include "controllers/legacy_import.h"
#include "controllers/wallet_scan.h"
#include "controllers/sync_controller.h"
#include "models/wallet_tx.h"
#include "validation/chainstate.h"
#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "primitives/block.h"
#include "core/serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include "controllers/scan_util.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static const uint8_t ZCL_MAGIC[4] = {0x24, 0xe9, 0x27, 0x64};

static bool legacy_import_exec_checked(struct node_db *ndb,
                                       const char *sql,
                                       const char *label)
{
    if (!ndb || !ndb->open || !sql)
        return false;
    if (sqlite3_exec(ndb->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "legacy_import: %s failed: %s\n",
                label, sqlite3_errmsg(ndb->db));
        return false;
    }
    return true;
}

static bool legacy_import_begin_checked(struct node_db *ndb,
                                        const char *label)
{
    if (!ndb || !ndb->open || !node_db_begin(ndb)) {
        fprintf(stderr, "legacy_import: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static bool legacy_import_commit_checked(struct node_db *ndb,
                                         const char *label)
{
    if (!ndb || !ndb->open || !node_db_commit(ndb)) {
        fprintf(stderr, "legacy_import: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static bool legacy_import_rollback_checked(struct node_db *ndb,
                                           const char *label)
{
    if (!ndb || !ndb->open || !node_db_rollback(ndb)) {
        fprintf(stderr, "legacy_import: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static uint8_t *ser_tx(const struct transaction *tx, size_t *len)
{
    struct byte_stream s;
    stream_init(&s, 512);
    transaction_serialize(tx, &s);
    *len = s.size;
    return s.data;
}

/* Extract BIP34 block height from coinbase scriptSig. */
static int extract_bip34_height(const struct transaction *coinbase)
{
    if (coinbase->num_vin == 0) return -1;
    const uint8_t *sig = coinbase->vin[0].script_sig.data;
    size_t sig_len = coinbase->vin[0].script_sig.size;
    if (sig_len < 1) return -1;

    uint8_t nbytes = sig[0];

    /* OP_0 = height 0 */
    if (nbytes == 0x00) return 0;
    /* OP_1 through OP_16 = heights 1-16 */
    if (nbytes >= 0x51 && nbytes <= 0x60)
        return nbytes - 0x50;

    /* CScriptNum: nbytes = number of following bytes encoding height */
    if (nbytes > 8 || (size_t)nbytes + 1 > sig_len) return -1;
    int64_t h = 0;
    for (uint8_t i = 0; i < nbytes; i++)
        h |= (int64_t)sig[1 + i] << (8 * i);
    /* Sign bit handling */
    if (sig[nbytes] & 0x80)
        h = -(h & ~((int64_t)0x80 << (8 * (nbytes - 1))));
    return (int)h;
}

/* --- Pass 1: Parallel raw byte scan (same as wallet_scan.c) --- */

static bool scan_file_raw(const uint8_t *data, size_t size,
                           const struct addr_ht *ht)
{
    for (size_t i = 0; i + 25 <= size; i++) {
        if (data[i] == 0x76 && data[i + 1] == 0xa9 &&
            data[i + 2] == 0x14 &&
            data[i + 23] == 0x88 && data[i + 24] == 0xac) {
            if (aht_has(ht, data + i + 3)) return true;
        }
        if (data[i] == 0xa9 && data[i + 1] == 0x14 &&
            i + 23 <= size && data[i + 22] == 0x87) {
            if (aht_has(ht, data + i + 2)) return true;
        }
    }
    return false;
}

struct scan_thread_arg {
    const char *datadir;
    int file_num;
    const struct addr_ht *ht;
    bool result;
};

static void *scan_file_thread(void *arg)
{
    struct scan_thread_arg *a = (struct scan_thread_arg *)arg;
    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             a->datadir, a->file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { a->result = false; return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); a->result = false; return NULL; }
    size_t sz = (size_t)st.st_size;
    uint8_t *data = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { a->result = false; return NULL; }
    posix_madvise(data, sz, POSIX_MADV_SEQUENTIAL);
    a->result = scan_file_raw(data, sz, a->ht);
    munmap(data, sz);
    return NULL;
}

/* --- Pass 2: Walk matched block files directly --- */

/* Process a single deserialized block for wallet txns. */
static bool scan_block_txs(const struct block *blk, int height,
                            const struct addr_ht *ht,
                            struct utxo_set *uset,
                            struct wtx_list *wl)
{
    bool any_found = false;

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        struct transaction *tx = &blk->vtx[ti];
        bool is_ours = false, from_me = false;
        int64_t debit = 0;

        for (size_t vi = 0; vi < tx->num_vout; vi++) {
            uint8_t ah[20];
            if (!extract_addr(tx->vout[vi].script_pub_key.data,
                              tx->vout[vi].script_pub_key.size, ah))
                continue;
            if (!aht_has(ht, ah)) continue;

            is_ours = true;
            struct mem_utxo u;
            memset(&u, 0, sizeof(u));
            memcpy(u.txid, tx->hash.data, 32);
            u.vout = (uint32_t)vi;
            u.value = tx->vout[vi].value;
            memcpy(u.addr_hash, ah, 20);
            size_t sl = tx->vout[vi].script_pub_key.size;
            if (sl > sizeof(u.script)) sl = sizeof(u.script);
            memcpy(u.script, tx->vout[vi].script_pub_key.data, sl);
            u.script_len = (uint8_t)sl;
            u.height = height;
            u.is_coinbase = (ti == 0);
            uset_add(uset, &u);
        }

        if (ti > 0) {
            for (size_t vi = 0; vi < tx->num_vin; vi++) {
                int ui = uset_find(uset,
                    tx->vin[vi].prevout.hash.data,
                    tx->vin[vi].prevout.n);
                if (ui >= 0) {
                    is_ours = true;
                    from_me = true;
                    debit += uset->items[ui].value;
                    uset->items[ui].spent = true;
                    memcpy(uset->items[ui].spent_txid,
                           tx->hash.data, 32);
                    uset->items[ui].spent_vin = (int)vi;
                }
            }
        }

        if (is_ours) {
            any_found = true;
            struct mem_wtx wt;
            memset(&wt, 0, sizeof(wt));
            memcpy(wt.txid, tx->hash.data, 32);
            wt.raw = ser_tx(tx, &wt.raw_len);
            wt.height = height;
            wt.time = blk->header.nTime;
            wt.from_me = from_me;
            if (from_me) {
                int64_t vout_total = transaction_get_value_out(tx);
                wt.fee = debit > vout_total ? debit - vout_total : 0;
            }
            wl_add(wl, &wt);
        }
    }
    return any_found;
}

/* Walk a block file, deserialize each block at magic boundaries.
 * For transparent wallet scan (pass 2) or Sapling scan (pass 3). */
typedef bool (*block_visitor_fn)(const struct block *blk, int height,
                                  void *ctx);

static int walk_block_file(const uint8_t *fdata, size_t fsize,
                            block_visitor_fn visitor, void *ctx)
{
    int blocks = 0;
    size_t pos = 0;

    while (pos + 8 <= fsize) {
        /* Find magic bytes. */
        if (memcmp(fdata + pos, ZCL_MAGIC, 4) != 0) {
            pos++;
            continue;
        }
        uint32_t blk_size;
        memcpy(&blk_size, fdata + pos + 4, 4);
        if (blk_size == 0 || blk_size > 4000000 ||
            pos + 8 + blk_size > fsize) {
            pos++;
            continue;
        }

        struct block blk;
        block_init(&blk);
        struct byte_stream bs;
        stream_init_from_data(&bs, fdata + pos + 8, blk_size);
        if (!block_deserialize(&blk, &bs)) {
            block_free(&blk);
            pos += 8 + blk_size;
            continue;
        }

        int height = -1;
        if (blk.num_vtx > 0)
            height = extract_bip34_height(&blk.vtx[0]);

        blocks++;
        visitor(&blk, height, ctx);
        block_free(&blk);
        pos += 8 + blk_size;
    }
    return blocks;
}

/* --- Pass 2 visitor: transparent scan --- */

struct transparent_ctx {
    const struct addr_ht *ht;
    struct utxo_set *uset;
    struct wtx_list *wl;
    int found;
};

static bool transparent_visitor(const struct block *blk, int height,
                                 void *ctx)
{
    struct transparent_ctx *tc = (struct transparent_ctx *)ctx;
    if (scan_block_txs(blk, height, tc->ht, tc->uset, tc->wl))
        tc->found++;
    return true;
}

/* --- Pass 3: Sapling scan with lightweight pre-filter --- */

#define SAPLING_ACTIVATION_HEIGHT 476969

/* Read a Bitcoin CompactSize varint from raw bytes.
 * Returns bytes consumed, or 0 on error. */
static int read_compact_size_raw(const uint8_t *d, size_t avail, uint64_t *out)
{
    if (avail < 1) return 0;
    if (d[0] < 0xfd) { *out = d[0]; return 1; }
    if (d[0] == 0xfd && avail >= 3) {
        *out = (uint64_t)d[1] | ((uint64_t)d[2] << 8);
        return 3;
    }
    if (d[0] == 0xfe && avail >= 5) {
        uint32_t v; memcpy(&v, d + 1, 4); *out = v;
        return 5;
    }
    if (d[0] == 0xff && avail >= 9) {
        memcpy(out, d + 1, 8);
        return 9;
    }
    return 0;
}

/* Extract BIP34 height from raw block data without full deserialization.
 * Parses only: header → num_tx → coinbase scriptSig → height.
 * ~200 bytes examined vs full block_deserialize of entire block. */
static int extract_height_raw(const uint8_t *bdata, size_t bsize)
{
    /* Header: version(4)+hashPrev(32)+hashMerkle(32)+hashReserved(32)+
     *         nTime(4)+nBits(4)+nNonce(32) = 140, then solution */
    if (bsize < 141) return -1;
    size_t pos = 140;
    uint64_t sol_size;
    int n = read_compact_size_raw(bdata + pos, bsize - pos, &sol_size);
    if (n == 0 || sol_size > 4096) return -1;
    pos += (size_t)n + (size_t)sol_size;

    /* num_tx */
    if (pos >= bsize) return -1;
    uint64_t num_tx;
    n = read_compact_size_raw(bdata + pos, bsize - pos, &num_tx);
    if (n == 0 || num_tx == 0) return -1;
    pos += (size_t)n;

    /* Coinbase tx version (with optional fOverwintered bit) */
    if (pos + 4 > bsize) return -1;
    int32_t tx_ver;
    memcpy(&tx_ver, bdata + pos, 4);
    pos += 4;
    if (tx_ver & (int32_t)0x80000000) pos += 4; /* skip versionGroupId */

    /* vin_count */
    if (pos >= bsize) return -1;
    uint64_t vin_count;
    n = read_compact_size_raw(bdata + pos, bsize - pos, &vin_count);
    if (n == 0 || vin_count == 0) return -1;
    pos += (size_t)n;

    /* First vin prevout (36 bytes) + scriptSig */
    pos += 36;
    if (pos >= bsize) return -1;
    uint64_t script_len;
    n = read_compact_size_raw(bdata + pos, bsize - pos, &script_len);
    if (n == 0) return -1;
    pos += (size_t)n;
    if (pos + script_len > bsize || script_len == 0) return -1;

    /* BIP34 height from scriptSig */
    const uint8_t *sig = bdata + pos;
    uint8_t nbytes = sig[0];
    if (nbytes == 0x00) return 0;
    if (nbytes >= 0x51 && nbytes <= 0x60) return nbytes - 0x50;
    if (nbytes > 8 || (size_t)nbytes + 1 > script_len) return -1;
    int64_t h = 0;
    for (uint8_t i = 0; i < nbytes; i++)
        h |= (int64_t)sig[1 + i] << (8 * i);
    if (sig[nbytes] & 0x80)
        h = -(h & ~((int64_t)0x80 << (8 * (nbytes - 1))));
    return (int)h;
}

/* Check raw block data for any transaction with shielded outputs or spends.
 * Skips through serialized tx data without malloc — just pointer arithmetic.
 * Returns true if any tx has num_shielded_output > 0 or num_shielded_spend > 0. */
static bool block_has_shielded_raw(const uint8_t *bdata, size_t bsize)
{
    /* Skip block header: 140 fixed bytes + varint(solution) + solution */
    if (bsize < 141) return false;
    size_t pos = 140;
    uint64_t sol_size;
    int n = read_compact_size_raw(bdata + pos, bsize - pos, &sol_size);
    if (n == 0) return false;
    pos += (size_t)n + (size_t)sol_size;

    /* num_tx */
    if (pos >= bsize) return false;
    uint64_t num_tx;
    n = read_compact_size_raw(bdata + pos, bsize - pos, &num_tx);
    if (n == 0 || num_tx == 0 || num_tx > 50000) return false;
    pos += (size_t)n;

    /* For each transaction, skip to num_shielded_spend/output. */
    for (uint64_t ti = 0; ti < num_tx; ti++) {
        if (pos + 4 > bsize) return false;
        int32_t tx_ver;
        memcpy(&tx_ver, bdata + pos, 4);
        pos += 4;
        bool overwintered = (tx_ver & (int32_t)0x80000000) != 0;
        int32_t ver = tx_ver & 0x7FFFFFFF;
        uint32_t vg_id = 0;
        if (overwintered) {
            if (pos + 4 > bsize) return false;
            memcpy(&vg_id, bdata + pos, 4);
            pos += 4;
        }

        /* Skip vin: count + each(prevout(36) + varint(scriptSig) + seq(4)) */
        uint64_t vin_count;
        n = read_compact_size_raw(bdata + pos, bsize - pos, &vin_count);
        if (n == 0) return false;
        pos += (size_t)n;
        for (uint64_t vi = 0; vi < vin_count; vi++) {
            pos += 36; /* prevout */
            if (pos >= bsize) return false;
            uint64_t script_len;
            n = read_compact_size_raw(bdata + pos, bsize - pos, &script_len);
            if (n == 0) return false;
            pos += (size_t)n + (size_t)script_len + 4; /* script + seq */
        }

        /* Skip vout: count + each(value(8) + varint(script) + script) */
        if (pos >= bsize) return false;
        uint64_t vout_count;
        n = read_compact_size_raw(bdata + pos, bsize - pos, &vout_count);
        if (n == 0) return false;
        pos += (size_t)n;
        for (uint64_t vo = 0; vo < vout_count; vo++) {
            pos += 8; /* value */
            if (pos >= bsize) return false;
            uint64_t script_len;
            n = read_compact_size_raw(bdata + pos, bsize - pos, &script_len);
            if (n == 0) return false;
            pos += (size_t)n + (size_t)script_len;
        }

        /* nLockTime(4) + nExpiryHeight(4) */
        if (pos + 4 > bsize) return false;
        pos += 4; /* nLockTime */
        if (overwintered) {
            if (pos + 4 > bsize) return false;
            pos += 4; /* nExpiryHeight */
        }

        /* Sapling: v4 (versionGroupId == 0x892F2085) */
        if (overwintered && vg_id == 0x892F2085) {
            if (pos + 8 > bsize) return false;
            pos += 8; /* valueBalance */

            /* num_shielded_spend */
            if (pos >= bsize) return false;
            uint64_t num_spend;
            n = read_compact_size_raw(bdata + pos, bsize - pos, &num_spend);
            if (n == 0) return false;
            pos += (size_t)n;
            /* skip spends: each 384 bytes */
            pos += (size_t)num_spend * 384;

            /* num_shielded_output */
            if (pos >= bsize) return false;
            uint64_t num_output;
            n = read_compact_size_raw(bdata + pos, bsize - pos, &num_output);
            if (n == 0) return false;
            pos += (size_t)n;
            if (num_output > 0) return true;
            /* skip outputs: each 948 bytes */
            pos += (size_t)num_output * 948;
        }

        /* Skip JoinSplits (v2+, including Sapling v4) */
        if (ver >= 2 && (!overwintered || ver < 5)) {
            if (pos >= bsize) return false;
            uint64_t num_js;
            n = read_compact_size_raw(bdata + pos, bsize - pos, &num_js);
            if (n == 0) return false;
            pos += (size_t)n;
            if (num_js > 0) {
                /* Per JoinSplit: 304 fixed + proof + 2×601 ciphertext */
                size_t js_size = (overwintered && vg_id == 0x892F2085) ?
                                  1698 : 1802;
                pos += (size_t)num_js * js_size;
                pos += 32 + 64; /* joinSplitPubKey + joinSplitSig */
            }
        }

        /* Binding sig: only present when has shielded spend or output.
         * But we returned true above in that case, so if we reach here,
         * both counts are 0 — no binding sig to skip. */
    }
    return false;
}

/* Block position record — collected by filter threads. */
struct blk_pos {
    int file_num;
    size_t offset;      /* offset of magic bytes in file */
    uint32_t blk_size;
    int height;
};

struct filter_file_ctx {
    const char *datadir;
    int file_num;
    int blocks_total;
    int blocks_passed;
    int height_failed;
    struct blk_pos *hits;
    int hit_count;
    int hit_cap;
};

/* Thread: scan one file, apply size + height filters, collect positions. */
static void *sapling_filter_thread(void *arg)
{
    struct filter_file_ctx *ctx = (struct filter_file_ctx *)arg;
    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             ctx->datadir, ctx->file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    size_t fsize = (size_t)st.st_size;
    uint8_t *fdata = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (fdata == MAP_FAILED) return NULL;
    posix_madvise(fdata, fsize, POSIX_MADV_SEQUENTIAL);

    ctx->hit_cap = 256;
    ctx->hits = zcl_malloc((size_t)ctx->hit_cap * sizeof(struct blk_pos), "legacy scan hits");

    size_t pos = 0;
    while (pos + 8 <= fsize) {
        if (memcmp(fdata + pos, ZCL_MAGIC, 4) != 0) { pos++; continue; }
        uint32_t blk_size;
        memcpy(&blk_size, fdata + pos + 4, 4);
        if (blk_size == 0 || blk_size > 4000000 ||
            pos + 8 + blk_size > fsize) { pos++; continue; }
        ctx->blocks_total++;

        /* Filter 1: lightweight height — skip pre-Sapling. */
        int height = extract_height_raw(fdata + pos + 8, blk_size);
        if (height < 0) ctx->height_failed++;
        if (height >= 0 && height < SAPLING_ACTIVATION_HEIGHT) {
            pos += 8 + blk_size;
            continue;
        }

        /* Filter 2: parse raw tx structure — skip blocks without
         * any shielded spends or outputs. No malloc, just pointer
         * arithmetic through serialized data. */
        if (!block_has_shielded_raw(fdata + pos + 8, blk_size)) {
            pos += 8 + blk_size;
            continue;
        }

        /* Record this block for trial decryption. */
        if (ctx->hit_count >= ctx->hit_cap) {
            ctx->hit_cap *= 2;
            ctx->hits = zcl_realloc(ctx->hits,
                (size_t)ctx->hit_cap * sizeof(struct blk_pos), "legacy scan hits grow");
        }
        ctx->hits[ctx->hit_count++] = (struct blk_pos){
            .file_num = ctx->file_num,
            .offset = pos,
            .blk_size = blk_size,
            .height = height,
        };
        ctx->blocks_passed++;
        pos += 8 + blk_size;
    }
    munmap(fdata, fsize);
    return NULL;
}

/* --- Pass 3b: parallel trial decryption thread --- */

struct decrypt_file_ctx {
    const char *datadir;
    struct blk_pos *hits;
    int count;
    int file_num;
    struct wallet tw;
    int outputs_seen;
    int notes_found;
    struct db_sapling_note *results;
    int result_count;
    int result_cap;
};

static void *decrypt_thread(void *arg)
{
    struct decrypt_file_ctx *c = (struct decrypt_file_ctx *)arg;
    if (c->count == 0) return NULL;

    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             c->datadir, c->file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    size_t fsize = (size_t)st.st_size;
    uint8_t *fdata = mmap(NULL, fsize, PROT_READ,
                           MAP_PRIVATE, fd, 0);
    close(fd);
    if (fdata == MAP_FAILED) return NULL;

    for (int hi = 0; hi < c->count; hi++) {
        struct blk_pos *bp = &c->hits[hi];
        const uint8_t *bdata = fdata + bp->offset + 8;
        struct block blk;
        block_init(&blk);
        struct byte_stream bs;
        stream_init_from_data(&bs, bdata, bp->blk_size);
        if (!block_deserialize(&blk, &bs)) {
            block_free(&blk);
            continue;
        }

        int height = bp->height;
        if (height < 0 && blk.num_vtx > 0)
            height = extract_bip34_height(&blk.vtx[0]);

        for (size_t ti = 0; ti < blk.num_vtx; ti++) {
            struct transaction *tx = &blk.vtx[ti];
            if (tx->num_shielded_output == 0) continue;
            c->outputs_seen += (int)tx->num_shielded_output;
            struct uint256 txid = tx->hash;
            int n = wallet_try_sapling_decrypt(&c->tw, tx, &txid);
            if (n > 0) {
                c->notes_found += n;
                for (size_t ni = 0;
                     ni < c->tw.num_sapling_notes; ni++) {
                    struct sapling_received_note *note =
                        &c->tw.sapling_notes[ni];
                    if (!note->used) continue;
                    if (memcmp(note->txid.data, txid.data, 32) != 0)
                        continue;
                    if (c->result_count >= c->result_cap) {
                        c->result_cap *= 2;
                        c->results = zcl_realloc(c->results,
                            (size_t)c->result_cap *
                            sizeof(struct db_sapling_note), "sapling decrypt results grow");
                    }
                    struct db_sapling_note *dn =
                        &c->results[c->result_count++];
                    memset(dn, 0, sizeof(*dn));
                    memcpy(dn->txid, note->txid.data, 32);
                    dn->output_index = note->output_index;
                    dn->value = (int64_t)note->value;
                    memcpy(dn->rcm, note->rcm, 32);
                    memcpy(dn->memo, note->memo, 512);
                    dn->memo_len = 512;
                    memcpy(dn->ivk, note->ivk, 32);
                    memcpy(dn->diversifier, note->diversifier, 11);
                    memcpy(dn->pk_d, note->pk_d, 32);
                    memcpy(dn->cm, note->cm, 32);
                    memcpy(dn->nullifier, note->nf, 32);
                    dn->block_height = height;
                }
            }
        }
        block_free(&blk);
    }
    munmap(fdata, fsize);
    return NULL;
}

/* --- Main entry point --- */

int legacy_import(const char *legacy_datadir,
                  struct node_db *ndb,
                  struct wallet *w,
                  bool sapling_scan)
{
    int ret = -1;
    struct filter_file_ctx *fctxs = NULL;
    struct decrypt_file_ctx *dctxs = NULL;

    if (!legacy_datadir || !ndb || !ndb->open || !w)
        LOG_ERR("legacy_import", "invalid args: datadir=%p ndb=%p open=%d wallet=%p",
                (const void *)legacy_datadir, (const void *)ndb,
                (ndb ? ndb->open : 0), (const void *)w);

    struct timespec ts_start, ts_p1, ts_p2, ts_p3;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* Build address hash table from wallet keys. */
    struct addr_ht aht;
    aht_init(&aht);
    for (size_t i = 0; i < w->keystore.num_keys; i++)
        if (w->keystore.keys[i].used)
            aht_insert(&aht, w->keystore.keys[i].keyid.id.data);
    for (size_t i = 0; i < w->keystore.num_scripts; i++)
        if (w->keystore.scripts[i].used)
            aht_insert(&aht, w->keystore.scripts[i].script_id.data);

    printf("legacy_import: %d address hashes, %zu sapling keys\n",
           aht.count, w->sapling_keys.num_keys);
    fflush(stdout);

    /* Count block files. */
    int num_files = 0;
    for (int f = 0; f < 200; f++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 legacy_datadir, f);
        if (access(path, R_OK) != 0) break;
        num_files = f + 1;
    }
    printf("legacy_import: %d block files in %s\n",
           num_files, legacy_datadir);
    fflush(stdout);

    /* ========== PASS 1: Parallel raw byte scan ========== */
    printf("legacy_import: pass 1 — parallel raw byte scan...\n");
    fflush(stdout);

    bool *file_has_match = zcl_calloc((size_t)num_files, sizeof(bool), "legacy file match flags");
    int batch = 8;

    for (int base = 0; base < num_files; base += batch) {
        int n = num_files - base;
        if (n > batch) n = batch;
        struct scan_thread_arg args[8];
        pthread_t threads[8];
        int started = 0;
        for (int i = 0; i < n; i++) {
            args[i].datadir = legacy_datadir;
            args[i].file_num = base + i;
            args[i].ht = &aht;
            args[i].result = false;
            if (pthread_create(&threads[i], NULL,
                               scan_file_thread, &args[i]) != 0) {
                fprintf(stderr,
                        "legacy_import: failed to start pass-1 scan thread\n");
                for (int j = 0; j < started; j++)
                    pthread_join(threads[j], NULL);
                goto cleanup;
            }
            started++;
        }
        for (int i = 0; i < n; i++) {
            pthread_join(threads[i], NULL);
            file_has_match[base + i] = args[i].result;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_p1);
    double p1_ms = (double)(ts_p1.tv_sec - ts_start.tv_sec) * 1000.0 +
                   (double)(ts_p1.tv_nsec - ts_start.tv_nsec) / 1e6;

    int matched = 0;
    for (int i = 0; i < num_files; i++)
        if (file_has_match[i]) matched++;
    printf("legacy_import: pass 1 done in %.1f ms — %d/%d files match\n",
           p1_ms, matched, num_files);
    fflush(stdout);

    /* ========== PASS 2: Walk matched files for transparent txns ========== */
    printf("legacy_import: pass 2 — transparent scan of %d files...\n",
           matched);
    fflush(stdout);

    struct utxo_set uset;
    uset_init(&uset);
    struct wtx_list wl;
    wl_init(&wl);
    struct transparent_ctx tctx = { &aht, &uset, &wl, 0 };

    int total_blocks_p2 = 0;
    for (int f = 0; f < num_files; f++) {
        if (!file_has_match[f]) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 legacy_datadir, f);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); continue; }
        size_t sz = (size_t)st.st_size;
        uint8_t *data = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (data == MAP_FAILED) continue;
        posix_madvise(data, sz, POSIX_MADV_SEQUENTIAL);

        total_blocks_p2 += walk_block_file(data, sz,
                                            transparent_visitor, &tctx);
        munmap(data, sz);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_p2);
    double p2_ms = (double)(ts_p2.tv_sec - ts_p1.tv_sec) * 1000.0 +
                   (double)(ts_p2.tv_nsec - ts_p1.tv_nsec) / 1e6;

    int64_t balance = 0;
    int unspent = 0;
    for (int i = 0; i < uset.count; i++) {
        if (!uset.items[i].spent) {
            balance += uset.items[i].value;
            unspent++;
        }
    }

    printf("legacy_import: pass 2 done in %.1f ms — %d blocks, "
           "%d wallet txs, %d UTXOs, balance %.8f ZCL\n",
           p2_ms, total_blocks_p2, wl.count, unspent,
           (double)balance / (double)ZATOSHI_PER_ZCL);
    fflush(stdout);

    /* Write transparent results to SQLite. */
    {
        bool import_tx_open = false;
        if (!legacy_import_begin_checked(ndb, "pass 2 begin")) {
            goto cleanup;
        }
        import_tx_open = true;
        if (!legacy_import_exec_checked(ndb, "DELETE FROM wallet_utxos",
                                        "pass 2 clear wallet_utxos") ||
            !legacy_import_exec_checked(ndb,
                                        "DELETE FROM wallet_transactions",
                                        "pass 2 clear wallet_transactions") ||
            !legacy_import_exec_checked(ndb,
                                        "DELETE FROM wallet_sapling_notes",
                                        "pass 2 clear wallet_sapling_notes")) {
            goto pass2_db_fail;
        }

        for (int i = 0; i < uset.count; i++) {
            struct mem_utxo *u = &uset.items[i];
            struct db_wallet_utxo du;
            memset(&du, 0, sizeof(du));
            memcpy(du.txid, u->txid, 32);
            du.vout = u->vout;
            du.value = u->value;
            memcpy(du.address_hash, u->addr_hash, 20);
            du.script = u->script;
            du.script_len = u->script_len;
            du.height = u->height;
            du.is_coinbase = u->is_coinbase;
            if (!db_wallet_utxo_save(ndb, &du)) {
                fprintf(stderr,
                        "legacy_import: pass 2 wallet_utxo save failed\n");
                goto pass2_db_fail;
            }
            if (u->spent &&
                !db_wallet_utxo_mark_spent(ndb, u->txid, u->vout,
                                           u->spent_txid, u->spent_vin)) {
                fprintf(stderr,
                        "legacy_import: pass 2 wallet_utxo mark_spent failed\n");
                goto pass2_db_fail;
            }
        }

        for (int i = 0; i < wl.count; i++) {
            struct mem_wtx *t = &wl.items[i];
            struct db_wallet_tx dt;
            memset(&dt, 0, sizeof(dt));
            memcpy(dt.txid, t->txid, 32);
            dt.raw_tx = t->raw;
            dt.raw_tx_len = t->raw_len;
            dt.has_block = true;
            dt.block_height = t->height;
            dt.time_received = (int64_t)t->time;
            dt.from_me = t->from_me;
            dt.fee = t->fee;
            if (!db_wallet_tx_save(ndb, &dt)) {
                fprintf(stderr,
                        "legacy_import: pass 2 wallet_tx save failed\n");
                goto pass2_db_fail;
            }
        }
        if (!legacy_import_commit_checked(ndb, "pass 2 commit")) {
            import_tx_open = false;
            goto cleanup;
        }
        import_tx_open = false;
        goto pass2_db_done;

pass2_db_fail:
        if (import_tx_open &&
            !legacy_import_rollback_checked(ndb, "pass 2 rollback")) {
            fprintf(stderr,
                    "legacy_import: pass 2 rollback failed after DB error\n");
        }
        goto cleanup;
pass2_db_done:
        ;
    }

    /* ========== PASS 3: Sapling trial decryption ========== */
    /* Phase A: parallel filter (8 threads) — lightweight height + size
     *          filter to find candidate blocks without deserialization.
     * Phase B: serial trial decryption — only on candidate blocks. */
    int z_found = 0;
    if (sapling_scan && w->sapling_keys.num_keys > 0) {
        printf("legacy_import: pass 3a — parallel block filter "
               "(%d files, 8 threads)...\n", num_files);
        fflush(stdout);

        fctxs = zcl_calloc((size_t)num_files, sizeof(struct filter_file_ctx), "sapling filter contexts");
        if (!fctxs) {
            fprintf(stderr,
                    "legacy_import: failed to allocate sapling filter contexts\n");
            goto cleanup;
        }
        for (int base = 0; base < num_files; base += batch) {
            int n = num_files - base;
            if (n > batch) n = batch;
            pthread_t thr[8];
            int started = 0;
            for (int i = 0; i < n; i++) {
                fctxs[base + i].datadir = legacy_datadir;
                fctxs[base + i].file_num = base + i;
                if (pthread_create(&thr[i], NULL,
                                   sapling_filter_thread,
                                   &fctxs[base + i]) != 0) {
                    fprintf(stderr,
                            "legacy_import: failed to start sapling filter thread\n");
                    for (int j = 0; j < started; j++)
                        pthread_join(thr[j], NULL);
                    goto cleanup;
                }
                started++;
            }
            for (int i = 0; i < n; i++)
                pthread_join(thr[i], NULL);
        }

        int total_blocks = 0, total_candidates = 0, total_hfail = 0;
        for (int f = 0; f < num_files; f++) {
            total_blocks += fctxs[f].blocks_total;
            total_candidates += fctxs[f].hit_count;
            total_hfail += fctxs[f].height_failed;
        }

        struct timespec ts_p3a;
        clock_gettime(CLOCK_MONOTONIC, &ts_p3a);
        double p3a_ms = (double)(ts_p3a.tv_sec - ts_p2.tv_sec) * 1000.0 +
                        (double)(ts_p3a.tv_nsec - ts_p2.tv_nsec) / 1e6;
        printf("legacy_import: pass 3a done in %.1f ms — %d blocks, "
               "%d candidates (%.1f%%), %d height-fail\n",
               p3a_ms, total_blocks, total_candidates,
               total_blocks > 0 ?
                   100.0 * (double)total_candidates / (double)total_blocks :
                   0.0, total_hfail);
        fflush(stdout);

        /* Phase B: parallel deserialize + trial decrypt.
         * Each thread gets its own wallet clone (sapling keys only)
         * to avoid shared-state races. Results merged after. */
        printf("legacy_import: pass 3b — parallel trial decryption of "
               "%d blocks (%zu keys, 8 threads)...\n",
               total_candidates, w->sapling_keys.num_keys);
        fflush(stdout);

        dctxs = zcl_calloc((size_t)num_files, sizeof(struct decrypt_file_ctx), "sapling decrypt contexts");
        if (!dctxs) {
            fprintf(stderr,
                    "legacy_import: failed to allocate sapling decrypt contexts\n");
            goto cleanup;
        }
        for (int f = 0; f < num_files; f++) {
            dctxs[f].datadir = legacy_datadir;
            dctxs[f].hits = fctxs[f].hits;
            dctxs[f].count = fctxs[f].hit_count;
            dctxs[f].file_num = f;
            wallet_init(&dctxs[f].tw);
            dctxs[f].tw.sapling_keys = w->sapling_keys;
            dctxs[f].result_cap = 64;
            dctxs[f].results = zcl_malloc(64 * sizeof(struct db_sapling_note), "sapling decrypt results");
            if (!dctxs[f].results) {
                fprintf(stderr,
                        "legacy_import: failed to allocate sapling decrypt results\n");
                goto cleanup;
            }
        }

        pthread_t thr3[8];
        for (int base = 0; base < num_files; base += batch) {
            int n = num_files - base;
            if (n > batch) n = batch;
            int launched = 0;
            for (int i = 0; i < n; i++) {
                if (dctxs[base + i].count == 0) continue;
                if (pthread_create(&thr3[launched], NULL,
                                   decrypt_thread,
                                   &dctxs[base + i]) != 0) {
                    fprintf(stderr,
                            "legacy_import: failed to start sapling decrypt thread\n");
                    for (int j = 0; j < launched; j++)
                        pthread_join(thr3[j], NULL);
                    goto cleanup;
                }
                launched++;
            }
            for (int i = 0; i < launched; i++)
                pthread_join(thr3[i], NULL);
        }

        /* Merge results and write to SQLite. */
        int shielded_outputs_seen = 0;
        int sapling_notes = 0;
        {
            bool sapling_tx_open = false;
            if (!legacy_import_begin_checked(ndb, "pass 3 commit begin")) {
                goto cleanup;
            }
            sapling_tx_open = true;
            for (int f = 0; f < num_files; f++) {
                shielded_outputs_seen += dctxs[f].outputs_seen;
                sapling_notes += dctxs[f].notes_found;
                for (int i = 0; i < dctxs[f].result_count; i++) {
                    if (!db_sapling_note_save(ndb, &dctxs[f].results[i])) {
                        fprintf(stderr,
                                "legacy_import: pass 3 sapling note save failed\n");
                        if (sapling_tx_open &&
                            !legacy_import_rollback_checked(ndb,
                                "pass 3 rollback")) {
                            fprintf(stderr,
                                    "legacy_import: pass 3 rollback failed after DB error\n");
                        }
                        goto cleanup;
                    }
                }
                free(dctxs[f].results);
                free(fctxs[f].hits);
                free(dctxs[f].tw.sapling_notes);
            }
            if (!legacy_import_commit_checked(ndb, "pass 3 commit")) {
                sapling_tx_open = false;
                goto cleanup;
            }
            sapling_tx_open = false;
        }

        z_found = sapling_notes;
        clock_gettime(CLOCK_MONOTONIC, &ts_p3);
        double p3_ms = (double)(ts_p3.tv_sec - ts_p2.tv_sec) * 1000.0 +
                       (double)(ts_p3.tv_nsec - ts_p2.tv_nsec) / 1e6;
        printf("legacy_import: pass 3 done in %.1f ms — %d blocks, "
               "%d candidates, %d shielded outputs, %d notes\n",
               p3_ms, total_blocks, total_candidates,
               shielded_outputs_seen, z_found);
        fflush(stdout);

        free(dctxs);
        dctxs = NULL;
        free(fctxs);
        fctxs = NULL;
    }

    /* Sync wallet keys to SQLite. */
    node_db_sync_wallet_keys(ndb, w);

    /* Report final results. */
    int64_t t_bal = db_wallet_utxo_balance(ndb);
    int64_t z_bal = db_sapling_note_balance(ndb);
    int64_t total = t_bal + z_bal;

    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                     (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    printf("legacy_import: COMPLETE in %.1f seconds\n", elapsed);
    printf("  transparent: %.8f ZCL (%d UTXOs, %d txs)\n",
           (double)t_bal / 1e8, unspent, wl.count);
    printf("  shielded:    %.8f ZCL (%d notes)\n",
           (double)z_bal / 1e8, z_found);
    printf("  total:       %.8f ZCL\n", (double)total / 1e8);
    fflush(stdout);

    ret = wl.count;

cleanup:
    if (dctxs) {
        for (int f = 0; f < num_files; f++) {
            free(dctxs[f].results);
            free(dctxs[f].tw.sapling_notes);
        }
        free(dctxs);
    }
    if (fctxs) {
        for (int f = 0; f < num_files; f++)
            free(fctxs[f].hits);
        free(fctxs);
    }

    /* Cleanup. */
    aht_free(&aht);
    uset_free(&uset);
    wl_free(&wl);
    free(file_has_match);

    return ret;
}
