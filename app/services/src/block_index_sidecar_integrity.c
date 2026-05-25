/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Sidecar Integrity — hash sidecar verification for
 * block_index.bin. Split from block_index_integrity.c so the sidecar
 * verifier and structural repair paths stay separately readable. */

#include "platform/time_compat.h"
#include "services/block_index_integrity.h"

#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

struct bii_sidecar_header {
    uint8_t  magic[4];
    uint32_t version;
    uint64_t body_size;
    uint8_t  body_sha3[32];
};

_Static_assert(sizeof(struct bii_sidecar_header) == BII_SIDECAR_BYTES,
               "BII_SIDECAR_BYTES must match sidecar header layout");

static void bii_body_path(char *out, size_t cap, const char *datadir)
{
    snprintf(out, cap, "%s/block_index.bin", datadir);
}

static void bii_sidecar_path(char *out, size_t cap, const char *datadir)
{
    snprintf(out, cap, "%s/block_index.bin.sha3", datadir);
}

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
    if (!buf) {
        fclose(f);
        LOG_FAIL("block_integrity", "hash_block_body: malloc(%u) failed",
                 (unsigned)BUF_SIZE);
    }

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
    if (stat(body_path, &st) != 0)
        LOG_FAIL("bii", "stat %s: %s", body_path, strerror(errno));

    struct bii_sidecar_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, BII_MAGIC, 4);
    hdr.version = BII_SIDECAR_VERSION;
    hdr.body_size = (uint64_t)st.st_size;

    uint64_t hashed_size = 0;
    if (!bii_hash_body(body_path, hdr.body_sha3, &hashed_size))
        LOG_FAIL("bii", "hash body failed");
    /* stat size and streamed size must agree — disagreement means
     * something is truncating the file concurrently, which is a
     * bigger problem than this function can solve. */
    if (hashed_size != hdr.body_size) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
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
            HexStr(hdr.body_sha3, 32, false, exp, sizeof(exp));
            HexStr(actual_hash, 32, false, got, sizeof(got));
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

static void bii_rename_if_present(const char *src, int64_t ts,
                                    const char *label)
{
    struct stat st;
    if (stat(src, &st) != 0) return;  /* nothing to do */

    char dst[1200];
    snprintf(dst, sizeof(dst), "%s.corrupt.%lld", src, (long long)ts);
    if (rename(src, dst) != 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "bii_quarantine: rename %s -> %s failed: %s\n",
                src, dst, strerror(errno));
        return;
    }
    printf("bii: quarantined %s -> %s (%s)\n", src, dst, label);
}

void bii_quarantine_corrupt(const char *datadir, enum bii_verdict v)
{
    if (!datadir) return;
    int64_t ts = (int64_t)platform_time_wall_time_t();

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
