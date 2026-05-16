/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Addrman Integrity — see addrman_integrity.h for rationale.
 *
 * Deliberately a near-clone of block_index_integrity.c so the
 * two sidecars behave identically. Differences:
 *
 *   - sidecar magic is "ADIX" vs "BIIX"
 *   - no SQLite cross-check (peers.dat has no on-chain peer)
 *   - quarantine emits EV_ADDRMAN_CORRUPT instead of EV_BLOCK_INDEX_CORRUPT
 *
 * The streaming hash uses a 1 MiB window so we don't need to
 * mmap or load the whole file. peers.dat is tiny in practice
 * (<< 1 MB) but the same code path is used by tests that may
 * synthesise larger inputs.
 */

#include "services/addrman_integrity.h"

#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* ── Layout check ───────────────────────────────────────────── */

struct aii_sidecar_header {
    uint8_t  magic[4];
    uint32_t version;
    uint64_t body_size;
    uint8_t  body_sha3[32];
};

_Static_assert(sizeof(struct aii_sidecar_header) == AII_SIDECAR_BYTES,
               "AII_SIDECAR_BYTES must match sidecar header layout");

/* ── Verdict names ──────────────────────────────────────────── */

const char *aii_verdict_name(enum aii_verdict v)
{
    switch (v) {
    case AII_OK:                  return "ok";
    case AII_SIDECAR_MISSING:     return "sidecar_missing";
    case AII_SIDECAR_STALE:       return "sidecar_stale";
    case AII_HASH_MISMATCH:       return "hash_mismatch";
    case AII_BODY_MISSING:        return "body_missing";
    case AII_BODY_UNREADABLE:     return "body_unreadable";
    case AII_SIDECAR_BAD_MAGIC:   return "sidecar_bad_magic";
    case AII_SIDECAR_UNSUPPORTED: return "sidecar_unsupported";
    default:                      return "unknown";
    }
}

/* ── Path helpers ───────────────────────────────────────────── */

static void aii_body_path(char *out, size_t cap, const char *datadir)
{
    snprintf(out, cap, "%s/peers.dat", datadir);
}

static void aii_sidecar_path(char *out, size_t cap, const char *datadir)
{
    snprintf(out, cap, "%s/peers.dat.sha3", datadir);
}

/* ── Streaming body hash ────────────────────────────────────── */

static bool aii_hash_body(const char *body_path,
                          uint8_t out_hash[32],
                          uint64_t *out_size)
{
    FILE *f = fopen(body_path, "rb");
    if (!f) return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    enum { BUF_SIZE = 1u << 20 };  /* 1 MiB */
    uint8_t *buf = zcl_malloc(BUF_SIZE, "aii hash body buf");
    if (!buf) { fclose(f); return false; }

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

bool aii_write_sidecar(const char *datadir)
{
    if (!datadir) return false;

    char body_path[1024];
    char side_path[1024];
    char tmp_path[1056];
    aii_body_path(body_path, sizeof(body_path), datadir);
    aii_sidecar_path(side_path, sizeof(side_path), datadir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", side_path);

    struct stat st;
    if (stat(body_path, &st) != 0)
        LOG_FAIL("aii", "stat %s: %s", body_path, strerror(errno));

    struct aii_sidecar_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, AII_MAGIC, 4);
    hdr.version = AII_SIDECAR_VERSION;
    hdr.body_size = (uint64_t)st.st_size;

    uint64_t hashed_size = 0;
    if (!aii_hash_body(body_path, hdr.body_sha3, &hashed_size))
        LOG_FAIL("aii", "hash body failed");
    if (hashed_size != hdr.body_size)
        LOG_FAIL("aii", "size drift stat=%llu hashed=%llu",
                 (unsigned long long)hdr.body_size,
                 (unsigned long long)hashed_size);

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        LOG_FAIL("aii", "fopen %s: %s", tmp_path, strerror(errno));
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "aii_write_sidecar: fwrite failed\n");
        fclose(f);
        unlink(tmp_path);
        return false;
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);

    if (rename(tmp_path, side_path) != 0) {
        fprintf(stderr, "aii_write_sidecar: rename %s -> %s: %s\n",
                tmp_path, side_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

/* ── Sidecar reader ─────────────────────────────────────────── */

static enum aii_verdict aii_read_sidecar(const char *side_path,
                                          struct aii_sidecar_header *out)
{
    FILE *f = fopen(side_path, "rb");
    if (!f) {
        if (errno == ENOENT) return AII_SIDECAR_MISSING;
        return AII_BODY_UNREADABLE;
    }
    size_t n = fread(out, 1, sizeof(*out), f);
    bool io_err = ferror(f) != 0;
    fclose(f);
    if (io_err || n != sizeof(*out))
        return AII_SIDECAR_STALE;
    if (memcmp(out->magic, AII_MAGIC, 4) != 0)
        return AII_SIDECAR_BAD_MAGIC;
    if (out->version != AII_SIDECAR_VERSION)
        return AII_SIDECAR_UNSUPPORTED;
    return AII_OK;
}

/* ── Verification entry point ───────────────────────────────── */

enum aii_verdict aii_verify(const char *datadir,
                             char *err_out, size_t err_cap)
{
    if (err_out && err_cap) err_out[0] = '\0';
    if (!datadir) {
        if (err_out) snprintf(err_out, err_cap, "null datadir");
        return AII_BODY_UNREADABLE;
    }

    char body_path[1024];
    char side_path[1024];
    aii_body_path(body_path, sizeof(body_path), datadir);
    aii_sidecar_path(side_path, sizeof(side_path), datadir);

    struct stat body_st;
    if (stat(body_path, &body_st) != 0) {
        if (err_out) snprintf(err_out, err_cap,
                "peers.dat: %s", strerror(errno));
        return errno == ENOENT ? AII_BODY_MISSING : AII_BODY_UNREADABLE;
    }

    struct aii_sidecar_header hdr;
    enum aii_verdict rv = aii_read_sidecar(side_path, &hdr);
    if (rv == AII_SIDECAR_MISSING) {
        if (err_out) snprintf(err_out, err_cap,
                "no sidecar at %s (first run after upgrade?)", side_path);
        return AII_SIDECAR_MISSING;
    }
    if (rv != AII_OK) {
        if (err_out) snprintf(err_out, err_cap,
                "sidecar read: %s", aii_verdict_name(rv));
        return rv;
    }

    if (hdr.body_size != (uint64_t)body_st.st_size) {
        if (err_out) snprintf(err_out, err_cap,
                "size drift: sidecar=%llu actual=%lld",
                (unsigned long long)hdr.body_size,
                (long long)body_st.st_size);
        return AII_SIDECAR_STALE;
    }

    uint8_t actual_hash[32];
    uint64_t hashed_size = 0;
    if (!aii_hash_body(body_path, actual_hash, &hashed_size)) {
        if (err_out) snprintf(err_out, err_cap,
                "failed to hash %s: %s", body_path, strerror(errno));
        return AII_BODY_UNREADABLE;
    }
    if (hashed_size != hdr.body_size) {
        if (err_out) snprintf(err_out, err_cap,
                "size drift mid-hash: sidecar=%llu hashed=%llu",
                (unsigned long long)hdr.body_size,
                (unsigned long long)hashed_size);
        return AII_SIDECAR_STALE;
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
        return AII_HASH_MISMATCH;
    }

    return AII_OK;
}

/* ── Quarantine ─────────────────────────────────────────────── */

static void aii_rename_if_present(const char *src, int64_t ts,
                                    const char *label)
{
    struct stat st;
    if (stat(src, &st) != 0) return;

    char dst[1200];
    snprintf(dst, sizeof(dst), "%s.corrupt.%lld", src, (long long)ts);
    if (rename(src, dst) != 0) {
        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                "aii_quarantine: rename %s -> %s failed: %s\n",
                src, dst, strerror(errno));
        return;
    }
    printf("aii: quarantined %s -> %s (%s)\n", src, dst, label);
}

void aii_quarantine_corrupt(const char *datadir, enum aii_verdict v)
{
    if (!datadir) return;
    int64_t ts = (int64_t)time(NULL);

    char body_path[1024];
    char side_path[1024];
    aii_body_path(body_path, sizeof(body_path), datadir);
    aii_sidecar_path(side_path, sizeof(side_path), datadir);

    aii_rename_if_present(body_path, ts, aii_verdict_name(v));
    aii_rename_if_present(side_path, ts, aii_verdict_name(v));

    event_emitf(EV_ADDRMAN_CORRUPT, 0,
                "verdict=%s ts=%lld",
                aii_verdict_name(v), (long long)ts);
}
