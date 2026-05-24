/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * rebuild_recent — read a LIVE zclassicd data directory and rebuild the
 * last N blocks into zclassic23's native event-log format, timing the
 * rebuild.
 *
 * Why this works against a *live* node:
 *   - Past block data is immutable and physically present in blk*.dat.
 *     Those files are append-only; mmap-reading them never contends with
 *     a running zclassicd.
 *   - The only exclusive resource is the blocks/index/ LevelDB (it holds
 *     a LOCK). We take a point-in-time *snapshot copy* of just the index
 *     (small relative to block data) and open the copy, so zclassicd is
 *     never stopped. Because the data is immutable, the last-N entries in
 *     the snapshot are correct even if the tip advances afterward.
 *
 * Phases (each timed separately so the rebuild number is clean):
 *   1. snapshot   — copy blocks/index/ to a temp dir
 *   2. index load — open the copy, build the height map, pick last N
 *   3. REBUILD    — mmap bodies, deserialize, emit native events  ← the metric
 *
 * Native events emitted per block: EV_BLOCK_HEADER + EV_BLOCK_BODY, then
 * per transaction EV_UTXO_ADD (each output) and EV_UTXO_SPEND (each
 * non-coinbase input) — i.e. everything needed to represent those blocks
 * and their UTXO-set delta in our format.
 *
 * Usage:
 *   rebuild_recent [zclassicd_datadir] [N] [out_event_log]
 *     datadir        default $HOME/.zclassic
 *     N              default 10
 *     out_event_log  default a fresh temp file (removed on exit)
 */

#include "platform/time_compat.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
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

/* Provided by main.c in the node; standalone tools define their own. */
volatile sig_atomic_t g_shutdown_requested = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Snapshot a directory's contents into a fresh temp dir. Returns the
 * temp path (caller frees) or NULL. Uses `cp -a SRC/. DST` — fast for the
 * LevelDB index and gives a consistent point-in-time view of immutable
 * SST files. */
static char *snapshot_dir(const char *src)
{
    char tmpl[] = "/tmp/zcl_idx_XXXXXX";
    char *dst = mkdtemp(tmpl);
    if (!dst) { perror("mkdtemp"); return NULL; }
    char *out = strdup(dst);
    if (!out) return NULL;

    char cmd[4096];
    int n = snprintf(cmd, sizeof cmd, "cp -a '%s/.' '%s/'", src, out);
    if (n <= 0 || (size_t)n >= sizeof cmd) { free(out); return NULL; }
    if (system(cmd) != 0) {
        fprintf(stderr, "rebuild_recent: snapshot copy failed: %s\n", cmd);
        free(out);
        return NULL;
    }
    return out;
}

int main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    char default_dd[2048];
    snprintf(default_dd, sizeof default_dd, "%s/.zclassic", home ? home : ".");

    const char *datadir = (argc > 1) ? argv[1] : default_dd;
    int N = (argc > 2) ? atoi(argv[2]) : 10;
    if (N <= 0) N = 10;

    char out_path_buf[] = "/tmp/zcl_rebuild_XXXXXX";
    const char *out_path;
    bool out_is_temp = false;
    if (argc > 3) {
        out_path = argv[3];
    } else {
        int fd = mkstemp(out_path_buf);
        if (fd < 0) { perror("mkstemp"); return 1; }
        close(fd);
        unlink(out_path_buf);          /* event_log_open creates it fresh */
        out_path = out_path_buf;
        out_is_temp = true;
    }

    char idx_src[2200], blocks_dir[2200];
    snprintf(idx_src, sizeof idx_src, "%s/blocks/index", datadir);
    snprintf(blocks_dir, sizeof blocks_dir, "%s/blocks", datadir);

    printf("rebuild_recent: datadir=%s N=%d out=%s\n", datadir, N, out_path);

    /* ── Phase 1: live snapshot of the index ────────────────────── */
    int64_t t0 = now_ms();
    char *idx_snap = snapshot_dir(idx_src);
    if (!idx_snap) {
        fprintf(stderr, "rebuild_recent: cannot snapshot %s\n", idx_src);
        return 1;
    }
    int64_t t_snap = now_ms() - t0;

    /* ── Phase 2: open index copy + block files, pick last N ───── */
    int64_t t1 = now_ms();
    struct bilr *bilr = NULL;
    if (!bilr_open(idx_snap, &bilr)) {
        fprintf(stderr, "rebuild_recent: bilr_open(%s) failed\n", idx_snap);
        return 1;
    }
    struct legacy_block_loc *map = NULL;
    size_t map_count = 0;
    if (!bilr_load_height_map(bilr, &map, &map_count) || map_count == 0) {
        fprintf(stderr, "rebuild_recent: load_height_map failed\n");
        return 1;
    }
    struct blocks_mmap *bmr = NULL;
    if (!bmr_open(blocks_dir, &bmr)) {
        fprintf(stderr, "rebuild_recent: bmr_open(%s) failed\n", blocks_dir);
        return 1;
    }

    /* Highest populated height, then walk down collecting N real blocks. */
    int64_t tip = -1;
    for (size_t h = map_count; h-- > 0; ) {
        if (map[h].height >= 0) { tip = (int64_t)h; break; }
    }
    if (tip < 0) { fprintf(stderr, "rebuild_recent: empty index\n"); return 1; }

    int *heights = malloc((size_t)N * sizeof *heights);
    int collected = 0;
    for (int64_t h = tip; h >= 0 && collected < N; h--) {
        if (map[h].height >= 0) heights[collected++] = (int)h;
    }
    /* process ascending (oldest of the window first) */
    int64_t t_load = now_ms() - t1;

    event_log_t *log = event_log_open(out_path);
    if (!log) { fprintf(stderr, "rebuild_recent: event_log_open failed\n"); return 1; }
    uint64_t bytes_before = event_log_size(log);

    /* ── Phase 3: THE REBUILD (timed) ───────────────────────────── */
    uint64_t n_blocks = 0, n_tx = 0, n_add = 0, n_spend = 0;
    static uint8_t hdr_buf[EV_BLOCK_HEADER_FIXED_BYTES + EV_BLOCK_HEADER_MAX_SOLUTION];
    static uint8_t add_buf[EV_UTXO_ADD_HDR_WIRE_LEN + MAX_SCRIPT_SIZE];
    uint8_t spend_buf[EV_UTXO_SPEND_WIRE_LEN];
    bool ok = true;

    int64_t t2 = now_ms();
    for (int i = collected - 1; i >= 0 && ok; i--) {
        const struct legacy_block_loc *loc = &map[heights[i]];

        size_t plen = 0;
        const uint8_t *payload = bmr_get_payload(bmr, loc->nFile, loc->nDataPos, &plen);
        if (!payload || plen == 0) {
            fprintf(stderr, "rebuild_recent: h=%d mmap fetch failed\n", loc->height);
            ok = false; break;
        }

        struct byte_stream s;
        stream_init_from_data(&s, payload, plen);
        struct block blk;
        block_init(&blk);
        bool deser_ok = block_deserialize(&blk, &s);
        stream_free(&s);
        if (!deser_ok) {
            fprintf(stderr, "rebuild_recent: h=%d deserialize failed\n", loc->height);
            block_free(&blk); ok = false; break;
        }

        struct uint256 bhash;
        block_get_hash(&blk, &bhash);

        /* EV_BLOCK_HEADER */
        struct ev_block_header eh;
        memset(&eh, 0, sizeof eh);
        memcpy(eh.hash, bhash.data, 32);
        memcpy(eh.hashPrev, blk.header.hashPrevBlock.data, 32);
        eh.height   = loc->height;
        eh.nStatus  = loc->nStatus;
        eh.nFile    = loc->nFile;
        eh.nDataPos = loc->nDataPos;
        eh.nUndoPos = loc->nUndoPos;
        eh.nTime    = blk.header.nTime;
        eh.nBits    = blk.header.nBits;
        memcpy(eh.nNonce, blk.header.nNonce.data, 32);
        memcpy(eh.hashMerkleRoot, blk.header.hashMerkleRoot.data, 32);
        memcpy(eh.hashFinalSaplingRoot, blk.header.hashFinalSaplingRoot.data, 32);
        eh.nVersion = blk.header.nVersion;
        eh.nTx = (uint32_t)blk.num_vtx;
        eh.nSolutionSize = (uint16_t)blk.header.nSolutionSize;
        size_t hw = 0;
        if (!ev_block_header_serialize(&eh, blk.header.nSolution,
                                       hdr_buf, sizeof hdr_buf, &hw) ||
            event_log_append(log, EV_BLOCK_HEADER, hdr_buf, hw) == UINT64_MAX) {
            fprintf(stderr, "rebuild_recent: h=%d header append failed\n", loc->height);
            block_free(&blk); ok = false; break;
        }

        /* EV_BLOCK_BODY — the raw immutable block bytes */
        if (event_log_append(log, EV_BLOCK_BODY, payload, plen) == UINT64_MAX) {
            fprintf(stderr, "rebuild_recent: h=%d body append failed\n", loc->height);
            block_free(&blk); ok = false; break;
        }

        /* Per-tx UTXO deltas */
        for (size_t ti = 0; ti < blk.num_vtx && ok; ti++) {
            struct transaction *tx = &blk.vtx[ti];
            transaction_compute_hash(tx);
            bool coinbase = transaction_is_coinbase(tx);

            for (size_t vo = 0; vo < tx->num_vout && ok; vo++) {
                const struct tx_out *o = &tx->vout[vo];
                struct ev_utxo_add_hdr ah;
                memset(&ah, 0, sizeof ah);
                memcpy(ah.txid, tx->hash.data, 32);
                ah.vout = (uint32_t)vo;
                ah.value = o->value;
                ah.height = (uint32_t)loc->height;
                ah.is_coinbase = coinbase ? 1u : 0u;
                ah.script_len = (uint32_t)o->script_pub_key.size;
                size_t aw = 0;
                if (!ev_utxo_add_serialize(&ah, o->script_pub_key.data,
                                           add_buf, sizeof add_buf, &aw) ||
                    event_log_append(log, EV_UTXO_ADD, add_buf, aw) == UINT64_MAX) {
                    fprintf(stderr, "rebuild_recent: utxo_add append failed\n");
                    ok = false; break;
                }
                n_add++;
            }
            if (!coinbase) {
                for (size_t vi = 0; vi < tx->num_vin && ok; vi++) {
                    struct ev_utxo_spend sp;
                    memcpy(sp.txid, tx->vin[vi].prevout.hash.data, 32);
                    sp.vout = tx->vin[vi].prevout.n;
                    if (!ev_utxo_spend_serialize(&sp, spend_buf) ||
                        event_log_append(log, EV_UTXO_SPEND, spend_buf,
                                         EV_UTXO_SPEND_WIRE_LEN) == UINT64_MAX) {
                        fprintf(stderr, "rebuild_recent: utxo_spend append failed\n");
                        ok = false; break;
                    }
                    n_spend++;
                }
            }
            n_tx++;
        }

        block_free(&blk);
        n_blocks++;
    }
    int64_t t_rebuild = now_ms() - t2;
    uint64_t bytes_written = event_log_size(log) - bytes_before;

    /* ── Report ─────────────────────────────────────────────────── */
    double secs = (double)t_rebuild / 1000.0;
    printf("\n=== rebuild_recent: %s ===\n", ok ? "OK" : "ABORTED");
    printf("  source tip height     : %lld\n", (long long)tip);
    printf("  blocks rebuilt        : %llu (heights %d..%d)\n",
           (unsigned long long)n_blocks,
           collected ? heights[collected - 1] : 0,
           collected ? heights[0] : 0);
    printf("  transactions          : %llu\n", (unsigned long long)n_tx);
    printf("  UTXO adds / spends    : %llu / %llu\n",
           (unsigned long long)n_add, (unsigned long long)n_spend);
    printf("  native bytes written  : %llu (%.2f MB)\n",
           (unsigned long long)bytes_written,
           (double)bytes_written / (1024.0 * 1024.0));
    printf("\n  setup  : snapshot=%lld ms   index-load=%lld ms\n",
           (long long)t_snap, (long long)t_load);
    printf("  REBUILD: %lld ms  (%.0f blocks/s, %.1f tx/s, %.1f MB/s)\n",
           (long long)t_rebuild,
           secs > 0 ? (double)n_blocks / secs : 0.0,
           secs > 0 ? (double)n_tx / secs : 0.0,
           secs > 0 ? (double)bytes_written / (1024.0 * 1024.0) / secs : 0.0);

    /* ── Cleanup ────────────────────────────────────────────────── */
    event_log_close(log);
    bmr_close(bmr);
    bilr_free_height_map(map);
    bilr_close(bilr);
    free(heights);

    char rmcmd[2300];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", idx_snap);
    if (system(rmcmd) != 0) fprintf(stderr, "rebuild_recent: warn: snapshot cleanup failed\n");
    free(idx_snap);
    if (out_is_temp) unlink(out_path);

    return ok ? 0 : 1;
}
