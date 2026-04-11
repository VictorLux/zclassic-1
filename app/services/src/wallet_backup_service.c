/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet Backup Service — see header for rationale.
 *
 * Implementation strategy
 * -----------------------
 *
 * SQLite's online backup API (sqlite3_backup_init) copies the
 * whole database. We want only the wallet tables, so instead we
 * open the destination file as a fresh DB, ATTACH the source
 * via its on-disk path, and `CREATE TABLE <name> AS SELECT * FROM
 * src.<name>` for each wallet table. That keeps the destination
 * file small (users typically have a handful of wallet rows, not
 * the full 3M-row blocks table) and avoids copying UTXO data
 * that would leak peer-observable chain state to the backup.
 *
 * The ATTACH path must be absolute — sqlite3_db_filename returns
 * it for an opened connection, so we read that off the source
 * handle at backup time instead of asking the caller to thread
 * it through the config.
 *
 * Row-count verification
 * ----------------------
 *
 * After the CREATE TABLE AS SELECT statements run, we reopen the
 * destination in a second connection and count wallet_keys rows.
 * That round-trip proves the file is readable, the schema is as
 * expected, and the same number of keys landed as we thought we
 * wrote. Mismatches set last_error and emit
 * EV_WALLET_BACKUP_FAILED; the file is NOT deleted — operators
 * need the bytes even when verification fails.
 */

#include "services/wallet_backup_service.h"

#include "event/event.h"
#include "core/random.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/pbkdf2_sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

/* ── Wallet table list ──────────────────────────────────────── */

static const char *const WALLET_TABLES[] = {
    "wallet_keys",
    "wallet_sapling_keys",
    "wallet_seed",
    "wallet_scripts",
    "wallet_transactions",
    "wallet_utxos",
    "wallet_sapling_notes",
};

#define WALLET_TABLE_COUNT (sizeof(WALLET_TABLES) / sizeof(WALLET_TABLES[0]))

/* ── Module state ───────────────────────────────────────────── */

struct wallet_backup_service_state {
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            thread_running;
    bool            stop_requested;

    struct wallet_backup_config cfg;
    struct node_db             *db;

    /* Snapshot counters */
    int64_t total_runs;
    int64_t total_failures;
    int64_t last_run_unix;
    int64_t last_size_bytes;
    int64_t last_key_count;
    int64_t last_duration_ms;
    char    last_path[512];
    char    last_error[256];
};

static struct wallet_backup_service_state g_wbs = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ── Helpers ────────────────────────────────────────────────── */

static int64_t wbs_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t wbs_now_unix(void)
{
    return (int64_t)time(NULL);
}

void wallet_backup_config_defaults(struct wallet_backup_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval_seconds = WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
    cfg->max_versions     = WALLET_BACKUP_DEFAULT_MAX_VERSIONS;
    cfg->encrypt          = false;
}

void wallet_backup_status_snapshot(struct wallet_backup_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_wbs.lock);
    out->running          = g_wbs.thread_running;
    out->total_runs       = g_wbs.total_runs;
    out->total_failures   = g_wbs.total_failures;
    out->last_run_unix    = g_wbs.last_run_unix;
    out->last_size_bytes  = g_wbs.last_size_bytes;
    out->last_key_count   = g_wbs.last_key_count;
    out->last_duration_ms = g_wbs.last_duration_ms;
    snprintf(out->last_path,  sizeof(out->last_path),  "%s", g_wbs.last_path);
    snprintf(out->last_error, sizeof(out->last_error), "%s", g_wbs.last_error);
    pthread_mutex_unlock(&g_wbs.lock);
}

/* Create backup_dir with mode 0700 if missing. Returns true if the
 * directory exists on successful return. */
static bool wbs_ensure_backup_dir(const char *dir)
{
    if (!dir || !*dir) return false;
    struct stat st;
    if (stat(dir, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (mkdir(dir, 0700) != 0) {
        fprintf(stderr, "wallet_backup: mkdir %s: %s\n",
                dir, strerror(errno));
        return false;
    }
    return true;
}

/* Return the on-disk path backing the source sqlite connection.
 * Returns NULL for memory databases. */
static const char *wbs_source_path(sqlite3 *src)
{
    if (!src) return NULL;
    const char *p = sqlite3_db_filename(src, "main");
    if (!p || !*p) return NULL;
    return p;
}

/* SHA-style filename: wallet_backup_<unix_ts>_<usec>.sqlite. The
 * usec disambiguates rapid successive runs (tests call
 * run_once several times back-to-back). */
static void wbs_build_backup_path(const char *dir, char *out, size_t cap)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(out, cap, "%s/%s%lld_%06ld%s",
             dir,
             WALLET_BACKUP_FILENAME_PREFIX,
             (long long)tv.tv_sec,
             (long)tv.tv_usec,
             WALLET_BACKUP_FILENAME_SUFFIX);
}

/* ── Row-count reader ──────────────────────────────────────── */

static int64_t wbs_count_rows(sqlite3 *db, const char *table)
{
    if (!db || !table) return -1;
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

/* ── Core primitive ─────────────────────────────────────────── */

bool wallet_backup_run_once(const char *backup_dir,
                             struct node_db *db,
                             char *out_path, size_t out_path_cap,
                             int64_t *out_key_count,
                             char *err_out, size_t err_cap)
{
    if (err_out && err_cap) err_out[0] = '\0';
    if (out_path && out_path_cap) out_path[0] = '\0';
    if (out_key_count) *out_key_count = -1;

    if (!backup_dir || !db || !db->open || !db->db) {
        if (err_out) snprintf(err_out, err_cap, "null arg or db not open");
        return false;
    }

    if (!wbs_ensure_backup_dir(backup_dir)) {
        if (err_out) snprintf(err_out, err_cap,
                "cannot create backup_dir %s", backup_dir);
        return false;
    }

    const char *src_path = wbs_source_path(db->db);
    if (!src_path) {
        if (err_out) snprintf(err_out, err_cap,
                "source db has no file path (in-memory?)");
        return false;
    }

    /* In-memory source is valid for tests: use the ATTACH TO
     * "file::memory:?cache=shared" form only if the caller opened
     * it with a real filename. Here we simply require a disk file
     * — tests that want to exercise the primitive use a tmpdir. */

    char dst_path[640];
    wbs_build_backup_path(backup_dir, dst_path, sizeof(dst_path));

    /* Open the destination as a fresh empty db. */
    sqlite3 *dst = NULL;
    int rc = sqlite3_open_v2(dst_path, &dst,
        SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        if (err_out) snprintf(err_out, err_cap,
                "sqlite3_open dst: %s", sqlite3_errmsg(dst));
        if (dst) sqlite3_close(dst);
        unlink(dst_path);
        return false;
    }

    /* ATTACH the source by absolute path under alias "src". */
    {
        sqlite3_stmt *att = NULL;
        rc = sqlite3_prepare_v2(dst,
            "ATTACH DATABASE ? AS src", -1, &att, NULL);
        if (rc != SQLITE_OK || !att) {
            if (err_out) snprintf(err_out, err_cap,
                    "prepare ATTACH: %s", sqlite3_errmsg(dst));
            if (att) sqlite3_finalize(att);
            sqlite3_close(dst);
            unlink(dst_path);
            return false;
        }
        sqlite3_bind_text(att, 1, src_path, -1, SQLITE_STATIC);
        if (sqlite3_step(att) != SQLITE_DONE) {
            if (err_out) snprintf(err_out, err_cap,
                    "step ATTACH: %s", sqlite3_errmsg(dst));
            sqlite3_finalize(att);
            sqlite3_close(dst);
            unlink(dst_path);
            return false;
        }
        sqlite3_finalize(att);
    }

    /* For each wallet table, run CREATE TABLE IF NOT EXISTS t AS
     * SELECT ... The AS SELECT form copies both schema and rows
     * in one statement; if the source table is missing we just
     * skip it (older databases may not have every table). */
    char *errmsg = NULL;
    bool all_ok = true;
    for (size_t i = 0; i < WALLET_TABLE_COUNT; i++) {
        const char *table = WALLET_TABLES[i];
        /* Check the source even has this table. */
        char exists_sql[256];
        snprintf(exists_sql, sizeof(exists_sql),
            "SELECT name FROM src.sqlite_master "
            "WHERE type='table' AND name='%s'", table);
        sqlite3_stmt *chk = NULL;
        bool src_has = false;
        if (sqlite3_prepare_v2(dst, exists_sql, -1, &chk, NULL) == SQLITE_OK) {
            src_has = sqlite3_step(chk) == SQLITE_ROW;
            sqlite3_finalize(chk);
        }
        if (!src_has) continue;

        char sql[256];
        snprintf(sql, sizeof(sql),
            "CREATE TABLE %s AS SELECT * FROM src.%s", table, table);
        rc = sqlite3_exec(dst, sql, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            if (err_out) snprintf(err_out, err_cap,
                    "copy %s: %s", table, errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
            errmsg = NULL;
            all_ok = false;
            break;
        }
    }

    /* Detach + close. */
    (void)sqlite3_exec(dst, "DETACH DATABASE src", NULL, NULL, NULL);
    sqlite3_close(dst);

    if (!all_ok) {
        /* Leave the dst file on disk for forensics, but emit the
         * failure event and bail out. */
        struct stat st;
        int64_t bytes = stat(dst_path, &st) == 0 ? (int64_t)st.st_size : -1;
        event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                    "path=%s bytes=%lld reason=%s",
                    dst_path, (long long)bytes,
                    err_out ? err_out : "unknown");
        return false;
    }

    /* Round-trip verification: reopen the backup file read-only,
     * count the wallet_keys rows, and compare against the source.
     * If the counts differ the file is left on disk but we return
     * false so the caller knows not to trust it. */
    int64_t src_key_count = wbs_count_rows(db->db, "wallet_keys");
    int64_t dst_key_count = -1;
    {
        sqlite3 *verify = NULL;
        if (sqlite3_open_v2(dst_path, &verify,
                SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            dst_key_count = wbs_count_rows(verify, "wallet_keys");
            sqlite3_close(verify);
        }
    }

    if (dst_key_count < 0 || dst_key_count != src_key_count) {
        if (err_out) snprintf(err_out, err_cap,
                "verify row count mismatch src=%lld dst=%lld",
                (long long)src_key_count, (long long)dst_key_count);
        event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                    "path=%s reason=%s",
                    dst_path, err_out ? err_out : "count_mismatch");
        return false;
    }

    struct stat st;
    int64_t bytes = stat(dst_path, &st) == 0 ? (int64_t)st.st_size : -1;
    event_emitf(EV_WALLET_BACKUP, 0,
                "path=%s bytes=%lld keys=%lld",
                dst_path, (long long)bytes, (long long)dst_key_count);

    if (out_path) snprintf(out_path, out_path_cap, "%s", dst_path);
    if (out_key_count) *out_key_count = dst_key_count;

    return true;
}

/* ── Phase 2: encryption helpers ────────────────────────────── */

/* The existing chacha20poly1305_encrypt/_decrypt helpers in
 * lib/crypto use a 2048-byte stack buffer for the Poly1305 MAC
 * input, which is fine for Sapling notes and P2P handshakes but
 * too small for wallet backup SQLite files. Re-implement the
 * RFC 7539 AEAD construction locally with a heap buffer so we
 * can AEAD arbitrarily-sized messages without touching the
 * shared crypto code. */
static void wbs_pad16_len(size_t n, size_t *pad_out)
{
    *pad_out = (16 - (n % 16)) % 16;
}

static void wbs_store64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static bool wbs_aead_encrypt(const uint8_t *plain, size_t plain_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t nonce[12],
                              const uint8_t key[32],
                              uint8_t *ciphertext_out,
                              uint8_t  tag_out[16])
{
    /* Derive the Poly1305 one-time key from ChaCha20 block 0. */
    uint8_t poly_block[64];
    chacha20_block(key, 0, nonce, poly_block);

    /* Encrypt with ChaCha20 starting at counter 1. */
    chacha20_encrypt(key, 1, nonce, plain, plain_len, ciphertext_out);

    /* Build the Poly1305 message:
     *   aad || pad16(aad) || ciphertext || pad16(ciphertext) ||
     *   le64(aad_len) || le64(plain_len)
     */
    size_t pad_aad, pad_ct;
    wbs_pad16_len(aad_len,    &pad_aad);
    wbs_pad16_len(plain_len,  &pad_ct);

    size_t mac_len = aad_len + pad_aad + plain_len + pad_ct + 16;
    uint8_t *mac_buf = calloc(1, mac_len > 0 ? mac_len : 1);
    if (!mac_buf) {
        memset(poly_block, 0, sizeof(poly_block));
        return false;
    }
    size_t pos = 0;
    if (aad_len)    { memcpy(mac_buf + pos, aad, aad_len); pos += aad_len; }
    pos += pad_aad; /* calloc zeroed, no action needed */
    if (plain_len)  { memcpy(mac_buf + pos, ciphertext_out, plain_len); pos += plain_len; }
    pos += pad_ct;
    wbs_store64_le(mac_buf + pos, (uint64_t)aad_len);   pos += 8;
    wbs_store64_le(mac_buf + pos, (uint64_t)plain_len); pos += 8;

    poly1305_mac(mac_buf, pos, poly_block, tag_out);

    memset(mac_buf,    0, mac_len);
    memset(poly_block, 0, sizeof(poly_block));
    free(mac_buf);
    return true;
}

static bool wbs_aead_decrypt(const uint8_t *ciphertext, size_t plain_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *tag,
                              const uint8_t nonce[12],
                              const uint8_t key[32],
                              uint8_t *plain_out)
{
    uint8_t poly_block[64];
    chacha20_block(key, 0, nonce, poly_block);

    size_t pad_aad, pad_ct;
    wbs_pad16_len(aad_len,   &pad_aad);
    wbs_pad16_len(plain_len, &pad_ct);
    size_t mac_len = aad_len + pad_aad + plain_len + pad_ct + 16;
    uint8_t *mac_buf = calloc(1, mac_len > 0 ? mac_len : 1);
    if (!mac_buf) {
        memset(poly_block, 0, sizeof(poly_block));
        return false;
    }
    size_t pos = 0;
    if (aad_len)   { memcpy(mac_buf + pos, aad, aad_len); pos += aad_len; }
    pos += pad_aad;
    if (plain_len) { memcpy(mac_buf + pos, ciphertext, plain_len); pos += plain_len; }
    pos += pad_ct;
    wbs_store64_le(mac_buf + pos, (uint64_t)aad_len);   pos += 8;
    wbs_store64_le(mac_buf + pos, (uint64_t)plain_len); pos += 8;

    uint8_t computed[16];
    poly1305_mac(mac_buf, pos, poly_block, computed);

    memset(mac_buf,    0, mac_len);
    memset(poly_block, 0, sizeof(poly_block));
    free(mac_buf);

    /* Constant-time tag compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= tag[i] ^ computed[i];
    memset(computed, 0, sizeof(computed));
    if (diff != 0) return false;

    /* Tag verified — now decrypt. */
    chacha20_encrypt(key, 1, nonce, ciphertext, plain_len, plain_out);
    return true;
}

/* Read the whole file at `path` into a freshly malloc'd buffer.
 * On success `*out_buf` / `*out_len` are set and the caller owns
 * the buffer. On failure both are zeroed and false is returned. */
static bool wbs_read_whole_file(const char *path,
                                 uint8_t **out_buf, size_t *out_len)
{
    if (out_buf) *out_buf = NULL;
    if (out_len) *out_len = 0;
    if (!path || !out_buf || !out_len) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long ls = ftell(f);
    if (ls < 0) { fclose(f); return false; }
    rewind(f);

    size_t n = (size_t)ls;
    uint8_t *buf = NULL;
    if (n > 0) {
        buf = malloc(n);
        if (!buf) { fclose(f); return false; }
        if (fread(buf, 1, n, f) != n) {
            free(buf);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    *out_buf = buf;
    *out_len = n;
    return true;
}

/* Write `buf/len` to `path` atomically: write to a sibling
 * `.tmp` file, fsync, then rename over the final path. Returns
 * true on success. */
static bool wbs_write_file_atomic(const char *path,
                                   const uint8_t *buf, size_t len)
{
    if (!path || (!buf && len > 0)) return false;

    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f); unlink(tmp); return false;
    }
    if (fflush(f) != 0) { fclose(f); unlink(tmp); return false; }
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);

    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

bool wallet_backup_encrypt_file(const char *src_path,
                                 const char *dst_path,
                                 const char *password)
{
    if (!src_path || !dst_path || !password || !*password)
        return false;

    uint8_t *plain = NULL;
    size_t   plen  = 0;
    if (!wbs_read_whole_file(src_path, &plain, &plen))
        return false;

    /* Fresh salt + nonce from the system CSPRNG. */
    uint8_t salt[WALLET_BACKUP_ENC_SALT_LEN];
    uint8_t nonce[WALLET_BACKUP_ENC_NONCE_LEN];
    GetRandBytes(salt,  sizeof(salt));
    GetRandBytes(nonce, sizeof(nonce));

    /* Derive the AEAD key. */
    uint8_t key[WALLET_BACKUP_ENC_KEY_LEN];
    pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password),
                        salt, sizeof(salt),
                        WALLET_BACKUP_ENC_ITERATIONS,
                        key, sizeof(key));

    /* Build the header (also serves as AAD). */
    uint8_t header[WALLET_BACKUP_ENC_HEADER_LEN];
    memset(header, 0, sizeof(header));
    memcpy(header, WALLET_BACKUP_ENC_MAGIC, 4);
    uint32_t ver = WALLET_BACKUP_ENC_VERSION;
    uint32_t its = WALLET_BACKUP_ENC_ITERATIONS;
    header[4]  = (uint8_t)(ver);
    header[5]  = (uint8_t)(ver >>  8);
    header[6]  = (uint8_t)(ver >> 16);
    header[7]  = (uint8_t)(ver >> 24);
    header[8]  = (uint8_t)(its);
    header[9]  = (uint8_t)(its >>  8);
    header[10] = (uint8_t)(its >> 16);
    header[11] = (uint8_t)(its >> 24);
    /* reserved [12..16] stays zero */
    memcpy(header + 16, salt,  sizeof(salt));
    memcpy(header + 32, nonce, sizeof(nonce));

    /* Allocate the output buffer: header + ciphertext + tag. */
    size_t out_len = sizeof(header) + plen + WALLET_BACKUP_ENC_TAG_LEN;
    uint8_t *out = malloc(out_len);
    if (!out) {
        free(plain);
        memset(key, 0, sizeof(key));
        return false;
    }
    memcpy(out, header, sizeof(header));

    /* Encrypt into out + header, tag follows. */
    bool ok = wbs_aead_encrypt(plain, plen,
                                header, sizeof(header),
                                nonce, key,
                                out + sizeof(header),
                                out + sizeof(header) + plen);
    /* Scrub sensitive material promptly. */
    memset(key, 0, sizeof(key));
    memset(plain, 0, plen);
    free(plain);

    if (!ok) {
        free(out);
        return false;
    }

    bool wrote = wbs_write_file_atomic(dst_path, out, out_len);
    free(out);
    return wrote;
}

bool wallet_backup_decrypt_file(const char *src_path,
                                 const char *dst_path,
                                 const char *password)
{
    if (!src_path || !dst_path || !password || !*password)
        return false;

    uint8_t *enc = NULL;
    size_t   elen = 0;
    if (!wbs_read_whole_file(src_path, &enc, &elen))
        return false;

    if (elen < (size_t)(WALLET_BACKUP_ENC_HEADER_LEN +
                         WALLET_BACKUP_ENC_TAG_LEN)) {
        free(enc); return false;
    }

    if (memcmp(enc, WALLET_BACKUP_ENC_MAGIC, 4) != 0) {
        free(enc); return false;
    }

    uint32_t ver = (uint32_t)enc[4]       |
                   ((uint32_t)enc[5] <<  8) |
                   ((uint32_t)enc[6] << 16) |
                   ((uint32_t)enc[7] << 24);
    if (ver != WALLET_BACKUP_ENC_VERSION) {
        free(enc); return false;
    }
    uint32_t its = (uint32_t)enc[8]       |
                   ((uint32_t)enc[9]  <<  8) |
                   ((uint32_t)enc[10] << 16) |
                   ((uint32_t)enc[11] << 24);
    if (its == 0 || its > (1u << 24)) { /* sanity cap */
        free(enc); return false;
    }

    const uint8_t *salt  = enc + 16;
    const uint8_t *nonce = enc + 32;

    uint8_t key[WALLET_BACKUP_ENC_KEY_LEN];
    pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password),
                        salt, WALLET_BACKUP_ENC_SALT_LEN,
                        its, key, sizeof(key));

    size_t body_len  = elen - WALLET_BACKUP_ENC_HEADER_LEN;
    size_t plain_len = body_len - WALLET_BACKUP_ENC_TAG_LEN;

    uint8_t *plain = NULL;
    if (plain_len > 0) {
        plain = malloc(plain_len);
        if (!plain) {
            free(enc);
            memset(key, 0, sizeof(key));
            return false;
        }
    }

    const uint8_t *ciphertext = enc + WALLET_BACKUP_ENC_HEADER_LEN;
    const uint8_t *tag        = ciphertext + plain_len;
    bool ok = wbs_aead_decrypt(ciphertext, plain_len,
                                enc, WALLET_BACKUP_ENC_HEADER_LEN,
                                tag, nonce, key, plain);
    memset(key, 0, sizeof(key));
    free(enc);

    if (!ok) {
        if (plain) memset(plain, 0, plain_len);
        free(plain);
        return false;
    }

    bool wrote = wbs_write_file_atomic(dst_path, plain, plain_len);
    if (plain) memset(plain, 0, plain_len);
    free(plain);
    return wrote;
}

/* ── Rotation / listing ─────────────────────────────────────── */

struct wbs_file {
    char    name[256];
    int64_t mtime;
};

static int wbs_cmp_mtime_desc(const void *a, const void *b)
{
    const struct wbs_file *fa = a;
    const struct wbs_file *fb = b;
    if (fa->mtime > fb->mtime) return -1;
    if (fa->mtime < fb->mtime) return 1;
    return 0;
}

static int wbs_scan_backup_dir(const char *dir,
                                struct wbs_file *out, int max)
{
    if (!dir || !out || max <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (strncmp(e->d_name, WALLET_BACKUP_FILENAME_PREFIX,
                    strlen(WALLET_BACKUP_FILENAME_PREFIX)) != 0)
            continue;
        size_t nl = strlen(e->d_name);
        size_t sl = strlen(WALLET_BACKUP_FILENAME_SUFFIX);
        if (nl < sl || strcmp(e->d_name + nl - sl,
                               WALLET_BACKUP_FILENAME_SUFFIX) != 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].mtime = (int64_t)st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(struct wbs_file), wbs_cmp_mtime_desc);
    return n;
}

int wallet_backup_list(const char *backup_dir,
                        char (*out_paths)[512], int max)
{
    struct wbs_file files[256];
    int n = wbs_scan_backup_dir(backup_dir, files,
        max < (int)(sizeof(files) / sizeof(files[0]))
            ? max : (int)(sizeof(files) / sizeof(files[0])));
    for (int i = 0; i < n; i++)
        snprintf(out_paths[i], 512, "%s/%s", backup_dir, files[i].name);
    return n;
}

int wallet_backup_rotate(const char *backup_dir, int max_versions)
{
    if (max_versions <= 0) return 0;
    struct wbs_file files[256];
    int n = wbs_scan_backup_dir(backup_dir, files, 256);
    int deleted = 0;
    for (int i = max_versions; i < n; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", backup_dir, files[i].name);
        if (unlink(full) == 0)
            deleted++;
    }
    return deleted;
}

/* ── Synchronous entry points ───────────────────────────────── */

static bool wbs_run_one_locked(void)
{
    int64_t started_ms = wbs_now_ms();
    char path[512] = "";
    char err[256]  = "";
    int64_t key_count = -1;
    bool ok = wallet_backup_run_once(g_wbs.cfg.backup_dir, g_wbs.db,
                                      path, sizeof(path),
                                      &key_count,
                                      err, sizeof(err));
    int64_t elapsed = wbs_now_ms() - started_ms;

    if (ok) {
        g_wbs.total_runs++;
        g_wbs.last_run_unix    = wbs_now_unix();
        g_wbs.last_key_count   = key_count;
        g_wbs.last_duration_ms = elapsed;
        snprintf(g_wbs.last_path, sizeof(g_wbs.last_path), "%s", path);
        g_wbs.last_error[0] = '\0';
        struct stat st;
        g_wbs.last_size_bytes =
            stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
        /* Rotate after success — never lose the newest backup. */
        int max = g_wbs.cfg.max_versions > 0
            ? g_wbs.cfg.max_versions
            : WALLET_BACKUP_DEFAULT_MAX_VERSIONS;
        (void)wallet_backup_rotate(g_wbs.cfg.backup_dir, max);
    } else {
        g_wbs.total_failures++;
        snprintf(g_wbs.last_error, sizeof(g_wbs.last_error), "%s", err);
    }
    return ok;
}

bool wallet_backup_now(void)
{
    pthread_mutex_lock(&g_wbs.lock);
    if (!g_wbs.db || !g_wbs.cfg.backup_dir) {
        pthread_mutex_unlock(&g_wbs.lock);
        return false;
    }
    bool ok = wbs_run_one_locked();
    pthread_mutex_unlock(&g_wbs.lock);
    return ok;
}

/* ── Thread loop ────────────────────────────────────────────── */

static void wbs_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *wbs_thread_fn(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_wbs.lock);
    int interval = g_wbs.cfg.interval_seconds > 0
        ? g_wbs.cfg.interval_seconds
        : WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
    pthread_mutex_unlock(&g_wbs.lock);

    /* Do one immediate backup on start so the user always has a
     * fresh copy within a few seconds of boot — the worst failure
     * is the boot that hasn't reached its first hourly tick yet. */
    (void)wallet_backup_now();

    int64_t next_at_ms = wbs_now_ms() + (int64_t)interval * 1000;
    while (true) {
        pthread_mutex_lock(&g_wbs.lock);
        bool stop = g_wbs.stop_requested;
        pthread_mutex_unlock(&g_wbs.lock);
        if (stop) break;

        if (wbs_now_ms() >= next_at_ms) {
            (void)wallet_backup_now();
            /* Re-read interval in case config was updated. */
            pthread_mutex_lock(&g_wbs.lock);
            interval = g_wbs.cfg.interval_seconds > 0
                ? g_wbs.cfg.interval_seconds
                : WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
            pthread_mutex_unlock(&g_wbs.lock);
            next_at_ms = wbs_now_ms() + (int64_t)interval * 1000;
        }
        /* Sleep in small increments so stop_requested is honoured
         * without waiting up to `interval` seconds. */
        wbs_sleep_ms(200);
    }

    pthread_mutex_lock(&g_wbs.lock);
    g_wbs.thread_running = false;
    pthread_mutex_unlock(&g_wbs.lock);
    return NULL;
}

bool wallet_backup_start(const struct wallet_backup_config *cfg,
                          struct node_db *db)
{
    if (!cfg || !db || !cfg->backup_dir) return false;

    pthread_mutex_lock(&g_wbs.lock);
    if (g_wbs.thread_running) {
        pthread_mutex_unlock(&g_wbs.lock);
        return true;
    }

    /* Refuse to back up into the same datadir as the source — the
     * whole point is an *external* copy. We detect this by
     * comparing the backup_dir to the directory containing the
     * source db file. */
    const char *src_path = wbs_source_path(db->db);
    if (src_path) {
        char src_dir[1024];
        snprintf(src_dir, sizeof(src_dir), "%s", src_path);
        char *slash = strrchr(src_dir, '/');
        if (slash) *slash = '\0';
        if (strcmp(src_dir, cfg->backup_dir) == 0) {
            pthread_mutex_unlock(&g_wbs.lock);
            fprintf(stderr,
                "wallet_backup: refusing to back up into source dir %s\n",
                src_dir);
            return false;
        }
    }

    if (!wbs_ensure_backup_dir(cfg->backup_dir)) {
        pthread_mutex_unlock(&g_wbs.lock);
        return false;
    }

    g_wbs.cfg = *cfg;
    g_wbs.db = db;
    g_wbs.stop_requested = false;
    g_wbs.thread_running = true;

    int rc = pthread_create(&g_wbs.thread, NULL, wbs_thread_fn, NULL);
    if (rc != 0) {
        g_wbs.thread_running = false;
        pthread_mutex_unlock(&g_wbs.lock);
        fprintf(stderr, "wallet_backup: pthread_create failed (%d)\n", rc);
        return false;
    }
    pthread_mutex_unlock(&g_wbs.lock);
    return true;
}

void wallet_backup_stop(void)
{
    pthread_t th;
    bool joinable = false;
    pthread_mutex_lock(&g_wbs.lock);
    if (g_wbs.thread_running) {
        g_wbs.stop_requested = true;
        th = g_wbs.thread;
        joinable = true;
    }
    pthread_mutex_unlock(&g_wbs.lock);

    if (joinable) {
        pthread_join(th, NULL);
        pthread_mutex_lock(&g_wbs.lock);
        g_wbs.thread_running = false;
        g_wbs.stop_requested = false;
        g_wbs.db = NULL;
        pthread_mutex_unlock(&g_wbs.lock);
    }
}
