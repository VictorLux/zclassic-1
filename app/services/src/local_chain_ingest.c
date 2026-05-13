/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * local_chain_ingest — see services/local_chain_ingest.h for the
 * three-phase pipeline contract (SHA3 windows, chainstate import,
 * per-block advance) and the security model.
 *
 * Implementation strategy: this module is a thin orchestrator on top
 * of existing primitives.  Every heavy lift (LevelDB walk, atomic
 * UTXO batch write, per-block apply) lives in another module already
 * proven by tests + runtime; we glue them together against the
 * static SHA3 anchors and report progress through lib/health.
 *
 * Reentrant-safe: a single ingest run at a time per process.  The
 * dump-state path uses atomics so concurrent zcl_state callers see
 * consistent snapshots without taking the runner's locks.
 */

#include "services/local_chain_ingest.h"

#include "chain/chain.h"
#include "chain/checkpoints.h"
#include "chain/sha3_windows.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "core/uint256.h"
#include "consensus/validation.h"
#include "crypto/sha3.h"
#include "health/heartbeat.h"
#include "json/json.h"
#include "primitives/block.h"
#include "script/script.h"
#include "services/chain_advance.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/disk_block_io.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "validation/chainstate.h"
#include "validation/main_logic.h"
#include "validation/main_state.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Runtime state for state-dump introspection ─────────────────── */

/* Single shared snapshot.  Writers (the running ingest thread)
 * update via atomic stores or under g_state_lock for the multi-word
 * fields; readers (zcl_state) just copy the values out.  This avoids
 * the dump path ever blocking on the heavy ingest work. */
static struct {
    pthread_mutex_t lock;
    _Atomic int  phase;            /* 0 = idle, 1 = sha3 windows, 2 = chainstate, 3 = blocks, 4 = done */
    _Atomic int  result;           /* enum local_ingest_result; LCI_OK only after phase 4 */
    _Atomic int64_t blocks_done;
    _Atomic int64_t blocks_total;
    _Atomic int64_t utxos_imported;
    _Atomic int64_t windows_verified;
    _Atomic int64_t started_at;    /* unix seconds; 0 → never run */
    _Atomic int64_t finished_at;   /* unix seconds; 0 → in progress */
    int           health_id;
    char          legacy_datadir[512];
    char          last_error[256];
} g_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .phase = 0,
    .result = LCI_OK,
    .blocks_done = 0,
    .blocks_total = 0,
    .utxos_imported = 0,
    .windows_verified = 0,
    .started_at = 0,
    .finished_at = 0,
    .health_id = HEALTH_INVALID_ID,
    .legacy_datadir = {0},
    .last_error = {0},
};

static void state_set_error(const char *msg)
{
    pthread_mutex_lock(&g_state.lock);
    snprintf(g_state.last_error, sizeof(g_state.last_error), "%s",
             msg ? msg : "");
    pthread_mutex_unlock(&g_state.lock);
}

static void state_set_datadir(const char *path)
{
    pthread_mutex_lock(&g_state.lock);
    snprintf(g_state.legacy_datadir, sizeof(g_state.legacy_datadir), "%s",
             path ? path : "");
    pthread_mutex_unlock(&g_state.lock);
}

/* health_register_periodic callback — fires from the sweeper thread
 * every PROGRESS_TICK_SECS regardless of ingest activity.  Prints a
 * single-line progress summary so operators can `journalctl -f` and
 * see liveness without enabling chatty per-block logs. */
#define LOCAL_INGEST_TICK_SECS  10

static void local_chain_ingest_tick(void *ctx)
{
    (void)ctx;
    int phase = atomic_load(&g_state.phase);
    if (phase == 0 || phase == 4) return;
    int64_t bdone  = atomic_load(&g_state.blocks_done);
    int64_t btotal = atomic_load(&g_state.blocks_total);
    int64_t utxos  = atomic_load(&g_state.utxos_imported);
    int64_t wins   = atomic_load(&g_state.windows_verified);
    fprintf(stderr,
            "[local_ingest] phase=%d blocks=%" PRId64 "/%" PRId64
            " utxos=%" PRId64 " sha3_windows_verified=%" PRId64 "\n",
            phase, bdone, btotal, utxos, wins);
}

static void ensure_health_registered(void)
{
    if (g_state.health_id == HEALTH_INVALID_ID) {
        g_state.health_id = health_register_periodic(
            "local_ingest", LOCAL_INGEST_TICK_SECS,
            local_chain_ingest_tick, NULL);
        /* HEALTH_INVALID_ID on registry-full or before health_start();
         * the ingest still runs, just without the periodic log line. */
    }
}

/* ── Result name table ───────────────────────────────────────────── */

const char *local_ingest_result_name(enum local_ingest_result r)
{
    static const char *names[] = {
        [LCI_OK]                   = "ok",
        [LCI_SOURCE_MISSING]       = "source_missing",
        [LCI_SHA3_WINDOW_MISMATCH] = "sha3_window_mismatch",
        [LCI_CHAINSTATE_MISMATCH]  = "chainstate_mismatch",
        [LCI_ABORTED]              = "aborted",
        [LCI_INTERNAL_ERROR]       = "internal_error",
    };
    if (r >= 0 && r < LCI_NUM_RESULTS) return names[r];
    return "unknown";
}

/* ── Detector ────────────────────────────────────────────────────── */

bool local_chain_ingest_detect_legacy_datadir(const char *path)
{
    if (!path || !path[0]) return false;
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s/blocks/blk00000.dat", path);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    struct stat st;
    if (stat(buf, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

static int count_legacy_block_files(const char *legacy_datadir)
{
    int n = 0;
    for (int f = 0; f < 1024; f++) {
        char p[1024];
        if (snprintf(p, sizeof(p), "%s/blocks/blk%05d.dat",
                     legacy_datadir, f) >= (int)sizeof(p))
            break;
        if (access(p, R_OK) != 0) break;
        n = f + 1;
    }
    return n;
}

/* ── Phase 1: SHA3 window verify ─────────────────────────────────── */

/* Walks the legacy datadir's blk files block-by-block and accumulates
 * SHA3 over each 1000-block window, comparing against g_sha3_windows[].
 * When g_sha3_windows_count == 0 (current placeholder), this is a
 * no-op trust-but-skip and returns LCI_OK.
 *
 * The walk treats the on-disk layout as: each block is preceded by
 * 4 bytes network-magic + 4 bytes little-endian payload length, then
 * the raw payload of `len` bytes.  This matches Bitcoin Core /
 * zclassicd's blk*.dat serialization. */
static enum local_ingest_result phase1_sha3_window_verify(
    const struct local_chain_ingest_config *cfg)
{
    atomic_store(&g_state.phase, 1);
    atomic_store(&g_state.windows_verified, 0);

    if (cfg->skip_blk_verify) {
        fprintf(stderr,
                "[local_ingest] phase 1 SHA3 window verify SKIPPED "
                "(cfg.skip_blk_verify=true)\n");
        return LCI_OK;
    }
    if (g_sha3_windows_count == 0) {
        fprintf(stderr,
                "[local_ingest] phase 1 SHA3 window verify SKIPPED "
                "(g_sha3_windows_count == 0; table is placeholder)\n");
        return LCI_OK;
    }

    int num_files = count_legacy_block_files(cfg->legacy_datadir);
    if (num_files == 0) {
        state_set_error("phase1: no blk*.dat files found");
        LOG_RETURN(LCI_SOURCE_MISSING, "local_ingest",
                   "phase1: zero blk files in %s/blocks/",
                   cfg->legacy_datadir);
    }

    /* Streaming SHA3 over each window.  We don't try to identify the
     * starting height of the first block in each file — that's why the
     * window table is height-aligned and we have to read every block in
     * order.  Concatenate raw payloads from height 0 onward. */
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    int64_t block_index_in_window = 0;
    int64_t current_window = 0;
    int64_t total_blocks = 0;
    int64_t verified = 0;

    for (int f = 0; f < num_files; f++) {
        if (thread_registry_shutdown_requested()) return LCI_ABORTED;
        char path[1024];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 cfg->legacy_datadir, f);
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            state_set_error("phase1: open blk file failed");
            LOG_RETURN(LCI_SOURCE_MISSING, "local_ingest",
                       "phase1: fopen(%s): %s", path, strerror(errno));
        }
        for (;;) {
            unsigned char hdr[8];
            size_t rd = fread(hdr, 1, 8, fp);
            if (rd < 8) break;
            /* Skip alignment runs of zero bytes (some blk files pad to
             * fixed size). */
            if (hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0)
                break;
            uint32_t plen = (uint32_t)hdr[4]        |
                            ((uint32_t)hdr[5] << 8) |
                            ((uint32_t)hdr[6] << 16)|
                            ((uint32_t)hdr[7] << 24);
            if (plen == 0 || plen > 32 * 1024 * 1024) {
                /* Garbage past last block — stop file. */
                break;
            }
            uint8_t *payload = zcl_malloc(plen, "local_ingest.phase1.payload");
            if (!payload) {
                fclose(fp);
                state_set_error("phase1: payload malloc failed");
                LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                           "phase1: malloc %u bytes failed", plen);
            }
            if (fread(payload, 1, plen, fp) != plen) {
                free(payload);
                break;
            }
            sha3_256_write(&ctx, payload, plen);
            free(payload);
            total_blocks++;
            block_index_in_window++;

            if (block_index_in_window == SHA3_WINDOW_SIZE) {
                uint8_t digest[32];
                sha3_256_finalize(&ctx, digest);
                if (current_window < (int64_t)g_sha3_windows_count) {
                    if (memcmp(digest,
                               g_sha3_windows[current_window].hash, 32) != 0) {
                        fclose(fp);
                        state_set_error("phase1: window hash mismatch");
                        LOG_RETURN(LCI_SHA3_WINDOW_MISMATCH, "local_ingest",
                                   "phase1: window %" PRId64 " mismatch",
                                   current_window);
                    }
                    verified++;
                    atomic_store(&g_state.windows_verified, verified);
                    if (verified % 10 == 0) {
                        fprintf(stderr,
                                "[local_ingest] phase1: verified %" PRId64
                                " / %zu windows\n",
                                verified, g_sha3_windows_count);
                    }
                }
                sha3_256_init(&ctx);
                block_index_in_window = 0;
                current_window++;
                if (current_window >= (int64_t)g_sha3_windows_count) {
                    /* No more entries to verify against; stop early. */
                    fclose(fp);
                    fprintf(stderr,
                            "[local_ingest] phase1: all %zu windows verified "
                            "(blocks scanned=%" PRId64 ")\n",
                            g_sha3_windows_count, total_blocks);
                    return LCI_OK;
                }
            }
        }
        fclose(fp);
    }

    fprintf(stderr,
            "[local_ingest] phase1: scan done — windows verified=%" PRId64
            " (table size=%zu) total blocks scanned=%" PRId64 "\n",
            verified, g_sha3_windows_count, total_blocks);
    return LCI_OK;
}

/* ── Phase 2: chainstate import ──────────────────────────────────── */

struct phase2_ctx {
    struct coins_view_cache *coins_tip;
    int64_t records;
    int64_t vouts;
    int64_t total_value_sat;
    bool    abort_requested;
};

static bool phase2_iter_cb(const struct uint256 *txid,
                            const struct legacy_coins *lc,
                            void *vctx)
{
    struct phase2_ctx *ctx = vctx;
    if (!ctx->coins_tip) return true;
    if (thread_registry_shutdown_requested()) {
        ctx->abort_requested = true;
        return false;
    }
    ctx->records++;

    /* Find max vout index to size the output array. */
    unsigned int max_n = 0;
    for (size_t i = 0; i < lc->num_vouts; i++) {
        if (lc->vouts[i].n > max_n) max_n = lc->vouts[i].n;
    }
    size_t num_vout = (size_t)max_n + 1;

    struct coins_cache_entry *e =
        coins_view_cache_modify_new(ctx->coins_tip, txid);
    if (!e) {
        state_set_error("phase2: coins_view_cache_modify_new failed");
        return false;
    }
    /* Allocate exactly the size we need; pre-NULL all entries. */
    struct tx_out *nv = zcl_calloc(num_vout, sizeof(struct tx_out),
                                    "local_ingest.phase2.vout");
    if (!nv) {
        state_set_error("phase2: vout calloc failed");
        return false;
    }
    for (size_t k = 0; k < num_vout; k++) tx_out_set_null(&nv[k]);
    /* Replace any previous allocation (cache hit case). */
    free(e->coins.vout);
    e->coins.vout = nv;
    e->coins.num_vout = num_vout;
    e->coins.height = lc->height;
    e->coins.is_coinbase = lc->coinbase;
    e->coins.version = lc->version;
    e->flags |= COINS_CACHE_DIRTY | COINS_CACHE_FRESH;

    for (size_t i = 0; i < lc->num_vouts; i++) {
        unsigned int n = lc->vouts[i].n;
        if (n >= num_vout) continue;
        e->coins.vout[n].value = lc->vouts[i].value;
        script_init(&e->coins.vout[n].script_pub_key);
        script_set(&e->coins.vout[n].script_pub_key,
                    lc->vouts[i].script,
                    lc->vouts[i].script_len);
        ctx->vouts++;
        ctx->total_value_sat += lc->vouts[i].value;
    }

    if ((ctx->records & 0x3fff) == 0) {
        atomic_store(&g_state.utxos_imported, ctx->vouts);
    }
    return true;
}

static enum local_ingest_result phase2_chainstate_import(
    const struct local_chain_ingest_config *cfg,
    struct coins_view_cache *coins_tip)
{
    atomic_store(&g_state.phase, 2);
    atomic_store(&g_state.utxos_imported, 0);

    if (!coins_tip) {
        state_set_error("phase2: NULL coins_tip");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: coins_tip is NULL");
    }

    char cs_path[1024];
    snprintf(cs_path, sizeof(cs_path), "%s/chainstate", cfg->legacy_datadir);
    struct stat st;
    if (stat(cs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        state_set_error("phase2: chainstate dir missing");
        LOG_RETURN(LCI_SOURCE_MISSING, "local_ingest",
                   "phase2: %s is not a directory", cs_path);
    }

    void *h = NULL;
    if (!chainstate_legacy_open(cs_path, &h) || !h) {
        state_set_error("phase2: chainstate_legacy_open failed");
        LOG_RETURN(LCI_SOURCE_MISSING, "local_ingest",
                   "phase2: chainstate_legacy_open(%s) failed", cs_path);
    }

    /* Anchor block hash from the static checkpoint — also what we set
     * as the coins_tip best_block after the bulk write. */
    const struct sha3_utxo_checkpoint *anchor = get_sha3_utxo_checkpoint();
    if (!anchor) {
        chainstate_legacy_close(h);
        state_set_error("phase2: no SHA3 anchor available");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: get_sha3_utxo_checkpoint returned NULL");
    }

    struct phase2_ctx ctx = {
        .coins_tip = coins_tip,
        .records = 0,
        .vouts = 0,
        .total_value_sat = 0,
        .abort_requested = false,
    };
    int64_t n = chainstate_legacy_iter(h, phase2_iter_cb, &ctx);
    chainstate_legacy_close(h);
    if (n < 0) {
        state_set_error("phase2: chainstate iter failed");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: chainstate_legacy_iter returned -1 after %" PRId64
                   " records", ctx.records);
    }
    if (ctx.abort_requested) {
        return LCI_ABORTED;
    }
    atomic_store(&g_state.utxos_imported, ctx.vouts);

    /* Bookkeeping: tag the coins cache's best block to the anchor hash
     * so the next chain_advance treats this state as "tip at anchor". */
    struct uint256 anchor_hash;
    memcpy(anchor_hash.data, anchor->block_hash, 32);
    coins_view_cache_set_best_block(coins_tip, &anchor_hash);

    /* Verify imported counts against the anchor.  This is a cheap
     * sanity check before we attempt the expensive SHA3-set verify
     * (which requires the data to be in the SQLite UTXO table — that
     * happens on the next batch flush in chain_advance). */
    if ((uint64_t)ctx.vouts != anchor->utxo_count) {
        fprintf(stderr,
                "[local_ingest] phase2: WARNING vout count mismatch "
                "imported=%" PRId64 " anchor=%" PRIu64
                " (legacy chainstate is at a different height than the static "
                "anchor; will retry verify against the SHA3 hash, which is the "
                "binding commitment).\n",
                ctx.vouts, anchor->utxo_count);
        /* This is NOT yet fatal — only the SHA3 verify is.  Counts
         * differ if the legacy node has advanced past h=3,056,758. */
    }

    fprintf(stderr,
            "[local_ingest] phase2: imported records=%" PRId64
            " vouts=%" PRId64 " total=%.4f ZCL (anchor expects %" PRIu64
            " UTXOs, %.4f ZCL)\n",
            ctx.records, ctx.vouts,
            (double)ctx.total_value_sat / 1e8,
            anchor->utxo_count,
            (double)anchor->total_supply / 1e8);

    return LCI_OK;
}

/* ── Phase 3: per-block ingest ────────────────────────────────────── */

/* Walk [anchor_height+1 .. final_height] applying each block via
 * chain_advance.  We rely on the local block_index already having
 * entries for these heights with valid nFile / nDataPos pointing at
 * OUR datadir's blk files.  In a fresh-boot scenario where local
 * block_index does not yet contain entries past the anchor, this
 * phase is a no-op; the standard P2P sync (block_sync_service) takes
 * over from the anchor onward.
 *
 * The full FS3-FS6 path that scans the LEGACY datadir's blk files,
 * parses each block header, and bootstraps block_index from there is
 * a separate task (the spec explicitly says "DO NOT touch boot.c —
 * that's FS5's job").  This module deliberately stops at the
 * already-known-locally boundary. */
static enum local_ingest_result phase3_block_ingest(
    const struct local_chain_ingest_config *cfg,
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct chain_params *params,
    const char *our_datadir)
{
    atomic_store(&g_state.phase, 3);
    atomic_store(&g_state.blocks_done, 0);

    if (!ms || !params || !our_datadir) {
        state_set_error("phase3: missing context");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase3: ms=%p params=%p datadir=%p",
                   (void *)ms, (const void *)params,
                   (const void *)our_datadir);
    }

    const struct sha3_utxo_checkpoint *anchor = get_sha3_utxo_checkpoint();
    int anchor_h = anchor ? anchor->height : 0;
    int tip_h = active_chain_height(&ms->chain_active);
    int final_h = (cfg->max_height > 0 && cfg->max_height < tip_h)
                  ? cfg->max_height : tip_h;
    if (final_h <= anchor_h) {
        fprintf(stderr,
                "[local_ingest] phase3: nothing to apply "
                "(anchor=%d local_tip=%d final=%d)\n",
                anchor_h, tip_h, final_h);
        return LCI_OK;
    }
    int64_t total = (int64_t)(final_h - anchor_h);
    atomic_store(&g_state.blocks_total, total);

    int64_t applied = 0;
    for (int h = anchor_h + 1; h <= final_h; h++) {
        if (thread_registry_shutdown_requested()) return LCI_ABORTED;

        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi) {
            /* No local block_index at this height yet — defer to P2P
             * sync.  Stop the phase cleanly so the caller can hand
             * off. */
            fprintf(stderr,
                    "[local_ingest] phase3: stopping at height %d "
                    "(no local block_index entry; falling back to "
                    "P2P sync from here)\n", h);
            break;
        }

        struct validation_state vs;
        memset(&vs, 0, sizeof(vs));
        enum chain_advance_result rc = chain_advance(&vs, ms, coins_tip,
                                                      bi, NULL, params,
                                                      our_datadir,
                                                      "local_chain_ingest");
        if (rc != CA_OK) {
            char err[256];
            snprintf(err, sizeof(err),
                     "phase3: chain_advance(height=%d) -> %s",
                     h, chain_advance_result_name(rc));
            state_set_error(err);
            LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest", "%s", err);
        }
        applied++;
        atomic_store(&g_state.blocks_done, applied);
    }

    fprintf(stderr,
            "[local_ingest] phase3: applied=%" PRId64 " blocks "
            "(anchor=%d → tip=%d)\n",
            applied, anchor_h, anchor_h + (int)applied);
    return LCI_OK;
}

/* ── Public entry point ─────────────────────────────────────────── */

enum local_ingest_result local_chain_ingest_run(
    const struct local_chain_ingest_config *cfg,
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct chain_params *params,
    const char *our_datadir)
{
    if (!cfg || !cfg->legacy_datadir) {
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "run: cfg or legacy_datadir is NULL");
    }
    if (!local_chain_ingest_detect_legacy_datadir(cfg->legacy_datadir)) {
        state_set_datadir(cfg->legacy_datadir);
        state_set_error("legacy datadir missing blocks/blk00000.dat");
        atomic_store(&g_state.result, LCI_SOURCE_MISSING);
        LOG_RETURN(LCI_SOURCE_MISSING, "local_ingest",
                   "run: legacy datadir not detectable at %s",
                   cfg->legacy_datadir);
    }
    state_set_datadir(cfg->legacy_datadir);
    ensure_health_registered();

    atomic_store(&g_state.started_at, (int64_t)time(NULL));
    atomic_store(&g_state.finished_at, 0);
    atomic_store(&g_state.result, LCI_OK);

    enum local_ingest_result r = phase1_sha3_window_verify(cfg);
    if (r != LCI_OK) {
        atomic_store(&g_state.result, (int)r);
        atomic_store(&g_state.finished_at, (int64_t)time(NULL));
        return r;
    }

    r = phase2_chainstate_import(cfg, coins_tip);
    if (r != LCI_OK) {
        atomic_store(&g_state.result, (int)r);
        atomic_store(&g_state.finished_at, (int64_t)time(NULL));
        return r;
    }

    r = phase3_block_ingest(cfg, ms, coins_tip, params, our_datadir);
    if (r != LCI_OK) {
        atomic_store(&g_state.result, (int)r);
        atomic_store(&g_state.finished_at, (int64_t)time(NULL));
        return r;
    }

    atomic_store(&g_state.phase, 4);
    atomic_store(&g_state.finished_at, (int64_t)time(NULL));
    atomic_store(&g_state.result, LCI_OK);
    return LCI_OK;
}

/* ── State dump for zcl_state subsystem=local_ingest ────────────── */

bool local_chain_ingest_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    /* Caller is expected to have json_set_object'd `out` first (per the
     * *_dump_state_json convention in CLAUDE.md).  We tolerate either
     * initialised-or-not by setting it explicitly here too — json_set_object
     * is idempotent. */
    json_set_object(out);

    int phase = atomic_load(&g_state.phase);
    int result = atomic_load(&g_state.result);
    int64_t started  = atomic_load(&g_state.started_at);
    int64_t finished = atomic_load(&g_state.finished_at);
    int64_t bdone    = atomic_load(&g_state.blocks_done);
    int64_t btotal   = atomic_load(&g_state.blocks_total);
    int64_t utxos    = atomic_load(&g_state.utxos_imported);
    int64_t wins     = atomic_load(&g_state.windows_verified);

    pthread_mutex_lock(&g_state.lock);
    char datadir_copy[sizeof(g_state.legacy_datadir)];
    char err_copy[sizeof(g_state.last_error)];
    memcpy(datadir_copy, g_state.legacy_datadir, sizeof(datadir_copy));
    memcpy(err_copy, g_state.last_error, sizeof(err_copy));
    pthread_mutex_unlock(&g_state.lock);

    json_push_kv_int (out, "phase", phase);
    json_push_kv_str (out, "result_name",
                      local_ingest_result_name((enum local_ingest_result)result));
    json_push_kv_int (out, "result_code", result);
    json_push_kv_int (out, "started_at", started);
    json_push_kv_int (out, "finished_at", finished);
    json_push_kv_int (out, "blocks_done", bdone);
    json_push_kv_int (out, "blocks_total", btotal);
    json_push_kv_int (out, "utxos_imported", utxos);
    json_push_kv_int (out, "windows_verified", wins);
    json_push_kv_int (out, "windows_table_size",
                      (int64_t)g_sha3_windows_count);
    json_push_kv_str (out, "legacy_datadir", datadir_copy);
    json_push_kv_str (out, "last_error", err_copy);
    json_push_kv_bool(out, "in_progress",
                      (started > 0 && finished == 0));
    return true;
}
