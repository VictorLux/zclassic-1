/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Integrity — see block_index_integrity.h for rationale.
 *
 * Implementation notes
 * --------------------
 * The body hash is computed in 1 MiB streaming chunks so we don't
 * need to mmap the whole file (typical block_index.bin is a few
 * hundred MB; we want to run on 8 GB hosts without blowing the
 * RSS on a boot-time check).
 *
 * SQLite cross-check semantics
 * ----------------------------
 * `declared_tip` is the in-memory block_index node that the loader
 * thinks is the tip of the indexed chain. We then ask SQLite:
 *   - "Do you have a row with this exact hash?"  → if no, return
 *      BII_TIP_MISSING_IN_SQL. This is the case that would trip
 *      the 2026-04-10 wipe: the flat file points at a tip SQLite
 *      doesn't believe in.
 *   - "What height do you hold for that row?" → if no match,
 *      return BII_TIP_HEIGHT_MISMATCH. Catches the symmetric bug
 *      where the row exists but the heights have drifted apart.
 *
 * These checks are additive to the chain_state_repository guard —
 * CSR catches the mutation attempt, bii catches the load attempt.
 */

#include "services/block_index_integrity.h"

#include "crypto/sha3.h"
#include "util/safe_alloc.h"
#include "event/event.h"
#include "core/uint256.h"
#include "validation/main_state.h"
#include "chain/pow.h"
#include "storage/disk_block_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

/* ── Layout check ───────────────────────────────────────────── */

struct bii_sidecar_header {
    uint8_t  magic[4];
    uint32_t version;
    uint64_t body_size;
    uint8_t  body_sha3[32];
};

_Static_assert(sizeof(struct bii_sidecar_header) == BII_SIDECAR_BYTES,
               "BII_SIDECAR_BYTES must match sidecar header layout");

/* ── Verdict names ──────────────────────────────────────────── */

const char *bii_verdict_name(enum bii_verdict v)
{
    switch (v) {
    case BII_OK:                   return "ok";
    case BII_SIDECAR_MISSING:      return "sidecar_missing";
    case BII_SIDECAR_STALE:        return "sidecar_stale";
    case BII_HASH_MISMATCH:        return "hash_mismatch";
    case BII_TIP_HEIGHT_MISMATCH:  return "tip_height_mismatch";
    case BII_TIP_MISSING_IN_SQL:   return "tip_missing_in_sql";
    case BII_BODY_MISSING:         return "body_missing";
    case BII_BODY_UNREADABLE:      return "body_unreadable";
    case BII_SIDECAR_BAD_MAGIC:    return "sidecar_bad_magic";
    case BII_SIDECAR_UNSUPPORTED:  return "sidecar_unsupported";
    default:                       return "unknown";
    }
}

/* ── Path helpers ───────────────────────────────────────────── */

static void bii_body_path(char *out, size_t cap, const char *datadir)
{
    snprintf(out, cap, "%s/block_index.bin", datadir);
}

static void bii_sidecar_path(char *out, size_t cap, const char *datadir)
{
    snprintf(out, cap, "%s/block_index.bin.sha3", datadir);
}

/* ── Streaming body hash ────────────────────────────────────── */

static bool bii_hash_body(const char *body_path,
                          uint8_t out_hash[32],
                          uint64_t *out_size)
{
    FILE *f = fopen(body_path, "rb");
    if (!f) return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    enum { BUF_SIZE = 1u << 20 };  /* 1 MiB */
    uint8_t *buf = zcl_malloc(BUF_SIZE, "integrity_hash_buf");
    if (!buf) { fclose(f); LOG_FAIL("block_integrity", "hash_block_body: malloc(%u) failed", (unsigned)BUF_SIZE); }

    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, BUF_SIZE, f)) > 0) {
        sha3_256_write(&ctx, buf, n);
        total += n;
    }
    bool io_err = ferror(f) != 0;
    free(buf);
    fclose(f);
    if (io_err) return false;

    sha3_256_finalize(&ctx, out_hash);
    if (out_size) *out_size = total;
    return true;
}

/* ── Sidecar writer ─────────────────────────────────────────── */

bool bii_write_sidecar(const char *datadir)
{
    if (!datadir) return false;

    char body_path[1024];
    char side_path[1024];
    char tmp_path[1056];
    bii_body_path(body_path, sizeof(body_path), datadir);
    bii_sidecar_path(side_path, sizeof(side_path), datadir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", side_path);

    struct stat st;
    if (stat(body_path, &st) != 0) {
        fprintf(stderr, "bii_write_sidecar: stat %s: %s\n",
                body_path, strerror(errno));
        return false;
    }

    struct bii_sidecar_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, BII_MAGIC, 4);
    hdr.version = BII_SIDECAR_VERSION;
    hdr.body_size = (uint64_t)st.st_size;

    uint64_t hashed_size = 0;
    if (!bii_hash_body(body_path, hdr.body_sha3, &hashed_size)) {
        fprintf(stderr, "bii_write_sidecar: hash body failed\n");
        return false;
    }
    /* stat size and streamed size must agree — disagreement means
     * something is truncating the file concurrently, which is a
     * bigger problem than this function can solve. */
    if (hashed_size != hdr.body_size) {
        fprintf(stderr,
                "bii_write_sidecar: size drift stat=%llu hashed=%llu\n",
                (unsigned long long)hdr.body_size,
                (unsigned long long)hashed_size);
        return false;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        fprintf(stderr, "bii_write_sidecar: fopen %s: %s\n",
                tmp_path, strerror(errno));
        return false;
    }
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "bii_write_sidecar: fwrite failed\n");
        fclose(f);
        unlink(tmp_path);
        return false;
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);

    if (rename(tmp_path, side_path) != 0) {
        fprintf(stderr, "bii_write_sidecar: rename %s -> %s: %s\n",
                tmp_path, side_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

/* ── Sidecar reader ─────────────────────────────────────────── */

static enum bii_verdict bii_read_sidecar(const char *side_path,
                                          struct bii_sidecar_header *out)
{
    FILE *f = fopen(side_path, "rb");
    if (!f) {
        if (errno == ENOENT) return BII_SIDECAR_MISSING;
        return BII_BODY_UNREADABLE;
    }
    size_t n = fread(out, 1, sizeof(*out), f);
    bool io_err = ferror(f) != 0;
    fclose(f);
    if (io_err || n != sizeof(*out))
        return BII_SIDECAR_STALE;
    if (memcmp(out->magic, BII_MAGIC, 4) != 0)
        return BII_SIDECAR_BAD_MAGIC;
    if (out->version != BII_SIDECAR_VERSION)
        return BII_SIDECAR_UNSUPPORTED;
    return BII_OK;
}

/* ── SQLite cross-check ─────────────────────────────────────── */

static enum bii_verdict bii_check_tip_in_sql(struct node_db *db,
                                               const struct block_index *tip)
{
    if (!db || !db->open || !db->db || !tip || !tip->phashBlock)
        return BII_OK;  /* nothing to cross-check */

    sqlite3_stmt *st = NULL;
    enum bii_verdict verdict = BII_OK;
    if (sqlite3_prepare_v2(db->db,
            "SELECT height FROM blocks WHERE hash=?", -1, &st, NULL) != SQLITE_OK)
        return BII_OK;  /* schema may not be ready — defer to CSR */

    sqlite3_bind_blob(st, 1, tip->phashBlock->data, 32, SQLITE_STATIC);
    int rc = AR_STEP_ROW_READONLY(st);
    if (rc == SQLITE_ROW) {
        int64_t sql_h = sqlite3_column_int64(st, 0);
        if (sql_h != (int64_t)tip->nHeight)
            verdict = BII_TIP_HEIGHT_MISMATCH;
    } else {
        verdict = BII_TIP_MISSING_IN_SQL;
    }
    sqlite3_finalize(st);
    return verdict;
}

/* ── Verification entry point ───────────────────────────────── */

enum bii_verdict bii_verify(const char *datadir,
                             struct node_db *db,
                             const struct block_index *declared_tip,
                             char *err_out, size_t err_cap)
{
    if (err_out && err_cap) err_out[0] = '\0';
    if (!datadir) {
        if (err_out) snprintf(err_out, err_cap, "null datadir");
        return BII_BODY_UNREADABLE;
    }

    char body_path[1024];
    char side_path[1024];
    bii_body_path(body_path, sizeof(body_path), datadir);
    bii_sidecar_path(side_path, sizeof(side_path), datadir);

    /* Body presence. */
    struct stat body_st;
    if (stat(body_path, &body_st) != 0) {
        if (err_out) snprintf(err_out, err_cap,
                "block_index.bin: %s", strerror(errno));
        return errno == ENOENT ? BII_BODY_MISSING : BII_BODY_UNREADABLE;
    }

    /* Sidecar read. */
    struct bii_sidecar_header hdr;
    enum bii_verdict rv = bii_read_sidecar(side_path, &hdr);
    if (rv == BII_SIDECAR_MISSING) {
        if (err_out) snprintf(err_out, err_cap,
                "no sidecar at %s (first run after upgrade?)", side_path);
        /* Even without a sidecar the SQLite cross-check is still
         * useful — the 2026-04-10 bug would trip the tip check
         * regardless. */
        enum bii_verdict sql = bii_check_tip_in_sql(db, declared_tip);
        if (sql != BII_OK) {
            if (err_out) {
                char tmp[256];
                snprintf(tmp, sizeof(tmp),
                         "sidecar missing; SQLite cross-check: %s",
                         bii_verdict_name(sql));
                snprintf(err_out, err_cap, "%s", tmp);
            }
            return sql;
        }
        return BII_SIDECAR_MISSING;
    }
    if (rv != BII_OK) {
        if (err_out) snprintf(err_out, err_cap,
                "sidecar read: %s", bii_verdict_name(rv));
        return rv;
    }

    /* Size check before expensive hash. */
    if (hdr.body_size != (uint64_t)body_st.st_size) {
        if (err_out) snprintf(err_out, err_cap,
                "size drift: sidecar=%llu actual=%lld",
                (unsigned long long)hdr.body_size,
                (long long)body_st.st_size);
        return BII_SIDECAR_STALE;
    }

    /* Full hash. */
    uint8_t actual_hash[32];
    uint64_t hashed_size = 0;
    if (!bii_hash_body(body_path, actual_hash, &hashed_size)) {
        if (err_out) snprintf(err_out, err_cap,
                "failed to hash %s: %s", body_path, strerror(errno));
        return BII_BODY_UNREADABLE;
    }
    if (hashed_size != hdr.body_size) {
        if (err_out) snprintf(err_out, err_cap,
                "size drift mid-hash: sidecar=%llu hashed=%llu",
                (unsigned long long)hdr.body_size,
                (unsigned long long)hashed_size);
        return BII_SIDECAR_STALE;
    }
    if (memcmp(actual_hash, hdr.body_sha3, 32) != 0) {
        if (err_out) {
            char exp[65], got[65];
            for (int i = 0; i < 32; i++) {
                sprintf(exp + i*2, "%02x", hdr.body_sha3[i]);
                sprintf(got + i*2, "%02x", actual_hash[i]);
            }
            snprintf(err_out, err_cap,
                    "body sha3 mismatch expected=%s actual=%s",
                    exp, got);
        }
        return BII_HASH_MISMATCH;
    }

    /* SQLite cross-check. */
    enum bii_verdict sql = bii_check_tip_in_sql(db, declared_tip);
    if (sql != BII_OK) {
        if (err_out && declared_tip) snprintf(err_out, err_cap,
                "tip h=%d: %s", declared_tip->nHeight,
                bii_verdict_name(sql));
        return sql;
    }

    return BII_OK;
}

/* ── Quarantine ─────────────────────────────────────────────── */

static void bii_rename_if_present(const char *src, int64_t ts,
                                    const char *label)
{
    struct stat st;
    if (stat(src, &st) != 0) return;  /* nothing to do */

    char dst[1200];
    snprintf(dst, sizeof(dst), "%s.corrupt.%lld", src, (long long)ts);
    if (rename(src, dst) != 0) {
        fprintf(stderr,
                "bii_quarantine: rename %s -> %s failed: %s\n",
                src, dst, strerror(errno));
        return;
    }
    printf("bii: quarantined %s -> %s (%s)\n", src, dst, label);
}

void bii_quarantine_corrupt(const char *datadir, enum bii_verdict v)
{
    if (!datadir) return;
    int64_t ts = (int64_t)time(NULL);

    char body_path[1024];
    char side_path[1024];
    bii_body_path(body_path, sizeof(body_path), datadir);
    bii_sidecar_path(side_path, sizeof(side_path), datadir);

    bii_rename_if_present(body_path, ts, bii_verdict_name(v));
    bii_rename_if_present(side_path, ts, bii_verdict_name(v));

    event_emitf(EV_BLOCK_INDEX_CORRUPT, 0,
                "verdict=%s ts=%lld",
                bii_verdict_name(v), (long long)ts);
}

/* ── Bulk block index height repair ────────────────────────── */

static _Atomic bool g_heights_repaired = false;

bool block_index_heights_repaired(void)
{
    return atomic_load(&g_heights_repaired);
}

/* Comparator for sorting block_index pointers by height (ascending). */
static int bii_cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    return (pa->nHeight > pb->nHeight) - (pa->nHeight < pb->nHeight);
}

int block_index_repair_heights(struct main_state *ms)
{
    if (!ms) return 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t n = ms->map_block_index.size;
    if (n == 0) {
        atomic_store(&g_heights_repaired, true);
        return 0;
    }

    /* Pass 1: count entries with wrong heights. */
    int wrong = 0;
    {
        size_t iter = 0;
        struct block_index *pi;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
            if (!pi) continue;
            if (pi->pprev && pi->nHeight != pi->pprev->nHeight + 1)
                wrong++;
            else if (!pi->pprev && pi->nHeight != 0)
                wrong++;
        }
    }

    if (wrong == 0) {
        printf("[height-repair] all %zu block index heights correct\n", n);
        fflush(stdout);
        atomic_store(&g_heights_repaired, true);
        return 0;
    }

    printf("[height-repair] found %d/%zu entries with wrong heights, repairing...\n",
           wrong, n);
    fflush(stdout);

    /* Collect all entries into an array for sorting. */
    struct block_index **arr = zcl_malloc(n * sizeof(*arr), "height_repair_arr");
    if (!arr) {
        fprintf(stderr, "[height-repair] malloc failed for %zu entries\n", n);
        return 0;
    }

    size_t iter = 0, idx = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (pi && idx < n)
            arr[idx++] = pi;
    }
    n = idx;

    /* Pass 2: Fix heights via multi-pass forward propagation.
     * Each pass sets height = pprev->height + 1 for entries where pprev
     * already has the correct height.  Converges in O(max_depth) passes
     * but typically 1-3 for a well-linked chain. */
    int repaired = 0;
    for (int pass = 0; pass < 20; pass++) {
        int fixed_this_pass = 0;
        for (size_t i = 0; i < n; i++) {
            struct block_index *b = arr[i];
            if (!b->pprev) {
                /* Genesis: height must be 0 */
                if (b->nHeight != 0) {
                    b->nHeight = 0;
                    fixed_this_pass++;
                }
                continue;
            }
            int expected = b->pprev->nHeight + 1;
            if (b->nHeight != expected) {
                b->nHeight = expected;
                fixed_this_pass++;
            }
        }
        repaired += fixed_this_pass;
        if (fixed_this_pass == 0)
            break;
    }

    /* Pass 3: Recompute nChainWork now that heights are correct.
     * Must sort by height first for correct forward propagation. */
    qsort(arr, n, sizeof(*arr), bii_cmp_height);

    for (size_t i = 0; i < n; i++) {
        struct block_index *b = arr[i];
        struct arith_uint256 proof = GetBlockProof(b);
        if (b->pprev)
            arith_uint256_add(&b->nChainWork, &b->pprev->nChainWork, &proof);
        else
            b->nChainWork = proof;
    }

    free(arr);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                       + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    printf("[height-repair] repaired %d heights in %lld ms\n",
           repaired, (long long)elapsed_ms);
    fflush(stdout);

    event_emitf(EV_BLOCK_INDEX_REPAIR, 0,
                "repaired=%d elapsed_ms=%lld",
                repaired, (long long)elapsed_ms);

    atomic_store(&g_heights_repaired, true);
    return repaired;
}

/* ── pprev chain repair ────────────────────────────────────────
 * After LDB import, the flat file may store wrong prev_hash values
 * (copied from pprev->phashBlock when pprev was already corrupted).
 * This function reads hashPrevBlock directly from block data on disk
 * for every entry with BLOCK_HAVE_DATA and fixes pprev if it points
 * to the wrong parent.
 *
 * Call AFTER block_index_repair_heights() so heights are correct.
 * After pprev repair, recomputes nChainWork and nChainTx. */
int block_index_repair_pprev(struct main_state *ms, const char *datadir)
{
    if (!ms || !datadir) return 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t n = ms->map_block_index.size;
    if (n == 0) return 0;

    /* Collect entries with BLOCK_HAVE_DATA into an array sorted by
     * (nFile, nDataPos) so we read each block file sequentially. */
    struct block_index **arr = zcl_malloc(n * sizeof(*arr), "pprev_repair_arr");
    if (!arr) {
        fprintf(stderr, "[pprev-repair] malloc failed for %zu entries\n", n);
        return 0;
    }

    size_t iter = 0, count = 0;
    struct block_index *pi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
        if (pi && pi->nHeight > 0 && (pi->nStatus & BLOCK_HAVE_DATA) &&
            pi->nFile >= 0 && pi->nDataPos > 0)
            arr[count++] = pi;
    }

    int repaired = 0, read_errors = 0;

    for (size_t i = 0; i < count; i++) {
        struct block_index *bi = arr[i];

        /* Read just nVersion (4 bytes) + hashPrevBlock (32 bytes) = 36 bytes
         * from the start of block data on disk. This is much faster than
         * deserializing the entire block. */
        uint8_t hdr_buf[36];
        struct disk_block_pos pos = {
            .nFile = bi->nFile,
            .nPos = bi->nDataPos
        };
        ssize_t nr = disk_block_pread(datadir, &pos, "blk", hdr_buf, 36);
        if (nr < 36) {
            read_errors++;
            continue;
        }

        /* hashPrevBlock is at offset 4 (after nVersion) in little-endian */
        struct uint256 prev_hash;
        memcpy(prev_hash.data, hdr_buf + 4, 32);

        /* Look up the correct parent in the block map */
        struct block_index *correct_pprev =
            block_map_find(&ms->map_block_index, &prev_hash);

        if (!correct_pprev)
            continue; /* parent not in index — can't fix */

        if (bi->pprev != correct_pprev) {
            bi->pprev = correct_pprev;
            repaired++;
        }
    }

    /* After fixing pprev, recompute nChainWork and nChainTx.
     * Sort all entries by height for forward propagation. */
    if (repaired > 0) {
        /* Re-collect ALL entries (not just HAVE_DATA) for chain recomputation */
        size_t all_count = 0;
        struct block_index **all = zcl_malloc(n * sizeof(*all), "pprev_repair_all");
        if (all) {
            iter = 0;
            while (block_map_next(&ms->map_block_index, &iter, NULL, &pi)) {
                if (pi && all_count < n)
                    all[all_count++] = pi;
            }
            qsort(all, all_count, sizeof(*all), bii_cmp_height);

            int fixed_work = 0, fixed_tx = 0;
            for (size_t i = 0; i < all_count; i++) {
                struct block_index *b = all[i];
                struct arith_uint256 proof = GetBlockProof(b);
                struct arith_uint256 expected;

                if (b->pprev) {
                    arith_uint256_add(&expected, &b->pprev->nChainWork, &proof);
                    if (arith_uint256_compare(&expected, &b->nChainWork) != 0) {
                        b->nChainWork = expected;
                        fixed_work++;
                    }
                    if (b->nTx > 0 && b->pprev->nChainTx > 0) {
                        uint32_t expected_ctx = b->pprev->nChainTx + b->nTx;
                        if (b->nChainTx != expected_ctx) {
                            b->nChainTx = expected_ctx;
                            fixed_tx++;
                        }
                    }
                } else {
                    b->nChainWork = proof;
                }
            }
            free(all);

            if (fixed_work > 0 || fixed_tx > 0)
                printf("[pprev-repair] recomputed: %d chain_work, %d chain_tx\n",
                       fixed_work, fixed_tx);
        }
    }

    free(arr);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                       + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    printf("[pprev-repair] fixed %d pprev links (%d read errors) "
           "from %zu blocks with data in %lld ms\n",
           repaired, read_errors, count, (long long)elapsed_ms);
    fflush(stdout);

    if (repaired > 0)
        event_emitf(EV_BLOCK_INDEX_REPAIR, 0,
                    "pprev_repaired=%d read_errors=%d elapsed_ms=%lld",
                    repaired, read_errors, (long long)elapsed_ms);

    return repaired;
}
