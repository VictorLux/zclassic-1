/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * rebuild_recent — read a LIVE zclassicd data directory and rebuild N
 * blocks (or the WHOLE chain) into zclassic23's native event-log format
 * at high speed, using io_uring for the write path. Times the rebuild.
 *
 * Why this is live-safe: past blocks are immutable and physically present
 * in blk*.dat (mmap-readable while zclassicd runs). Only the blocks/index/
 * LevelDB is locked, so we snapshot-copy just the index and read bodies
 * from the live, immutable .dat files. zclassicd never stops.
 *
 * Why io_uring: the first cut proved the rebuild was *fsync-bound* — the
 * durable event_log appender fsync()s twice per event. For a bulk rebuild
 * we don't need per-event durability, so we emit the SAME canonical wire
 * format into large buffers, stream them to disk via io_uring (8 buffers
 * in flight, overlapping serialization with I/O), and fsync ONCE at the
 * end. The output is a byte-identical native event log: openable by
 * event_log_open and consumable by every projection.
 *
 * Usage:
 *   rebuild_recent [datadir] [N|all] [out_path]
 *     datadir   default $HOME/.zclassic
 *     N         number of most-recent blocks, or "all"/"0" = whole chain
 *     out_path  default ./rebuild_recent.evlog (removed unless you pass one)
 */

#define _GNU_SOURCE 1

#include "platform/time_compat.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "util/safe_alloc.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "core/serialize.h"
#include "core/uint256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <nmmintrin.h>   /* SSE4.2 hardware CRC-32C */

/* Provided by main.c in the node; standalone tools define their own. */
volatile sig_atomic_t g_shutdown_requested = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── crc32c (Castagnoli) — CRC-32C value identical to event_log.c ──────
 * Software table is the reference; the SSE4.2 hardware path (this CPU has
 * it) is ~10-20x faster and is verified to match the table at startup. */
static uint32_t g_crc_tab[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0x82F63B78u & -(c & 1u));
        g_crc_tab[i] = c;
    }
}
static uint32_t crc32c_sw(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ g_crc_tab[(crc ^ p[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}
/* Hardware CRC-32C via SSE4.2 (8 bytes/step). Same polynomial + reflection
 * + 0xFFFFFFFF init/final-xor as the table, so values are identical. */
static uint32_t crc32c(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len >= 8) { uint64_t v; memcpy(&v, p, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, v); p += 8; len -= 8; }
    while (len--) crc = _mm_crc32_u8(crc, *p++);
    return crc ^ 0xFFFFFFFFu;
}
/* Abort early if HW and SW disagree — guarantees byte-valid output. */
static void crc_selfcheck(void)
{
    uint8_t buf[1031];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31u + 7u);
    for (size_t n = 0; n <= sizeof buf; n += (n < 16 ? 1 : 97)) {
        if (crc32c(buf, n) != crc32c_sw(buf, n)) {
            fprintf(stderr, "rebuild_recent: HW crc32c mismatch at n=%zu — abort\n", n);
            exit(2);
        }
    }
}
static void put_u32_le(uint8_t *d, uint32_t v) { for (int i=0;i<4;i++) d[i]=(uint8_t)(v>>(8*i)); }
static void put_u64_le(uint8_t *d, uint64_t v) { for (int i=0;i<8;i++) d[i]=(uint8_t)(v>>(8*i)); }

/* ── raw io_uring bulk writer ──────────────────────────────────────────
 * Canonical event framing (matches event_log wire format):
 *   [16B header: len|type|flags=0|crc32c(payload)] [payload] [16B sentinel]
 * Buffers stream to disk via IORING_OP_WRITE; one fsync at finish. */

#define UW_NBUF 8
#define UW_CAP  (4u * 1024u * 1024u)   /* per-buffer staging; > max block */
#define UW_QD   16

static int io_uring_setup_(unsigned e, struct io_uring_params *p)
{ return (int)syscall(__NR_io_uring_setup, e, p); }
static int io_uring_enter_(int fd, unsigned ts, unsigned mc, unsigned fl)
{ return (int)syscall(__NR_io_uring_enter, fd, ts, mc, fl, NULL, 0); }

struct uw {
    int ring_fd, out_fd;
    /* SQ ring */
    uint32_t *sq_tail, *sq_mask, *sq_array;
    struct io_uring_sqe *sqes;
    /* CQ ring */
    uint32_t *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
    void *sq_ptr, *cq_ptr; size_t sq_sz, cq_sz, sqe_sz;

    uint8_t *buf[UW_NBUF];
    uint32_t blen[UW_NBUF];     /* bytes queued in an in-flight buffer */
    uint64_t boff[UW_NBUF];     /* file offset of an in-flight buffer  */
    bool     inflight[UW_NBUF];

    int cur;                    /* buffer currently being filled */
    uint32_t cursor;            /* bytes filled in cur */
    uint64_t base_off;          /* absolute file offset of cur's start */
    uint64_t total;             /* total bytes written */
    uint64_t events;
    int short_writes;
};

static bool full_pwrite(int fd, const uint8_t *b, size_t n, uint64_t off)
{
    while (n) { ssize_t w = pwrite(fd, b, n, (off_t)off);
        if (w < 0) { if (errno==EINTR) continue; return false; }
        b += w; n -= (size_t)w; off += (uint64_t)w; }
    return true;
}

static void uw_reap(struct uw *u, bool block)
{
    if (block) (void)io_uring_enter_(u->ring_fd, 0, 1, IORING_ENTER_GETEVENTS);
    uint32_t head = __atomic_load_n(u->cq_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE);
    while (head != tail) {
        struct io_uring_cqe *c = &u->cqes[head & *u->cq_mask];
        int idx = (int)c->user_data;
        if (c->res < 0) {
            fprintf(stderr, "rebuild_recent: uring write err idx=%d res=%d\n", idx, c->res);
        } else if ((uint32_t)c->res != u->blen[idx]) {
            /* short write — finish the remainder synchronously */
            uint32_t done = (uint32_t)c->res;
            full_pwrite(u->out_fd, u->buf[idx] + done, u->blen[idx] - done, u->boff[idx] + done);
            u->short_writes++;
        }
        u->inflight[idx] = false;
        head++;
    }
    __atomic_store_n(u->cq_head, head, __ATOMIC_RELEASE);
}

static int uw_get_free(struct uw *u)
{
    for (;;) {
        uw_reap(u, false);
        for (int i = 0; i < UW_NBUF; i++) if (!u->inflight[i]) return i;
        uw_reap(u, true);   /* all busy: block for at least one completion */
    }
}

static bool uw_submit(struct uw *u, int idx, uint32_t len, uint64_t off)
{
    uint32_t tail = *u->sq_tail;
    uint32_t i = tail & *u->sq_mask;
    struct io_uring_sqe *s = &u->sqes[i];
    memset(s, 0, sizeof *s);
    s->opcode = IORING_OP_WRITE;
    s->fd = u->out_fd;
    s->addr = (uint64_t)(uintptr_t)u->buf[idx];
    s->len = len;
    s->off = off;
    s->user_data = (uint64_t)idx;
    u->sq_array[i] = i;
    u->blen[idx] = len; u->boff[idx] = off; u->inflight[idx] = true;
    __atomic_store_n(u->sq_tail, tail + 1, __ATOMIC_RELEASE);
    int r = io_uring_enter_(u->ring_fd, 1, 0, 0);
    if (r < 0) { fprintf(stderr, "rebuild_recent: io_uring_enter: %s\n", strerror(errno)); return false; }
    return true;
}

/* flush the current buffer (if non-empty) and grab a fresh one */
static bool uw_flush(struct uw *u)
{
    if (u->cursor == 0) return true;
    if (!uw_submit(u, u->cur, u->cursor, u->base_off)) return false;
    u->base_off += u->cursor;
    u->total    += u->cursor;
    u->cur = uw_get_free(u);
    u->cursor = 0;
    return true;
}

/* append one framed event; copies payload into the staging buffer */
static bool uw_append(struct uw *u, uint32_t type, const void *payload, uint32_t plen)
{
    uint32_t evsize = 16u + plen + 16u;
    if (evsize > UW_CAP) { fprintf(stderr, "rebuild_recent: event too big (%u)\n", evsize); return false; }
    if (u->cursor + evsize > UW_CAP && !uw_flush(u)) return false;

    uint8_t *p = u->buf[u->cur] + u->cursor;
    uint64_t ev_start = u->base_off + u->cursor;
    put_u32_le(p + 0, plen);
    put_u32_le(p + 4, type);
    put_u32_le(p + 8, 0u);
    put_u32_le(p + 12, crc32c(payload, plen));
    if (plen) memcpy(p + 16, payload, plen);
    put_u64_le(p + 16 + plen, EVENT_LOG_SENTINEL_MAGIC);
    put_u64_le(p + 16 + plen + 8, ev_start);
    u->cursor += evsize;
    u->events++;
    return true;
}

static bool uw_setup(struct uw *u, const char *out_path)
{
    memset(u, 0, sizeof *u);
    u->out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (u->out_fd < 0) { perror("open(out)"); return false; }

    struct io_uring_params p; memset(&p, 0, sizeof p);
    u->ring_fd = io_uring_setup_(UW_QD, &p);
    if (u->ring_fd < 0) { perror("io_uring_setup"); return false; }

    u->sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    u->cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    u->sqe_sz = p.sq_entries * sizeof(struct io_uring_sqe);

    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        size_t sz = u->sq_sz > u->cq_sz ? u->sq_sz : u->cq_sz;
        u->sq_sz = u->cq_sz = sz;
    }
    u->sq_ptr = mmap(0, u->sq_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, u->ring_fd, IORING_OFF_SQ_RING);
    if (u->sq_ptr == MAP_FAILED) { perror("mmap sq"); return false; }
    u->cq_ptr = (p.features & IORING_FEAT_SINGLE_MMAP) ? u->sq_ptr
              : mmap(0, u->cq_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, u->ring_fd, IORING_OFF_CQ_RING);
    if (u->cq_ptr == MAP_FAILED) { perror("mmap cq"); return false; }
    u->sqes = mmap(0, u->sqe_sz, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, u->ring_fd, IORING_OFF_SQES);
    if (u->sqes == MAP_FAILED) { perror("mmap sqes"); return false; }

    u->sq_tail  = (uint32_t*)((char*)u->sq_ptr + p.sq_off.tail);
    u->sq_mask  = (uint32_t*)((char*)u->sq_ptr + p.sq_off.ring_mask);
    u->sq_array = (uint32_t*)((char*)u->sq_ptr + p.sq_off.array);
    u->cq_head  = (uint32_t*)((char*)u->cq_ptr + p.cq_off.head);
    u->cq_tail  = (uint32_t*)((char*)u->cq_ptr + p.cq_off.tail);
    u->cq_mask  = (uint32_t*)((char*)u->cq_ptr + p.cq_off.ring_mask);
    u->cqes     = (struct io_uring_cqe*)((char*)u->cq_ptr + p.cq_off.cqes);

    for (int i = 0; i < UW_NBUF; i++) {
        if (posix_memalign((void**)&u->buf[i], 4096, UW_CAP) != 0) { perror("posix_memalign"); return false; }
        u->inflight[i] = false;
    }
    u->cur = 0; u->cursor = 0; u->base_off = 0;
    return true;
}

/* submit last partial buffer, drain everything, single fsync */
static bool uw_finish(struct uw *u)
{
    if (!uw_flush(u)) return false;
    for (int i = 0; i < UW_NBUF; i++) {
        while (u->inflight[i]) uw_reap(u, true);
    }
    if (fsync(u->out_fd) < 0) { perror("fsync(out)"); return false; }
    return true;
}

static void uw_close(struct uw *u)
{
    for (int i = 0; i < UW_NBUF; i++) free(u->buf[i]);
    if (u->sqes && u->sqes != MAP_FAILED) munmap(u->sqes, u->sqe_sz);
    if (u->cq_ptr && u->cq_ptr != u->sq_ptr && u->cq_ptr != MAP_FAILED) munmap(u->cq_ptr, u->cq_sz);
    if (u->sq_ptr && u->sq_ptr != MAP_FAILED) munmap(u->sq_ptr, u->sq_sz);
    if (u->ring_fd > 0) close(u->ring_fd);
    if (u->out_fd > 0) close(u->out_fd);
}

/* ── live snapshot of the (locked) index dir ───────────────────────────── */
static char *snapshot_dir(const char *src)
{
    char tmpl[] = "/tmp/zcl_idx_XXXXXX";
    char *dst = mkdtemp(tmpl);
    if (!dst) { perror("mkdtemp"); return NULL; }
    char *out = strdup(dst);
    char cmd[4096];
    int n = snprintf(cmd, sizeof cmd, "cp -a '%s/.' '%s/'", src, out);
    if (n <= 0 || (size_t)n >= sizeof cmd || system(cmd) != 0) {
        fprintf(stderr, "rebuild_recent: snapshot copy failed\n"); free(out); return NULL;
    }
    return out;
}

int main(int argc, char **argv)
{
    crc_init();
    crc_selfcheck();
    const char *home = getenv("HOME");
    char default_dd[2048];
    snprintf(default_dd, sizeof default_dd, "%s/.zclassic", home ? home : ".");
    const char *datadir = (argc > 1) ? argv[1] : default_dd;

    bool whole = false;
    int N = 10;
    if (argc > 2) {
        if (!strcmp(argv[2], "all") || !strcmp(argv[2], "0")) whole = true;
        else { N = atoi(argv[2]); if (N <= 0) N = 10; }
    }
    const char *out_path = (argc > 3) ? argv[3] : "rebuild_recent.evlog";
    bool out_is_temp = (argc <= 3);

    char idx_src[2200], blocks_dir[2200];
    snprintf(idx_src, sizeof idx_src, "%s/blocks/index", datadir);
    snprintf(blocks_dir, sizeof blocks_dir, "%s/blocks", datadir);
    printf("rebuild_recent: datadir=%s mode=%s out=%s\n",
           datadir, whole ? "WHOLE-CHAIN" : "last-N", out_path);

    int64_t t0 = now_ms();
    char *idx_snap = snapshot_dir(idx_src);
    if (!idx_snap) return 1;
    int64_t t_snap = now_ms() - t0;

    int64_t t1 = now_ms();
    struct bilr *bilr = NULL;
    if (!bilr_open(idx_snap, &bilr)) { fprintf(stderr, "bilr_open failed\n"); return 1; }
    struct legacy_block_loc *map = NULL; size_t map_count = 0;
    if (!bilr_load_height_map(bilr, &map, &map_count) || map_count == 0) {
        fprintf(stderr, "load_height_map failed\n"); return 1;
    }
    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(blocks_dir, &bmr)) { fprintf(stderr, "bmr_open failed\n"); return 1; }

    int64_t tip = -1;
    for (size_t h = map_count; h-- > 0; ) if (map[h].height >= 0) { tip = (int64_t)h; break; }
    if (tip < 0) { fprintf(stderr, "empty index\n"); return 1; }
    int64_t lo = whole ? 0 : (tip - N + 1 < 0 ? 0 : tip - N + 1);
    int64_t t_load = now_ms() - t1;

    struct uw uw;
    if (!uw_setup(&uw, out_path)) return 1;

    uint64_t n_blocks = 0, n_tx = 0, n_add = 0, n_spend = 0;
    static uint8_t hdr_buf[EV_BLOCK_HEADER_FIXED_BYTES + EV_BLOCK_HEADER_MAX_SOLUTION];
    static uint8_t add_buf[EV_UTXO_ADD_HDR_WIRE_LEN + MAX_SCRIPT_SIZE];
    uint8_t spend_buf[EV_UTXO_SPEND_WIRE_LEN];
    bool ok = true;
    int64_t t_prog = now_ms();

    int64_t t2 = now_ms();
    for (int64_t h = lo; h <= tip && ok; h++) {
        const struct legacy_block_loc *loc = &map[h];
        if (loc->height < 0) continue;

        size_t plen = 0;
        const uint8_t *payload = bmr_get_payload(bmr, loc->nFile, loc->nDataPos, &plen);
        if (!payload || plen == 0) { fprintf(stderr, "h=%lld mmap fail\n", (long long)h); ok=false; break; }

        struct byte_stream s; stream_init_from_data(&s, payload, plen);
        struct block blk; block_init(&blk);
        bool dok = block_deserialize(&blk, &s); stream_free(&s);
        if (!dok) { fprintf(stderr, "h=%lld deser fail\n", (long long)h); block_free(&blk); ok=false; break; }

        struct uint256 bhash; block_get_hash(&blk, &bhash);
        struct ev_block_header eh; memset(&eh, 0, sizeof eh);
        memcpy(eh.hash, bhash.data, 32);
        memcpy(eh.hashPrev, blk.header.hashPrevBlock.data, 32);
        eh.height=loc->height; eh.nStatus=loc->nStatus; eh.nFile=loc->nFile;
        eh.nDataPos=loc->nDataPos; eh.nUndoPos=loc->nUndoPos;
        eh.nTime=blk.header.nTime; eh.nBits=blk.header.nBits;
        memcpy(eh.nNonce, blk.header.nNonce.data, 32);
        memcpy(eh.hashMerkleRoot, blk.header.hashMerkleRoot.data, 32);
        memcpy(eh.hashFinalSaplingRoot, blk.header.hashFinalSaplingRoot.data, 32);
        eh.nVersion=blk.header.nVersion; eh.nTx=(uint32_t)blk.num_vtx;
        eh.nSolutionSize=(uint16_t)blk.header.nSolutionSize;
        size_t hw = 0;
        if (!ev_block_header_serialize(&eh, blk.header.nSolution, hdr_buf, sizeof hdr_buf, &hw) ||
            !uw_append(&uw, EV_BLOCK_HEADER, hdr_buf, (uint32_t)hw) ||
            !uw_append(&uw, EV_BLOCK_BODY, payload, (uint32_t)plen)) {
            block_free(&blk); ok=false; break;
        }

        for (size_t ti = 0; ti < blk.num_vtx && ok; ti++) {
            struct transaction *tx = &blk.vtx[ti];
            transaction_compute_hash(tx);
            bool cb = transaction_is_coinbase(tx);
            for (size_t vo = 0; vo < tx->num_vout && ok; vo++) {
                const struct tx_out *o = &tx->vout[vo];
                struct ev_utxo_add_hdr ah; memset(&ah, 0, sizeof ah);
                memcpy(ah.txid, tx->hash.data, 32);
                ah.vout=(uint32_t)vo; ah.value=o->value; ah.height=(uint32_t)loc->height;
                ah.is_coinbase=cb?1u:0u; ah.script_len=(uint32_t)o->script_pub_key.size;
                size_t aw=0;
                if (!ev_utxo_add_serialize(&ah, o->script_pub_key.data, add_buf, sizeof add_buf, &aw) ||
                    !uw_append(&uw, EV_UTXO_ADD, add_buf, (uint32_t)aw)) { ok=false; break; }
                n_add++;
            }
            if (!cb) for (size_t vi = 0; vi < tx->num_vin && ok; vi++) {
                struct ev_utxo_spend sp;
                memcpy(sp.txid, tx->vin[vi].prevout.hash.data, 32); sp.vout=tx->vin[vi].prevout.n;
                if (!ev_utxo_spend_serialize(&sp, spend_buf) ||
                    !uw_append(&uw, EV_UTXO_SPEND, spend_buf, EV_UTXO_SPEND_WIRE_LEN)) { ok=false; break; }
                n_spend++;
            }
            n_tx++;
        }
        block_free(&blk);
        n_blocks++;

        if (whole && (n_blocks % 100000 == 0)) {
            int64_t el = now_ms() - t2;
            fprintf(stderr, "  ... %llu blocks  %.0f blk/s  %.1f GB\n",
                    (unsigned long long)n_blocks,
                    el>0 ? (double)n_blocks*1000.0/(double)el : 0.0,
                    (double)uw.total / 1e9);
            (void)t_prog;
        }
    }

    if (ok && !uw_finish(&uw)) ok = false;
    int64_t t_rebuild = now_ms() - t2;
    uint64_t bytes = uw.total;

    double secs = (double)t_rebuild / 1000.0;
    printf("\n=== rebuild_recent (io_uring): %s ===\n", ok ? "OK" : "ABORTED");
    printf("  source tip            : %lld\n", (long long)tip);
    printf("  blocks rebuilt        : %llu (heights %lld..%lld)\n",
           (unsigned long long)n_blocks, (long long)lo, (long long)tip);
    printf("  transactions          : %llu\n", (unsigned long long)n_tx);
    printf("  UTXO adds / spends    : %llu / %llu\n",
           (unsigned long long)n_add, (unsigned long long)n_spend);
    printf("  events written        : %llu  (short_writes=%d)\n",
           (unsigned long long)uw.events, uw.short_writes);
    printf("  native bytes written  : %llu (%.2f GB)\n",
           (unsigned long long)bytes, (double)bytes / 1e9);
    printf("\n  setup  : snapshot=%lld ms   index-load=%lld ms\n",
           (long long)t_snap, (long long)t_load);
    printf("  REBUILD: %.2f s  (%.0f blk/s, %.0f tx/s, %.0f MB/s, %.2f GB/s)\n",
           secs,
           secs>0 ? (double)n_blocks/secs : 0.0,
           secs>0 ? (double)n_tx/secs : 0.0,
           secs>0 ? (double)bytes/1e6/secs : 0.0,
           secs>0 ? (double)bytes/1e9/secs : 0.0);

    uw_close(&uw);
    bmr_close(bmr);
    bilr_free_height_map(map);
    bilr_close(bilr);
    char rmcmd[2300];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", idx_snap);
    if (system(rmcmd) != 0) fprintf(stderr, "warn: snapshot cleanup failed\n");
    free(idx_snap);
    if (out_is_temp) unlink(out_path);
    return ok ? 0 : 1;
}
