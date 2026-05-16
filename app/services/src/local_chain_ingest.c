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
#include "chain/utxo_snapshot_loader.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "core/uint256.h"
#include "consensus/validation.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"
#include "health/heartbeat.h"
#include "json/json.h"
#include "primitives/block.h"
#include "script/script.h"
#include "services/chain_advance.h"
#include "services/chain_state_repository.h"
#include "services/header_probe_service.h"
#include "services/legacy_body_pull.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/coins_view_sqlite.h"
#include "storage/disk_block_io.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
#include "util/workpool.h"
#include "util/util.h"  /* GetNumCores */
#include "validation/chainstate.h"
#include "validation/main_logic.h"
#include "validation/main_state.h"
#include "validation/process_block_internals.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    _Atomic bool evidence_prefix_verified;  /* T3.3: full prefix verified this boot */
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
    .evidence_prefix_verified = false,
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
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
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

/* ── T1.3: Evidence-cache cookie ─────────────────────────────────────
 *
 * After a successful phase-1 SHA3 window scan, persist a fingerprint
 * of every blk file (mtime + size) and the verified window count to
 * <our_datadir>/legacy_evidence.cookie.  Next boot: if every blk file's
 * (mtime,size) is unchanged AND windows_count covers the full static
 * evidence prefix, skip phase 1 entirely.
 *
 * Mismatch falls back to a full scan — the cookie never weakens
 * verification, only short-circuits it when the on-disk evidence
 * proves no work needs redoing. */

#define LCI_COOKIE_SCHEMA 1
#define LCI_COOKIE_NAME   "legacy_evidence.cookie"
#define LCI_FINGERPRINT_NAME "chainstate_fingerprint.dat"
#define LCI_FINGERPRINT_SCHEMA 1

/* ── T3.2: chainstate fingerprint ────────────────────────────────
 *
 * After phase 2 produces a verifiable UTXO set, persist its SHA3
 * digest + count + total to <our_datadir>/chainstate_fingerprint.dat.
 * Next boot: if coins.db already contains a UTXO set whose SHA3
 * matches the stored digest, skip phase 2 entirely. */
struct chainstate_fingerprint {
    int     schema;
    int     anchor_height;
    uint8_t anchor_block_hash[32];
    uint8_t sha3_utxo_hash[32];
    uint64_t utxos_count;
    int64_t total_supply_sat;
};

static bool chainstate_fingerprint_path(char *out, size_t out_sz,
                                         const char *our_datadir)
{
    if (!our_datadir || !our_datadir[0]) return false;
    int n = snprintf(out, out_sz, "%s/%s", our_datadir,
                     LCI_FINGERPRINT_NAME);
    return n > 0 && (size_t)n < out_sz;
}

static bool chainstate_fingerprint_load(const char *our_datadir,
                                         struct chainstate_fingerprint *out)
{
    char path[1024];
    if (!chainstate_fingerprint_path(path, sizeof(path), our_datadir))
        return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    memset(out, 0, sizeof(*out));
    char line[1024];
    bool got_sha3 = false, got_anchor = false;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *e = val + strlen(val);
        while (e > val && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
        if (strcmp(key, "schema") == 0)
            out->schema = atoi(val);
        else if (strcmp(key, "anchor_height") == 0)
            out->anchor_height = atoi(val);
        else if (strcmp(key, "anchor_block_hash") == 0) {
            if (ParseHex(val, out->anchor_block_hash, 32) != 32) continue;
            got_anchor = true;
        } else if (strcmp(key, "sha3_utxo_hash") == 0) {
            if (ParseHex(val, out->sha3_utxo_hash, 32) != 32) continue;
            got_sha3 = true;
        } else if (strcmp(key, "utxos_count") == 0)
            out->utxos_count = strtoull(val, NULL, 10);
        else if (strcmp(key, "total_supply_sat") == 0)
            out->total_supply_sat = strtoll(val, NULL, 10);
    }
    fclose(f);
    return out->schema == LCI_FINGERPRINT_SCHEMA && got_sha3 && got_anchor;
}

static void chainstate_fingerprint_write(const char *our_datadir,
                                          const struct chainstate_fingerprint *fp)
{
    char path[1024], tmp[1024];
    if (!chainstate_fingerprint_path(path, sizeof(path), our_datadir)) return;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return;
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] fingerprint: fopen(%s) failed: %s\n",
                tmp, strerror(errno));
        return;
    }
    char anchor_hex[65], sha3_hex[65];
    HexStr(fp->anchor_block_hash, 32, false, anchor_hex, sizeof(anchor_hex));
    HexStr(fp->sha3_utxo_hash,    32, false, sha3_hex,   sizeof(sha3_hex));
    fprintf(f, "schema=%d\n", fp->schema);
    fprintf(f, "anchor_height=%d\n", fp->anchor_height);
    fprintf(f, "anchor_block_hash=%s\n", anchor_hex);
    fprintf(f, "sha3_utxo_hash=%s\n", sha3_hex);
    fprintf(f, "utxos_count=%" PRIu64 "\n", fp->utxos_count);
    fprintf(f, "total_supply_sat=%" PRId64 "\n", fp->total_supply_sat);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] fingerprint: fflush/fsync failed: %s\n",
                strerror(errno));
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] fingerprint: rename failed: %s\n",
                strerror(errno));
        unlink(tmp);
        return;
    }
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[local_ingest] fingerprint: persisted (anchor_h=%d "
            "count=%" PRIu64 " sha3=%s)\n",
            fp->anchor_height, fp->utxos_count, sha3_hex);
}

static bool legacy_evidence_cookie_path(char *out, size_t out_sz,
                                      const char *our_datadir)
{
    if (!our_datadir || !our_datadir[0]) return false;
    int n = snprintf(out, out_sz, "%s/%s", our_datadir, LCI_COOKIE_NAME);
    return n > 0 && (size_t)n < out_sz;
}

/* Returns true iff the cookie at <our_datadir>/legacy_evidence.cookie
 * proves a previous run already verified `g_sha3_windows_count` windows
 * of `legacy_datadir`'s blk files in their current on-disk state. */
static bool legacy_evidence_cookie_valid(const char *our_datadir,
                                       const char *legacy_datadir)
{
    char path[1024];
    if (!legacy_evidence_cookie_path(path, sizeof(path), our_datadir))
        return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    int schema = 0, cookie_file_count = -1;
    size_t cookie_windows = 0;
    char cookie_legacy[512] = {0};
    bool fields_ok = true;

    /* First pass: header fields. */
    char line[1024];
    long body_start = -1;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *e = val + strlen(val);
        while (e > val && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';

        if (strcmp(key, "schema") == 0) schema = atoi(val);
        else if (strcmp(key, "legacy_datadir") == 0)
            snprintf(cookie_legacy, sizeof(cookie_legacy), "%s", val);
        else if (strcmp(key, "windows_count") == 0)
            cookie_windows = (size_t)strtoull(val, NULL, 10);
        else if (strcmp(key, "file_count") == 0)
            cookie_file_count = atoi(val);
        else if (strncmp(key, "blk", 3) == 0) {
            /* Reached the per-file section. Restore the '=' and rewind. */
            *eq = '=';
            body_start = ftell(f) - (long)strlen(line) - 1;
            break;
        }
    }

    if (schema != LCI_COOKIE_SCHEMA ||
        strcmp(cookie_legacy, legacy_datadir) != 0 ||
        cookie_windows != g_sha3_windows_count ||
        cookie_file_count < 1) {
        fclose(f);
        return false;
    }

    int observed_file_count = count_legacy_block_files(legacy_datadir);
    if (observed_file_count != cookie_file_count) {
        fclose(f);
        return false;
    }

    /* Second pass: per-file mtime+size match. We require every file
     * named in the cookie to be present on disk with the same
     * (mtime,size); a strict equality check is enough since blk files
     * are append-only and resizing or rotation changes mtime. */
    if (body_start >= 0) fseek(f, body_start, SEEK_SET);
    int matched = 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *e = val + strlen(val);
        while (e > val && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
        if (strncmp(key, "blk", 3) != 0) continue;
        long long want_mtime = -1, want_size = -1;
        if (sscanf(val, "%lld:%lld", &want_mtime, &want_size) != 2) {
            fields_ok = false;
            break;
        }
        char blk_path[1024];
        snprintf(blk_path, sizeof(blk_path), "%s/blocks/%s",
                 legacy_datadir, key);
        struct stat st;
        if (stat(blk_path, &st) != 0 ||
            (long long)st.st_mtime != want_mtime ||
            (long long)st.st_size != want_size) {
            fields_ok = false;
            break;
        }
        matched++;
    }
    fclose(f);
    return fields_ok && matched == cookie_file_count;
}

/* Persist the cookie. Best-effort: a write failure logs a warning
 * but does not fail the ingest run (the data is already verified). */
static void legacy_evidence_cookie_write(const char *our_datadir,
                                       const char *legacy_datadir,
                                       int file_count)
{
    char path[1024], tmp[1024];
    if (!legacy_evidence_cookie_path(path, sizeof(path), our_datadir)) return;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return;

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] cookie: fopen(%s) failed: %s\n",
                tmp, strerror(errno));
        return;
    }
    fprintf(f, "schema=%d\n", LCI_COOKIE_SCHEMA);
    fprintf(f, "legacy_datadir=%s\n", legacy_datadir);
    fprintf(f, "windows_count=%zu\n", g_sha3_windows_count);
    fprintf(f, "file_count=%d\n", file_count);
    for (int i = 0; i < file_count; i++) {
        char blk_name[32];
        char blk_path[1024];
        snprintf(blk_name, sizeof(blk_name), "blk%05d.dat", i);
        snprintf(blk_path, sizeof(blk_path), "%s/blocks/%s",
                 legacy_datadir, blk_name);
        struct stat st;
        if (stat(blk_path, &st) != 0) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] cookie: stat(%s) failed at write\n",
                    blk_path);
            fclose(f);
            unlink(tmp);
            return;
        }
        fprintf(f, "%s=%lld:%lld\n", blk_name,
                (long long)st.st_mtime, (long long)st.st_size);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] cookie: fflush/fsync failed: %s\n",
                strerror(errno));
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] cookie: rename(%s,%s) failed: %s\n",
                tmp, path, strerror(errno));
        unlink(tmp);
        return;
    }
    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[local_ingest] cookie: persisted %s (file_count=%d "
            "windows_count=%zu)\n",
            path, file_count, g_sha3_windows_count);
}

/* ── T1.2: Parallel Phase-1 SHA3 helpers ────────────────────────
 *
 * Two-pass strategy:
 *   1. Index pass (single-threaded, cheap) — walk blk*.dat files and
 *      record (file_idx, payload_offset, payload_len) per block,
 *      seeking past payloads without reading them.
 *   2. Hash pass (parallel) — submit one task per 1000-block window
 *      to a workpool. Each worker opens the needed blk files,
 *      pread()s payloads, streams into a per-window SHA3 context,
 *      compares to g_sha3_windows[].
 *
 * Workers may share files via pread() which is thread-safe; we open
 * a small per-worker file handle cache to avoid re-opening on every
 * block. */

struct phase1_block_loc {
    int       file_idx;        /* blk%05d.dat index */
    uint64_t  payload_offset;  /* offset of payload bytes within the file */
    uint32_t  plen;
};

struct phase1_window_task {
    int                            window_idx;
    const struct phase1_block_loc *blocks;
    int                            num_blocks;   /* always SHA3_WINDOW_SIZE for verified windows */
    const char                    *legacy_datadir;
    _Atomic bool                   mismatch;
    _Atomic bool                   io_error;
    _Atomic int64_t               *windows_verified_counter;
};

/* Walk blk files once, recording every block's payload location.
 * Returns NULL on I/O failure; *out_count receives the number of
 * blocks indexed. */
static struct phase1_block_loc *phase1_index_blocks(const char *legacy_datadir,
                                                     int num_files,
                                                     int64_t *out_count)
{
    *out_count = 0;
    /* Reserve generously; we'll realloc if needed. */
    int64_t cap = 4 * 1024 * 1024;
    struct phase1_block_loc *arr =
        zcl_malloc((size_t)cap * sizeof(*arr), "phase1.index.arr");
    if (!arr) return NULL;
    int64_t n = 0;

    for (int f = 0; f < num_files; f++) {
        if (thread_registry_shutdown_requested()) {
            free(arr);
            return NULL;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 legacy_datadir, f);
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] index: fopen(%s) failed: %s\n",
                    path, strerror(errno));
            free(arr);
            return NULL;
        }
        for (;;) {
            unsigned char hdr[8];
            size_t rd = fread(hdr, 1, 8, fp);
            if (rd < 8) break;
            if (hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0)
                break;
            uint32_t plen = (uint32_t)hdr[4]        |
                            ((uint32_t)hdr[5] << 8) |
                            ((uint32_t)hdr[6] << 16)|
                            ((uint32_t)hdr[7] << 24);
            if (plen == 0 || plen > 32 * 1024 * 1024) break;
            long here = ftell(fp);
            if (here < 0) {
                fclose(fp);
                free(arr);
                return NULL;
            }
            if (n == cap) {
                cap *= 2;
                struct phase1_block_loc *narr =
                    zcl_realloc(arr,
                                (size_t)cap * sizeof(*arr),
                                "phase1.index.arr");
                if (!narr) {
                    fclose(fp);
                    free(arr);
                    return NULL;
                }
                arr = narr;
            }
            arr[n].file_idx = f;
            arr[n].payload_offset = (uint64_t)here;
            arr[n].plen = plen;
            n++;
            if (fseek(fp, (long)plen, SEEK_CUR) != 0) break;
        }
        fclose(fp);
    }
    *out_count = n;
    return arr;
}

/* Worker — hash one window's blocks and compare.
 *
 * F2: each blk file is mmap'd once per worker (small LRU cache),
 * SEQUENTIAL+WILLNEED hinted, and block payloads are hashed directly
 * from the mmap'd pointer. This eliminates 1.35M memcpy + malloc/free
 * pairs compared to the prior fopen+fread+malloc loop. Pagecache is
 * already shared with zclassicd (same inode). */
static bool phase1_window_worker(void *vtask)
{
    struct phase1_window_task *t = vtask;
    if (thread_registry_shutdown_requested()) {
        atomic_store(&t->io_error, true);
        return false;
    }

    enum { CACHE = 4 };
    struct {
        int       file_idx;
        const uint8_t *base;
        size_t    len;
    } maps[CACHE] = {0};
    for (int c = 0; c < CACHE; c++) maps[c].file_idx = -1;
    int nxt = 0;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    for (int b = 0; b < t->num_blocks; b++) {
        int fk = t->blocks[b].file_idx;
        const uint8_t *base = NULL;
        size_t base_len = 0;
        for (int c = 0; c < CACHE; c++) {
            if (maps[c].file_idx == fk && maps[c].base) {
                base = maps[c].base;
                base_len = maps[c].len;
                break;
            }
        }
        if (!base) {
            char path[1024];
            snprintf(path, sizeof(path),
                     "%s/blocks/blk%05d.dat",
                     t->legacy_datadir, fk);
            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                atomic_store(&t->io_error, true);
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[local_ingest] window %d: open(%s) failed: %s\n",
                        t->window_idx, path, strerror(errno));
                goto out_fail;
            }
            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0) {
                close(fd);
                atomic_store(&t->io_error, true);
                goto out_fail;
            }
            void *mp = mmap(NULL, (size_t)st.st_size, PROT_READ,
                            MAP_PRIVATE, fd, 0);
            close(fd);  /* fd no longer needed after mmap */
            if (mp == MAP_FAILED) {
                atomic_store(&t->io_error, true);
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[local_ingest] window %d: mmap(%s) failed: %s\n",
                        t->window_idx, path, strerror(errno));
                goto out_fail;
            }
            posix_madvise(mp, (size_t)st.st_size,
                          POSIX_MADV_SEQUENTIAL);
            /* Evict the LRU slot. */
            if (maps[nxt].base)
                munmap((void *)maps[nxt].base, maps[nxt].len);
            maps[nxt].base = (const uint8_t *)mp;
            maps[nxt].len  = (size_t)st.st_size;
            maps[nxt].file_idx = fk;
            base = maps[nxt].base;
            base_len = maps[nxt].len;
            nxt = (nxt + 1) % CACHE;
        }
        uint32_t plen = t->blocks[b].plen;
        size_t   off  = (size_t)t->blocks[b].payload_offset;
        if (off + plen > base_len) {
            atomic_store(&t->io_error, true);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] window %d: block %d: offset+plen "
                    "(%zu) > file size (%zu) in blk%05d.dat\n",
                    t->window_idx, b, off + plen, base_len, fk);
            goto out_fail;
        }
        sha3_256_write(&ctx, base + off, plen);
    }

    uint8_t digest[32];
    sha3_256_finalize(&ctx, digest);
    if (memcmp(digest, g_sha3_windows[t->window_idx].hash, 32) != 0) {
        atomic_store(&t->mismatch, true);
        goto out_fail;
    }
    if (t->windows_verified_counter)
        atomic_fetch_add(t->windows_verified_counter, 1);

    for (int c = 0; c < CACHE; c++)
        if (maps[c].base) munmap((void *)maps[c].base, maps[c].len);
    return true;

out_fail:
    for (int c = 0; c < CACHE; c++)
        if (maps[c].base) munmap((void *)maps[c].base, maps[c].len);
    return false;
}

/* Returns LCI_OK on full success or the appropriate error code. */
static enum local_ingest_result phase1_parallel_verify(
    const struct phase1_block_loc *locs, int64_t num_blocks,
    const char *legacy_datadir, int worker_count)
{
    int64_t verifiable_windows = (int64_t)g_sha3_windows_count;
    int64_t max_windows = num_blocks / (int64_t)SHA3_WINDOW_SIZE;
    if (max_windows < verifiable_windows) verifiable_windows = max_windows;
    if (verifiable_windows <= 0) return LCI_OK;

    struct workpool wp;
    if (!workpool_init(&wp, worker_count,
                       (size_t)verifiable_windows, phase1_window_worker)) {
        return LCI_INTERNAL_ERROR;
    }

    struct phase1_window_task *tasks =
        zcl_calloc((size_t)verifiable_windows, sizeof(*tasks),
                   "phase1.parallel.tasks");
    void **items = zcl_calloc((size_t)verifiable_windows, sizeof(*items),
                               "phase1.parallel.items");
    if (!tasks || !items) {
        free(tasks);
        free(items);
        workpool_destroy(&wp);
        return LCI_INTERNAL_ERROR;
    }
    for (int64_t w = 0; w < verifiable_windows; w++) {
        tasks[w].window_idx = (int)w;
        tasks[w].blocks = locs + (int64_t)w * SHA3_WINDOW_SIZE;
        tasks[w].num_blocks = (int)SHA3_WINDOW_SIZE;
        tasks[w].legacy_datadir = legacy_datadir;
        tasks[w].mismatch = false;
        tasks[w].io_error = false;
        tasks[w].windows_verified_counter = &g_state.windows_verified;
        items[w] = &tasks[w];
    }

    bool all_ok = workpool_run(&wp, items, (size_t)verifiable_windows);

    enum local_ingest_result r = LCI_OK;
    if (!all_ok) {
        for (int64_t w = 0; w < verifiable_windows; w++) {
            if (atomic_load(&tasks[w].mismatch)) {
                state_set_error("phase1: window hash mismatch (parallel)");
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[local_ingest] phase1 parallel: window %" PRId64
                        " mismatch\n", w);
                r = LCI_SHA3_WINDOW_MISMATCH;
                break;
            }
        }
        if (r == LCI_OK) {
            r = thread_registry_shutdown_requested()
                ? LCI_ABORTED : LCI_INTERNAL_ERROR;
            state_set_error("phase1: parallel verifier I/O failure");
        }
    }
    free(tasks);
    free(items);
    workpool_destroy(&wp);
    return r;
}

/* ── Phase 1: SHA3 window verify ─────────────────────────────────── */

/* Walks the legacy datadir's blk files block-by-block and accumulates
 * SHA3 over each 1000-block window, comparing against g_sha3_windows[].
 * When g_sha3_windows_count == 0 (current placeholder), this verifier
 * has no static window evidence to check and returns LCI_OK.
 *
 * The walk treats the on-disk layout as: each block is preceded by
 * 4 bytes network-magic + 4 bytes little-endian payload length, then
 * the raw payload of `len` bytes.  This matches Bitcoin Core /
 * zclassicd's blk*.dat serialization. */
static enum local_ingest_result phase1_sha3_window_verify(
    const struct local_chain_ingest_config *cfg,
    const char *our_datadir)
{
    atomic_store(&g_state.phase, 1);
    atomic_store(&g_state.windows_verified, 0);

    if (cfg->skip_blk_verify) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] phase 1 SHA3 window verify SKIPPED "
                "(cfg.skip_blk_verify=true)\n");
        return LCI_OK;
    }
    if (g_sha3_windows_count == 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] phase 1 SHA3 window verify SKIPPED "
                "(g_sha3_windows_count == 0; table is placeholder)\n");
        return LCI_OK;
    }

    /* T1.3: skip the SHA3 scan if a prior boot already verified the
     * current on-disk state. The cookie is mtime+size keyed per blk
     * file; any change forces a re-scan. */
    if (!cfg->ignore_evidence_cookie &&
        legacy_evidence_cookie_valid(our_datadir, cfg->legacy_datadir)) {
        atomic_store(&g_state.windows_verified,
                     (int64_t)g_sha3_windows_count);
        atomic_store(&g_state.evidence_prefix_verified, true);
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] phase 1 SHA3 window verify SKIPPED "
                "(evidence cookie valid; %zu windows previously verified)\n",
                g_sha3_windows_count);
        return LCI_OK;
    }

    int num_files = count_legacy_block_files(cfg->legacy_datadir);
    if (num_files == 0) {
        state_set_error("phase1: no blk*.dat files found");
        LOG_RETURN(LCI_SOURCE_MISSING, "local_ingest",
                   "phase1: zero blk files in %s/blocks/",
                   cfg->legacy_datadir);
    }

    /* T1.2: parallel path — index blocks once, hash windows in
     * parallel via workpool. Fall back to the streaming serial path
     * on indexing failure. */
    if (!cfg->force_sequential_phase1) {
        int workers = cfg->phase1_workers;
        if (workers <= 0) {
            int cores = GetNumCores();
            workers = cores > 1 ? cores / 2 : 1;
            if (workers > WORKPOOL_MAX_THREADS) workers = WORKPOOL_MAX_THREADS;
        }
        if (workers > 1) {
            int64_t idx_count = 0;
            struct phase1_block_loc *locs =
                phase1_index_blocks(cfg->legacy_datadir, num_files,
                                     &idx_count);
            if (locs && idx_count > 0) {
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[local_ingest] phase1 parallel: indexed %" PRId64
                        " blocks across %d files, hashing with %d workers\n",
                        idx_count, num_files, workers);
                enum local_ingest_result r =
                    phase1_parallel_verify(locs, idx_count,
                                            cfg->legacy_datadir, workers);
                free(locs);
                if (r == LCI_OK) {
                    int64_t verified_now =
                        atomic_load(&g_state.windows_verified);
                    if ((size_t)verified_now >= g_sha3_windows_count) {
                        atomic_store(&g_state.evidence_prefix_verified, true);
                        legacy_evidence_cookie_write(our_datadir,
                                                  cfg->legacy_datadir,
                                                  num_files);
                    }
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "[local_ingest] phase1 parallel: complete "
                            "(verified=%" PRId64 ")\n", verified_now);
                }
                return r;
            }
            free(locs);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] phase1: parallel indexing failed, "
                    "falling back to sequential scan\n");
        }
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
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "[local_ingest] phase1: all %zu windows verified "
                            "(blocks scanned=%" PRId64 ")\n",
                            g_sha3_windows_count, total_blocks);
                    atomic_store(&g_state.evidence_prefix_verified, true);
                    legacy_evidence_cookie_write(our_datadir,
                                              cfg->legacy_datadir,
                                              num_files);
                    return LCI_OK;
                }
            }
        }
        fclose(fp);
    }

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[local_ingest] phase1: scan done — windows verified=%" PRId64
            " (table size=%zu) total blocks scanned=%" PRId64 "\n",
            verified, g_sha3_windows_count, total_blocks);
    if ((size_t)verified == g_sha3_windows_count) {
        atomic_store(&g_state.evidence_prefix_verified, true);
        legacy_evidence_cookie_write(our_datadir, cfg->legacy_datadir,
                                   num_files);
    }
    return LCI_OK;
}

/* T3.3 accessors — exposed via services/local_chain_ingest.h so the
 * bg-validation service can skip historical heights covered by the
 * verified static SHA3 prefix. */
bool local_chain_ingest_evidence_prefix_verified(void)
{
    return atomic_load(&g_state.evidence_prefix_verified);
}

int local_chain_ingest_evidence_prefix_end_height(void)
{
    if (g_sha3_windows_count == 0) return -1; // raw-return-ok: sentinel-no-compile-time-windows
    return (int)(g_sha3_windows_count * SHA3_WINDOW_SIZE) - 1;
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

/* Stage J3 fast-path bulk-insert helper. Top-level because ISO C
 * forbids nested functions. The callback collects records into a
 * batch buffer and flushes via coins_view_sqlite_bulk_insert on
 * batch_cap or stop signal. */
struct fast_phase2_ctx {
    struct utxo_bulk_rec *batch;
    size_t fill;
    size_t batch_cap;
    struct coins_view_sqlite *cvs;
    int64_t inserted;
    int errors;
};

static enum local_ingest_result phase2_commit_anchor_via_csr(
    struct main_state *ms,
    const struct uint256 *anchor_hash,
    int anchor_height,
    int64_t expected_utxos,
    const char *reason)
{
    if (!ms || !anchor_hash) {
        state_set_error("phase2: missing CSR anchor context");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: missing CSR anchor context");
    }

    struct block_index *anchor_bi =
        block_map_find(&ms->map_block_index, anchor_hash);
    if (!anchor_bi || !anchor_bi->phashBlock) {
        state_set_error("phase2: verified anchor missing from block index");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: verified anchor h=%d missing from block index",
                   anchor_height);
    }
    if (anchor_bi->nHeight != anchor_height) {
        state_set_error("phase2: verified anchor height mismatch");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: verified anchor height mismatch index=%d expected=%d",
                   anchor_bi->nHeight, anchor_height);
    }

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_RESTORE,
        .decision = POLICY_ALLOW,
        .from_height = active_chain_height(&ms->chain_active),
        .to_height = anchor_bi->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "local_ingest_utxo_sha3_verified",
        .reason = reason ? reason : "local_ingest.phase2_anchor",
    };
    struct chain_state_commit commit = {
        .new_tip = anchor_bi,
        .new_coins_best = *anchor_bi->phashBlock,
        .expected_utxo_count = expected_utxos,
        .update_header_tip = true,
        .persist_coins_best = true,
        .rollback_auth = &rollback_auth,
        .wallet_scan_height = -1,
        .reason = reason ? reason : "local_ingest.phase2_anchor",
    };
    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc != CSR_OK) {
        char err[192];
        snprintf(err, sizeof(err),
                 "phase2: CSR anchor commit failed: %s",
                 csr_result_name(rc));
        state_set_error(err);
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest", "%s", err);
    }

    return LCI_OK;
}

static bool phase2_fast_bulk_cb(const struct uss_record *r, void *vctx)
{
    struct fast_phase2_ctx *c = vctx;
    c->batch[c->fill++] = (struct utxo_bulk_rec){
        .txid = r->txid,
        .vout = r->vout,
        .value = r->value,
        .script = r->script,
        .script_len = r->script_len,
        .height = r->height,
        .is_coinbase = r->is_coinbase,
    };
    if (c->fill == c->batch_cap) {
        int64_t w = coins_view_sqlite_bulk_insert(
            c->cvs, c->batch, c->fill);
        if (w != (int64_t)c->fill) c->errors++;
        c->inserted += (w > 0 ? w : 0);
        c->fill = 0;
    }
    return c->errors == 0;
}

static enum local_ingest_result phase2_chainstate_import(
    const struct local_chain_ingest_config *cfg,
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const char *our_datadir)
{
    atomic_store(&g_state.phase, 2);
    atomic_store(&g_state.utxos_imported, 0);

    if (!coins_tip) {
        state_set_error("phase2: NULL coins_tip");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: coins_tip is NULL");
    }
    if (!ms) {
        state_set_error("phase2: NULL main_state");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: main_state is NULL");
    }

    /* T3.2: skip phase 2 entirely if coins.db already contains a UTXO
     * set whose SHA3 matches the persisted fingerprint. */
    if (!cfg->ignore_evidence_cookie) {
        struct chainstate_fingerprint fp;
        if (chainstate_fingerprint_load(our_datadir, &fp)) {
            struct coins_view_sqlite *cvs =
                process_block_get_coins_sqlite();
            if (cvs && cvs->db) {
                uint8_t computed[32];
                uint64_t count = 0;
                utxo_commitment_sha3_compute(cvs->db, computed, &count);
                if (count == fp.utxos_count &&
                    memcmp(computed, fp.sha3_utxo_hash, 32) == 0) {
                    atomic_store(&g_state.utxos_imported,
                                 (int64_t)count);
                    struct uint256 anchor_hash;
                    memcpy(anchor_hash.data, fp.anchor_block_hash, 32);
                    enum local_ingest_result cr =
                        phase2_commit_anchor_via_csr(
                            ms, &anchor_hash, fp.anchor_height,
                            (int64_t)count,
                            "local_ingest.fingerprint_anchor");
                    if (cr != LCI_OK)
                        return cr;
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "[local_ingest] phase 2 chainstate import "
                            "SKIPPED (fingerprint match: anchor_h=%d "
                            "count=%" PRIu64 ")\n",
                            fp.anchor_height, count);
                    return LCI_OK;
                }
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[local_ingest] phase 2: fingerprint present "
                        "but coins.db SHA3 differs (count=%" PRIu64
                        " fp.count=%" PRIu64 ") — full import will run\n",
                        count, fp.utxos_count);
            }
        }
    }

    /* ── Stage J3 fast path: embedded UTXO sidecar ────────────────
     *
     * If <our_datadir>/utxo_snapshot.dat exists AND its SHA3-256
     * matches the compile-time checkpoint at h=3,056,758, mmap it,
     * bulk-INSERT into coins.db, and skip the chainstate iteration
     * entirely. The full-body SHA3 verify happens inside uss_open()
     * — the bind to the compile-time anchor proves the bytes match
     * what was committed to at build time, so evidence is preserved.
     *
     * The chainstate path remains as fallback when no sidecar is
     * present or its hash doesn't match (e.g. user generated the
     * sidecar at a different height for experimentation).
     */
    {
        char uss_path[1100];
        snprintf(uss_path, sizeof(uss_path),
                 "%s/utxo_snapshot.dat", our_datadir);
        struct stat ust;
        if (stat(uss_path, &ust) == 0 && S_ISREG(ust.st_mode)) {
            const struct sha3_utxo_checkpoint *anchor_pre =
                get_sha3_utxo_checkpoint();
            if (anchor_pre) {
                char err[256] = {0};
                struct uss_header hdr;
                struct uss_handle *uh = uss_open(
                    uss_path, true, anchor_pre->sha3_hash, &hdr,
                    err, sizeof(err));
                if (uh) {
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "[local_ingest] phase 2 fast path: sidecar "
                            "%s SHA3 matches anchor (count=%" PRIu64
                            " total=%.4f ZCL)\n",
                            uss_path, hdr.count,
                            (double)hdr.total_supply / 1e8);
                    struct coins_view_sqlite *cvs2 =
                        process_block_get_coins_sqlite();
                    if (!cvs2 || !cvs2->db) {
                        uss_close(uh);
                        state_set_error("phase2 fast-path: "
                                        "coins_view_sqlite unavailable");
                        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                                   "phase2 fast-path: coins_view_sqlite "
                                   "handle missing");
                    }
                    /* Batched bulk insert — 5000 records per
                     * transaction keeps memory bounded and gives the
                     * WAL a chance to checkpoint between batches. */
                    enum { BATCH = 5000 };
                    struct utxo_bulk_rec *batch =
                        zcl_malloc(sizeof(*batch) * BATCH,
                                   "phase2.fast.batch");
                    if (!batch) {
                        uss_close(uh);
                        state_set_error("phase2 fast-path: oom batch");
                        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                                   "phase2 fast-path: oom");
                    }
                    int64_t inserted = 0;
                    int errors = 0;
                    /* Iterate records, flushing each BATCH. */
                    struct fast_phase2_ctx fc = {
                        .batch = batch,
                        .cvs = cvs2,
                        .batch_cap = BATCH,
                    };
                    (void)uss_iter(uh, phase2_fast_bulk_cb, &fc);
                    if (fc.fill > 0 && fc.errors == 0) {
                        int64_t w = coins_view_sqlite_bulk_insert(
                            fc.cvs, fc.batch, fc.fill);
                        if (w != (int64_t)fc.fill) fc.errors++;
                        fc.inserted += (w > 0 ? w : 0);
                    }
                    inserted = fc.inserted;
                    errors = fc.errors;
                    /* batch buffer was allocated above as part of the
                     * BATCH path; free it before exiting this scope. */
                    free(batch);
                    uss_close(uh);
                    if (errors || (uint64_t)inserted != hdr.count) {
                        state_set_error("phase2 fast-path: bulk insert "
                                        "errors");
                        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                                   "phase2 fast-path: inserted=%" PRId64
                                   " expected=%" PRIu64 " errors=%d",
                                   inserted, hdr.count, errors);
                    }
                    atomic_store(&g_state.utxos_imported, inserted);
                    struct uint256 anchor_hash;
                    memcpy(anchor_hash.data,
                           anchor_pre->block_hash, 32);
                    enum local_ingest_result cr =
                        phase2_commit_anchor_via_csr(
                            ms, &anchor_hash, anchor_pre->height,
                            inserted,
                            "local_ingest.sidecar_anchor");
                    if (cr != LCI_OK)
                        return cr;
                    fprintf(stderr, // obs-ok:pre-existing-diagnostic
                            "[local_ingest] phase 2 FAST PATH complete: "
                            "%" PRId64 " UTXOs from sidecar (~1 s "
                            "vs ~30-60 s iteration)\n", inserted);
                    return LCI_OK;
                }
                /* Sidecar present but didn't verify against anchor —
                 * may be a non-anchor-height build. Fall through to
                 * chainstate iter path. */
                fprintf(stderr, // obs-ok:pre-existing-diagnostic
                        "[local_ingest] phase 2: sidecar %s present "
                        "but uss_open failed (%s); falling back to "
                        "chainstate iter\n", uss_path, err);
            }
        }
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

    struct uint256 anchor_hash;
    memcpy(anchor_hash.data, anchor->block_hash, 32);

    /* Verify imported counts against the anchor.  This is a cheap
     * sanity check before we attempt the expensive SHA3-set verify
     * (which requires the data to be in the SQLite UTXO table — that
     * happens on the next batch flush in chain_advance). */
    if ((uint64_t)ctx.vouts != anchor->utxo_count) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "[local_ingest] phase2: WARNING vout count mismatch "
                "imported=%" PRId64 " anchor=%" PRIu64
                " (legacy chainstate is at a different height than the static "
                "anchor; will retry verify against the SHA3 hash, which is the "
                "binding commitment).\n",
                ctx.vouts, anchor->utxo_count);
        /* This is NOT yet fatal — only the SHA3 verify is.  Counts
         * differ if the legacy node has advanced past h=3,056,758. */
    }

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[local_ingest] phase2: imported records=%" PRId64
            " vouts=%" PRId64 " total=%.4f ZCL (anchor expects %" PRIu64
            " UTXOs, %.4f ZCL)\n",
            ctx.records, ctx.vouts,
            (double)ctx.total_value_sat / 1e8,
            anchor->utxo_count,
            (double)anchor->total_supply / 1e8);

    /* ── T1.5: post-import SHA3 cross-check ─────────────────────
     * Flush the cache to coins.db (the import is durable now —
     * a crash during phase 3 cannot lose the UTXO set), then
     * stream-hash the SQLite UTXO table in canonical order.
     *
     * If shape (count + total supply) matches the static anchor's,
     * the import is verifiably at h=3,056,758 and we can hard-fail
     * on SHA3 mismatch. When the legacy node is past the anchor
     * height the shape differs — we still log the digest so an
     * operator can verify out-of-band and so the future T3.2
     * fingerprint can be built on this primitive. */
    if (!coins_view_cache_flush(coins_tip)) {
        state_set_error("phase2: post-import cache flush failed");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: coins_view_cache_flush failed after import");
    }

    struct coins_view_sqlite *cvs = process_block_get_coins_sqlite();
    if (!cvs || !cvs->db) {
        state_set_error("phase2: coins_view_sqlite unavailable");
        LOG_RETURN(LCI_INTERNAL_ERROR, "local_ingest",
                   "phase2: process_block_get_coins_sqlite returned NULL handle");
    }

    uint8_t computed_sha3[32];
    uint64_t computed_count = 0;
    utxo_commitment_sha3_compute(cvs->db, computed_sha3, &computed_count);

    char sha3_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha3_hex + 2 * i, 3, "%02x", computed_sha3[i]);
    sha3_hex[64] = '\0';

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "[local_ingest] phase2: imported UTXO SHA3=%s count=%" PRIu64 "\n",
            sha3_hex, computed_count);

    bool shape_matches_anchor =
        (computed_count == anchor->utxo_count) &&
        ((int64_t)ctx.total_value_sat == anchor->total_supply);

    if (!shape_matches_anchor) {
        state_set_error("phase2: imported chainstate shape does not match anchor");
        LOG_RETURN(LCI_CHAINSTATE_MISMATCH, "local_ingest",
                   "phase2: imported chainstate shape does not match anchor "
                   "(count=%" PRIu64 "/%" PRIu64 " total=%" PRId64 "/%" PRId64 ")",
                   computed_count, anchor->utxo_count,
                   ctx.total_value_sat, anchor->total_supply);
    }

    if (memcmp(computed_sha3, anchor->sha3_hash, 32) == 0) {
        event_emitf(EV_UTXO_CHECKPOINT_PASS, 0,
                    "height=%d count=%" PRIu64,
                    anchor->height, computed_count);
        fprintf(stderr,
                "[local_ingest] phase2: SHA3 anchor verified at h=%d\n",
                anchor->height);
    } else {
        char expected_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(expected_hex + 2 * i, 3, "%02x",
                     anchor->sha3_hash[i]);
        expected_hex[64] = '\0';
        event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                    "height=%d expected=%s got=%s",
                    anchor->height, expected_hex, sha3_hex);
        state_set_error("phase2: SHA3 anchor mismatch");
        LOG_RETURN(LCI_CHAINSTATE_MISMATCH, "local_ingest",
                   "phase2: SHA3 anchor mismatch at h=%d "
                   "expected=%s got=%s",
                   anchor->height, expected_hex, sha3_hex);
    }

    enum local_ingest_result cr =
        phase2_commit_anchor_via_csr(ms, &anchor_hash, anchor->height,
                                     (int64_t)computed_count,
                                     "local_ingest.chainstate_anchor");
    if (cr != LCI_OK)
        return cr;

    /* T3.2: persist fingerprint so next boot can skip phase 2.  Best-
     * effort — write failure is non-fatal. */
    struct chainstate_fingerprint fp = {
        .schema           = LCI_FINGERPRINT_SCHEMA,
        .anchor_height    = anchor->height,
        .utxos_count      = computed_count,
        .total_supply_sat = ctx.total_value_sat,
    };
    memcpy(fp.anchor_block_hash, anchor->block_hash, 32);
    memcpy(fp.sha3_utxo_hash, computed_sha3, 32);
    chainstate_fingerprint_write(our_datadir, &fp);

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

    /* ── T1.1: pre-populate block_index from zclassicd ───────────
     * Headers go into block_map + pindex_best_header so the rest of
     * the system (block_sync_service, MMB, FlyClient) has the full
     * header chain without waiting on P2P header sync. This does NOT
     * activate the headers into chain_active — that requires block
     * data, which P2P still provides. The win: by the time phase 3
     * begins the per-block walk, every header anchor+1..remote_tip is
     * known locally and PoW-verified.
     *
     * Each header is validated locally via accept_block_header
     * (Equihash + nBits lineage + checkpoints), so a malicious
     * zclassicd cannot inject a forged header. If zclassicd is
     * unreachable, this is a no-op and P2P fills the gap. */
    int phase3_remote_tip = -1;
    {
        struct header_probe_config hp_cfg = {0};
        if (header_probe_init(&hp_cfg, ms, params)) {
            int hp_added = 0;
            int active_tip = active_chain_height(&ms->chain_active);
            int from = (active_tip > anchor_h ? active_tip : anchor_h) + 1;
            if (from < 1) from = 1;
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] phase3-pre: header_probe pull "
                    "from h=%d ...\n", from);
            bool reached_tip = header_probe_pull_range_blocking(
                from, &hp_added, &phase3_remote_tip);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] phase3-pre: pulled %d headers "
                    "(remote_tip=%d, reached_tip=%s)\n",
                    hp_added, phase3_remote_tip,
                    reached_tip ? "yes" : "no");
        } else {
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] phase3-pre: header_probe_init "
                    "unavailable; skipping bulk header pull (P2P will "
                    "fill the gap)\n");
        }
    }

    /* ── Durable body-pull (P6) ──────────────────────────────────
     * If our active chain trails the legacy node's tip, fetch the
     * missing bodies directly over loopback JSON-RPC.
     * legacy_body_pull's process_new_block path writes each block
     * to disk AND triggers activate_best_chain, so the active tip
     * extends as we go.
     *
     * Upper bound is the remote_tip returned by header_probe — NOT
     * pindex_best_header, which doesn't move on accept_block_header
     * (only csr_commit_tip promotes it). */
    {
        int active_tip_pre = active_chain_height(&ms->chain_active);
        if (phase3_remote_tip > active_tip_pre) {
            int bp_from = active_tip_pre + 1;
            int bp_to = phase3_remote_tip;
            if (cfg->max_height > 0 && cfg->max_height < bp_to)
                bp_to = cfg->max_height;
            int bp_applied = 0;
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] phase3-pre: legacy_body_pull "
                    "[%d..%d] (active_tip=%d remote_tip=%d)\n",
                    bp_from, bp_to, active_tip_pre, phase3_remote_tip);
            bool bp_ok = legacy_body_pull_range_blocking(
                ms, coins_tip, params, our_datadir,
                bp_from, bp_to, &bp_applied);
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
                    "[local_ingest] phase3-pre: legacy_body_pull "
                    "applied=%d ok=%s\n",
                    bp_applied, bp_ok ? "yes" : "no");
        }
    }

    int tip_h = active_chain_height(&ms->chain_active);
    int final_h = (cfg->max_height > 0 && cfg->max_height < tip_h)
                  ? cfg->max_height : tip_h;
    if (final_h <= anchor_h) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
             * off. legacy_body_pull above is the durable backstop;
             * if we still hit a gap here it means the legacy node
             * doesn't have it either (or RPC failed mid-window). */
            fprintf(stderr, // obs-ok:pre-existing-diagnostic
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

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
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

    enum local_ingest_result r = phase1_sha3_window_verify(cfg, our_datadir);
    if (r != LCI_OK) {
        atomic_store(&g_state.result, (int)r);
        atomic_store(&g_state.finished_at, (int64_t)time(NULL));
        return r;
    }

    r = phase2_chainstate_import(cfg, ms, coins_tip, our_datadir);
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
