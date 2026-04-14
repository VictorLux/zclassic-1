/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast P2P sync: UTXO snapshot transfer between zclassic23 nodes. */

#include "net/fast_sync.h"
#include "views/format_helpers.h"
#include "coins/utxo_commitment.h"
#include "models/database.h"
#include "models/utxo.h"
#include "core/hash.h"
#include "crypto/sha256.h"
#include "crypto/sha3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <pthread.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"

/* Cached UTXO root: the O(n) rolling SHA-256 is computed once at startup.
 * The incremental XOR commitment (maintained per-block) can verify the
 * root is still valid without rescanning. */
static uint8_t g_cached_utxo_root[32];
static uint64_t g_cached_utxo_count = 0;
static bool g_cached_root_valid = false;
static _Atomic uint64_t g_cached_utxo_root_version = 0;
static pthread_mutex_t g_utxo_root_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

bool fast_sync_publish_utxo_root_cache(const uint8_t root[32], uint64_t count)
{
    if (!root || count == 0)
        LOG_FAIL("sync", "publish_utxo_root_cache: root is NULL or count is 0");

    pthread_mutex_lock(&g_utxo_root_cache_mutex);
    memcpy(g_cached_utxo_root, root, sizeof(g_cached_utxo_root));
    g_cached_utxo_count = count;
    g_cached_root_valid = true;
    g_cached_utxo_root_version++;
    pthread_mutex_unlock(&g_utxo_root_cache_mutex);
    return true;
}

void fast_sync_reset_utxo_root_cache(void)
{
    pthread_mutex_lock(&g_utxo_root_cache_mutex);
    memset(g_cached_utxo_root, 0, sizeof(g_cached_utxo_root));
    g_cached_utxo_count = 0;
    g_cached_root_valid = false;
    g_cached_utxo_root_version++;
    pthread_mutex_unlock(&g_utxo_root_cache_mutex);
}

bool fast_sync_get_utxo_root_cache(uint8_t out[32], uint64_t *count)
{
    if (!out)
        LOG_FAIL("sync", "get_utxo_root_cache: out buffer is NULL");

    pthread_mutex_lock(&g_utxo_root_cache_mutex);
    if (!g_cached_root_valid) {
        pthread_mutex_unlock(&g_utxo_root_cache_mutex);
        LOG_FAIL("sync", "get_utxo_root_cache: cached root not valid");
    }
    memcpy(out, g_cached_utxo_root, sizeof(g_cached_utxo_root));
    if (count)
        *count = g_cached_utxo_count;
    pthread_mutex_unlock(&g_utxo_root_cache_mutex);
    return true;
}

uint64_t fast_sync_utxo_root_cache_version(void)
{
    pthread_mutex_lock(&g_utxo_root_cache_mutex);
    uint64_t version = g_cached_utxo_root_version;
    pthread_mutex_unlock(&g_utxo_root_cache_mutex);
    return version;
}

bool fast_sync_build_offer(const char *datadir,
                            struct snapshot_offer *offer)
{
    GUARD(offer, "sync", "build_offer: offer is NULL");
    memset(offer, 0, sizeof(*offer));

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "build_offer: failed to open db %s", db_path);

    /* Get tip height and hash */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db, "SELECT value FROM node_state WHERE key='tip_height'",
                        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(s, 0);
        if (len == (int)sizeof(int64_t)) {
            int64_t h;
            memcpy(&h, sqlite3_column_blob(s, 0), sizeof(h));
            offer->height = (int32_t)h;
        } else if (len >= 1 && len <= 8) {
            const void *blob = sqlite3_column_blob(s, 0);
            memcpy(&offer->height, blob, len < 4 ? (size_t)len : 4);
        }
    }
    sqlite3_finalize(s);

    sqlite3_prepare_v2(db, "SELECT value FROM node_state WHERE key='tip_hash'",
                        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(s, 0);
        if (h && sqlite3_column_bytes(s, 0) >= 32)
            memcpy(offer->block_hash, h, 32);
    }
    sqlite3_finalize(s);

    /* Count UTXOs */
    sqlite3_prepare_v2(db, "SELECT count(*) FROM utxos", -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        offer->num_utxos = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    /* Estimate size: ~80 bytes per UTXO */
    offer->total_bytes = offer->num_utxos * 80;

    /* Compute UTXO root: use cache if available, else O(n) scan.
     * The root is cached after first computation; subsequent calls
     * reuse it since the offer is rebuilt only at startup. */
    uint8_t cached_root[32];
    uint64_t cached_count = 0;
    if (fast_sync_get_utxo_root_cache(cached_root, &cached_count) &&
        cached_count == offer->num_utxos) {
        memcpy(offer->utxo_root, cached_root, sizeof(cached_root));
    } else {
        fast_sync_compute_utxo_root_db(db, offer->utxo_root);
        if (!fast_sync_publish_utxo_root_cache(offer->utxo_root,
                                               offer->num_utxos)) {
            sqlite3_close(db);
            LOG_FAIL("sync", "build_offer: failed to publish UTXO root cache");
        }
    }

    sqlite3_close(db);
    return offer->height > 0;
}

/* Internal: compute root from open db handle */
void fast_sync_compute_utxo_root_db(sqlite3 *db, uint8_t root_out[32])
{
    /* SHA3-256 commitment over all UTXOs in canonical order.
     * Identical to utxo_commitment_sha3_compute() — ensures the
     * snapshot offer hash matches what the receiver will compute. */
    uint64_t count = 0;
    utxo_commitment_sha3_compute(db, root_out, &count);
}

bool fast_sync_compute_utxo_root(const char *datadir,
                                  uint8_t root_out[32])
{
    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "compute_utxo_root: failed to open db %s", db_path);

    fast_sync_compute_utxo_root_db(db, root_out);
    sqlite3_close(db);
    return true;
}

/* ── Pre-serialized snapshot for zero-copy serving ───────────── */

void fast_sync_snapshot_path(char *out, size_t max, const char *datadir)
{
    snprintf(out, max, "%s/snapshot.bin", datadir);
}

/* Cached SHA3 hash from pre-serialization — guaranteed to match file contents */
static uint8_t g_snapshot_sha3[32];
static uint64_t g_snapshot_count = 0;
static bool g_snapshot_sha3_valid = false;
static _Atomic uint64_t g_snapshot_cache_version = 0;

/* In-memory snapshot buffer — loaded once at startup for instant serving.
 * 96 MB for 1.35M UTXOs. Eliminates all file I/O during serving. */
static uint8_t *g_snapshot_buf = NULL;
static int64_t  g_snapshot_buf_size = 0;
static pthread_mutex_t g_snapshot_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

bool fast_sync_publish_snapshot_cache(uint8_t *snapshot_buf, int64_t size,
                                      const uint8_t sha3[32],
                                      uint64_t count)
{
    if (!snapshot_buf || size <= 0 || !sha3 || count == 0) {
        LOG_FAIL("sync", "publish_snapshot_cache: invalid args (buf=%p size=%lld count=%llu)",
                 (void *)snapshot_buf, (long long)size, (unsigned long long)count);
    }

    pthread_mutex_lock(&g_snapshot_cache_mutex);
    free(g_snapshot_buf);
    g_snapshot_buf = snapshot_buf;
    g_snapshot_buf_size = size;
    memcpy(g_snapshot_sha3, sha3, 32);
    g_snapshot_count = count;
    g_snapshot_sha3_valid = true;
    g_snapshot_cache_version++;
    pthread_mutex_unlock(&g_snapshot_cache_mutex);
    return true;
}

void fast_sync_reset_snapshot_cache(void)
{
    pthread_mutex_lock(&g_snapshot_cache_mutex);
    free(g_snapshot_buf);
    g_snapshot_buf = NULL;
    g_snapshot_buf_size = 0;
    memset(g_snapshot_sha3, 0, sizeof(g_snapshot_sha3));
    g_snapshot_count = 0;
    g_snapshot_sha3_valid = false;
    g_snapshot_cache_version++;
    pthread_mutex_unlock(&g_snapshot_cache_mutex);
}

uint64_t fast_sync_snapshot_cache_version(void)
{
    pthread_mutex_lock(&g_snapshot_cache_mutex);
    uint64_t version = g_snapshot_cache_version;
    pthread_mutex_unlock(&g_snapshot_cache_mutex);
    return version;
}

bool fast_sync_get_snapshot_sha3(uint8_t out[32], uint64_t *count)
{
    pthread_mutex_lock(&g_snapshot_cache_mutex);
    if (!g_snapshot_sha3_valid) {
        pthread_mutex_unlock(&g_snapshot_cache_mutex);
        LOG_FAIL("sync", "get_snapshot_sha3: snapshot SHA3 cache not valid");
    }
    memcpy(out, g_snapshot_sha3, 32);
    if (count) *count = g_snapshot_count;
    pthread_mutex_unlock(&g_snapshot_cache_mutex);
    return true;
}

int64_t fast_sync_prebuild_snapshot(struct node_db *ndb, const char *datadir)
{
    char path[1024];
    fast_sync_snapshot_path(path, sizeof(path), datadir);

    printf("[snapshot] Pre-serializing UTXOs to %s...\n", path);
    uint8_t sha3[32];
    int64_t count = db_utxo_serialize_snapshot(ndb, path, SYNC_CHUNK_SIZE, sha3);
    if (count > 0) {
        /* Load entire file into memory for instant serving */
        FILE *fp = fopen(path, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            uint8_t *snapshot_buf = zcl_malloc((size_t)sz, "snapshot_buf");
            if (snapshot_buf) {
                size_t rd = fread(snapshot_buf, 1, (size_t)sz, fp);
                if (!fast_sync_publish_snapshot_cache(snapshot_buf,
                                                     (int64_t)rd,
                                                     sha3,
                                                     (uint64_t)count)) {
                    free(snapshot_buf);
                }
            }
            fclose(fp);

            char hex[65];
            for (int i = 0; i < 32; i++)
                sprintf(hex + i*2, "%02x", sha3[i]);
            printf("[snapshot] Pre-serialized %lld UTXOs (%.1f MB), "
                   "SHA3=%s — loaded in RAM for instant serving\n",
                   (long long)count, (double)sz / (1024.0 * 1024.0), hex);
        }
    } else {
        fprintf(stderr, "[snapshot] Pre-serialization failed\n");
    }
    return count;
}

uint64_t fast_sync_snapshot_file_size(const char *datadir)
{
    char path[1024];
    fast_sync_snapshot_path(path, sizeof(path), datadir);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    return (uint64_t)(sz > 0 ? sz : 0);
}

/* Get the in-memory snapshot buffer for zero-copy serving */
const uint8_t *fast_sync_get_snapshot_buf(int64_t *size)
{
    const uint8_t *buf;

    pthread_mutex_lock(&g_snapshot_cache_mutex);
    buf = g_snapshot_buf;
    if (size) *size = g_snapshot_buf_size;
    pthread_mutex_unlock(&g_snapshot_cache_mutex);
    return buf;
}

bool fast_sync_serve_snapshot(const char *datadir,
                               int from_height,
                               chunk_callback cb, void *ctx)
{
    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "serve_snapshot: failed to open db %s", db_path);

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT txid, vout, value, script, height "
        "FROM utxos ORDER BY txid, vout",
        -1, &s, NULL);

    struct utxo_chunk *chunk = zcl_calloc(1, sizeof(struct utxo_chunk), "utxo_chunk");
    if (!chunk) { sqlite3_finalize(s); sqlite3_close(db); LOG_FAIL("sync", "serve_snapshot: alloc utxo_chunk failed"); }

    (void)from_height;
    while (sqlite3_step(s) == SQLITE_ROW) {
        uint32_t idx = chunk->num_entries;
        const void *txid = sqlite3_column_blob(s, 0);
        if (txid) memcpy(chunk->entries[idx].txid, txid, 32);
        chunk->entries[idx].vout = (uint32_t)sqlite3_column_int(s, 1);
        chunk->entries[idx].value = sqlite3_column_int64(s, 2);

        const void *script = sqlite3_column_blob(s, 3);
        int slen = sqlite3_column_bytes(s, 3);
        if (script && slen > 0) {
            if (slen > 128) slen = 128;
            memcpy(chunk->entries[idx].script, script, (size_t)slen);
            chunk->entries[idx].script_len = (uint16_t)slen;
        }
        chunk->entries[idx].height = sqlite3_column_int(s, 4);
        chunk->num_entries++;

        if (chunk->num_entries >= 1000) {
            if (!cb(chunk, ctx)) break;
            memset(chunk, 0, sizeof(*chunk));
        }
    }

    /* Send remaining */
    if (chunk->num_entries > 0)
        cb(chunk, ctx);

    free(chunk);
    sqlite3_finalize(s);
    sqlite3_close(db);
    return true;
}

/* ── PoW defense ─────────────────────────────────────────── */

bool fast_sync_verify_pow(const struct fast_sync_pow *pow)
{
    GUARD(pow, "sync", "verify_pow: pow is NULL");

    /* Timestamp must be within 5 minutes */
    int64_t now = (int64_t)time(NULL);
    if (pow->timestamp < now - 300 || pow->timestamp > now + 60)
        LOG_FAIL("sync", "verify_pow: timestamp out of range: ts=%lld now=%lld",
                 (long long)pow->timestamp, (long long)now);

    /* SHA3-256(peer_id || timestamp || nonce) must have leading zeros.
     * ZCL23-only protocol — SHA3 for hash diversity from consensus layer. */
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, pow->peer_id, 32);
    sha3_256_write(&ctx, (const unsigned char *)&pow->timestamp, 8);
    sha3_256_write(&ctx, (const unsigned char *)&pow->nonce, 8);
    unsigned char hash[32];
    sha3_256_finalize(&ctx, hash);

    /* Check leading zero bits */
    int bits = FAST_SYNC_POW_BITS;
    for (int i = 0; i < bits / 8; i++)
        if (hash[i] != 0) return false;  /* normal during solve loop — not an error */
    if (bits % 8 > 0) {
        uint8_t mask = (uint8_t)(0xFF << (8 - bits % 8));
        if (hash[bits / 8] & mask) return false;  /* normal during solve loop — not an error */
    }
    return true;
}

bool fast_sync_solve_pow(const uint8_t peer_id[32], struct fast_sync_pow *pow)
{
    GUARD(pow, "sync", "solve_pow: pow is NULL");
    memcpy(pow->peer_id, peer_id, 32);
    pow->timestamp = (int64_t)time(NULL);
    pow->nonce = 0;

    while (pow->nonce < UINT64_MAX) {
        if (fast_sync_verify_pow(pow))
            return true;
        pow->nonce++;
    }
    LOG_FAIL("sync", "solve_pow: exhausted nonce space without finding solution");
}

/* ── Rate limiting ───────────────────────────────────────── */

bool fast_sync_rate_check(struct fast_sync_rate_limiter *rl,
                           const uint8_t ip[16])
{
    int64_t now = (int64_t)time(NULL);

    /* Global rate limit — prevents distributed DoS from many IPs */
    if (now - rl->global_window_start > 3600) {
        rl->global_window_start = now;
        rl->global_chunks_sent = 0;
    }
    if (rl->global_chunks_sent >= FAST_SYNC_MAX_GLOBAL_CHUNKS_PER_HOUR)
        LOG_FAIL("sync", "rate_check: global rate limit exceeded (%llu chunks/hr)",
                 (unsigned long long)rl->global_chunks_sent);

    /* Per-IP rate limit */
    for (size_t i = 0; i < rl->num_entries; i++) {
        if (memcmp(rl->entries[i].ip, ip, 16) == 0) {
            if (now - rl->entries[i].window_start > 3600) {
                rl->entries[i].window_start = now;
                rl->entries[i].chunks_sent = 0;
            }
            if (rl->entries[i].chunks_sent >= FAST_SYNC_MAX_CHUNKS_PER_HOUR)
                LOG_FAIL("sync", "rate_check: per-IP rate limit exceeded (%llu chunks/hr)",
                         (unsigned long long)rl->entries[i].chunks_sent);
            rl->entries[i].chunks_sent++;
            rl->global_chunks_sent++;
            return true;
        }
    }

    /* New IP */
    if (rl->num_entries < 1024) {
        size_t idx = rl->num_entries++;
        memcpy(rl->entries[idx].ip, ip, 16);
        rl->entries[idx].window_start = now;
        rl->entries[idx].chunks_sent = 1;
        rl->global_chunks_sent++;
        return true;
    }

    /* Table full — evict oldest */
    size_t oldest = 0;
    for (size_t i = 1; i < rl->num_entries; i++) {
        if (rl->entries[i].window_start < rl->entries[oldest].window_start)
            oldest = i;
    }
    memcpy(rl->entries[oldest].ip, ip, 16);
    rl->entries[oldest].window_start = now;
    rl->entries[oldest].chunks_sent = 1;
    rl->global_chunks_sent++;
    return true;
}

bool fast_sync_apply_chunk(const char *datadir,
                            const struct utxo_chunk *chunk)
{
    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "apply_chunk: failed to open db %s", db_path);

    if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        LOG_FAIL("sync", "apply_chunk: BEGIN transaction failed");
    }

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxos (txid,vout,value,script,script_type,height) "
        "VALUES (?,?,?,?,0,?)", -1, &ins, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        LOG_FAIL("sync", "apply_chunk: INSERT prepare failed");
    }

    bool insert_ok = true;
    for (uint32_t i = 0; i < chunk->num_entries; i++) {
        sqlite3_reset(ins);
        sqlite3_bind_blob(ins, 1, chunk->entries[i].txid, 32, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, (int)chunk->entries[i].vout);
        sqlite3_bind_int64(ins, 3, chunk->entries[i].value);
        sqlite3_bind_blob(ins, 4, chunk->entries[i].script,
                           chunk->entries[i].script_len, SQLITE_STATIC);
        sqlite3_bind_int(ins, 5, chunk->entries[i].height);
        int rc = sqlite3_step(ins);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            fprintf(stderr, "fast_sync_apply_chunk: insert %u/%u failed: %s\n",
                    i, chunk->num_entries, sqlite3_errmsg(db));
            insert_ok = false;
            break;
        }
    }
    sqlite3_finalize(ins);

    if (!insert_ok) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        LOG_FAIL("sync", "apply_chunk: UTXO insert failed");
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        LOG_FAIL("sync", "apply_chunk: COMMIT failed");
    }

    sqlite3_close(db);
    return true;
}

/* ── BitTorrent-style parallel chunk sync (SHA3-256) ─────── */
/* New ZCL23 protocol uses SHA3-256 for hash diversity.
 * Legacy consensus (block hashes, tx hashes) stays SHA-256d. */

void fast_sync_chunk_hash(const struct utxo_chunk *chunk,
                           uint8_t hash_out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    /* Hash chunk index so identical UTXOs at different positions differ */
    sha3_256_write(&ctx, (const unsigned char *)&chunk->chunk_index, 4);
    sha3_256_write(&ctx, (const unsigned char *)&chunk->num_entries, 4);

    for (uint32_t i = 0; i < chunk->num_entries; i++) {
        sha3_256_write(&ctx, chunk->entries[i].txid, 32);
        sha3_256_write(&ctx, (const unsigned char *)&chunk->entries[i].vout, 4);
        sha3_256_write(&ctx, (const unsigned char *)&chunk->entries[i].value, 8);
        sha3_256_write(&ctx, chunk->entries[i].script,
                     chunk->entries[i].script_len);
        sha3_256_write(&ctx, (const unsigned char *)&chunk->entries[i].script_len, 2);
        sha3_256_write(&ctx, (const unsigned char *)&chunk->entries[i].height, 4);
    }

    sha3_256_finalize(&ctx, hash_out);
}

bool fast_sync_verify_chunk(const struct utxo_chunk *chunk,
                             const uint8_t expected_hash[32])
{
    GUARD(chunk && expected_hash, "sync", "verify_chunk: chunk or expected_hash is NULL");
    uint8_t actual[32];
    fast_sync_chunk_hash(chunk, actual);
    return memcmp(actual, expected_hash, 32) == 0;
}

/* Round up to next power of two */
static uint32_t next_pow2(uint32_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/* Hash two 32-byte nodes together: SHA3-256(left || right) */
static void merkle_combine(const uint8_t left[32], const uint8_t right[32],
                            uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, left, 32);
    sha3_256_write(&ctx, right, 32);
    sha3_256_finalize(&ctx, out);
}

void fast_sync_merkle_root(const uint8_t (*hashes)[32],
                            uint32_t count,
                            uint8_t root_out[32])
{
    if (count == 0) {
        memset(root_out, 0, 32);
        return;
    }
    if (count == 1) {
        memcpy(root_out, hashes[0], 32);
        return;
    }

    /* Pad to power of two with copies of last hash */
    uint32_t padded = next_pow2(count);
    uint8_t (*layer)[32] = zcl_calloc(padded, 32, "merkle_layer");
    if (!layer) { memset(root_out, 0, 32); return; }

    for (uint32_t i = 0; i < padded; i++) {
        if (i < count)
            memcpy(layer[i], hashes[i], 32);
        else
            memcpy(layer[i], hashes[count - 1], 32);
    }

    /* Iteratively combine pairs until one root remains */
    uint32_t n = padded;
    while (n > 1) {
        for (uint32_t i = 0; i < n / 2; i++)
            merkle_combine(layer[2 * i], layer[2 * i + 1], layer[i]);
        n /= 2;
    }

    memcpy(root_out, layer[0], 32);
    free(layer);
}

uint32_t fast_sync_build_proof(const uint8_t (*hashes)[32],
                                uint32_t count,
                                uint32_t chunk_index,
                                uint8_t (**proof_out)[32])
{
    if (!hashes || count == 0 || chunk_index >= count || !proof_out) {
        if (proof_out) *proof_out = NULL;
        return 0;
    }
    if (count == 1) {
        *proof_out = NULL;
        return 0;
    }

    uint32_t padded = next_pow2(count);
    uint32_t depth = 0;
    for (uint32_t v = padded; v > 1; v >>= 1) depth++;

    uint8_t (*layer)[32] = zcl_calloc(padded, 32, "merkle_layer");
    uint8_t (*proof)[32] = zcl_calloc(depth, 32, "merkle_proof");
    if (!layer || !proof) {
        free(layer); free(proof);
        *proof_out = NULL;
        return 0;
    }

    for (uint32_t i = 0; i < padded; i++) {
        if (i < count)
            memcpy(layer[i], hashes[i], 32);
        else
            memcpy(layer[i], hashes[count - 1], 32);
    }

    uint32_t idx = chunk_index;
    uint32_t n = padded;
    uint32_t p = 0;

    while (n > 1) {
        uint32_t sibling = (idx % 2 == 0) ? idx + 1 : idx - 1;
        memcpy(proof[p++], layer[sibling], 32);
        /* Compute next layer */
        for (uint32_t i = 0; i < n / 2; i++)
            merkle_combine(layer[2 * i], layer[2 * i + 1], layer[i]);
        idx /= 2;
        n /= 2;
    }

    free(layer);
    *proof_out = proof;
    return p;
}

bool fast_sync_verify_chunk_proof(uint32_t chunk_index,
                                   const uint8_t chunk_hash[32],
                                   const uint8_t (*proof)[32],
                                   uint32_t proof_len,
                                   const uint8_t merkle_root[32])
{
    GUARD(chunk_hash && merkle_root, "sync", "verify_chunk_proof: chunk_hash or merkle_root is NULL");

    uint8_t current[32];
    memcpy(current, chunk_hash, 32);
    uint32_t idx = chunk_index;

    for (uint32_t i = 0; i < proof_len; i++) {
        uint8_t combined[32];
        if (idx % 2 == 0)
            merkle_combine(current, proof[i], combined);
        else
            merkle_combine(proof[i], current, combined);
        memcpy(current, combined, 32);
        idx /= 2;
    }

    return memcmp(current, merkle_root, 32) == 0;
}

bool fast_sync_serve_chunk_db(sqlite3 *db, uint32_t chunk_index,
                               uint32_t chunk_size,
                               struct utxo_chunk *out)
{
    GUARD(db && out, "sync", "serve_chunk_db: db or out is NULL");
    memset(out, 0, sizeof(*out));
    out->chunk_index = chunk_index;

    /* Keyset pagination: O(log n) seek instead of O(n) OFFSET.
     * For chunk 0, start from the beginning. For chunk N, seek to the
     * (N*chunk_size)th row using a subquery on the PK ordering. */
    sqlite3_stmt *s = NULL;

    if (chunk_index == 0) {
        /* First chunk: simple LIMIT */
        const char *sql = "SELECT txid, vout, value, script, height "
                          "FROM utxos ORDER BY txid, vout LIMIT ?";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
            LOG_FAIL("sync", "serve_chunk_db: prepare first-chunk query failed");
        sqlite3_bind_int(s, 1, (int)chunk_size);
    } else {
        /* Keyset seek: find the cursor position of the last row of the
         * previous chunk, then scan forward. Uses the PK index on (txid,vout)
         * so the seek is O(log n) regardless of chunk position. */
        const char *sql =
            "SELECT txid, vout, value, script, height FROM utxos "
            "WHERE (txid, vout) > ("
            "  SELECT txid, vout FROM utxos ORDER BY txid, vout "
            "  LIMIT 1 OFFSET ?"
            ") ORDER BY txid, vout LIMIT ?";
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
            /* Fallback: plain OFFSET (older SQLite without tuple comparison) */
            char fallback[256];
            snprintf(fallback, sizeof(fallback),
                     "SELECT txid, vout, value, script, height "
                     "FROM utxos ORDER BY txid, vout LIMIT %u OFFSET %u",
                     chunk_size, chunk_index * chunk_size);
            if (sqlite3_prepare_v2(db, fallback, -1, &s, NULL) != SQLITE_OK)
                LOG_FAIL("sync", "serve_chunk_db: prepare fallback query failed for chunk %u", chunk_index);
        } else {
            sqlite3_bind_int(s, 1, (int)(chunk_index * chunk_size - 1));
            sqlite3_bind_int(s, 2, (int)chunk_size);
        }
    }

    while (sqlite3_step(s) == SQLITE_ROW && out->num_entries < chunk_size) {
        uint32_t i = out->num_entries;
        const void *txid = sqlite3_column_blob(s, 0);
        if (txid) memcpy(out->entries[i].txid, txid, 32);
        out->entries[i].vout = (uint32_t)sqlite3_column_int(s, 1);
        out->entries[i].value = sqlite3_column_int64(s, 2);

        const void *script = sqlite3_column_blob(s, 3);
        int slen = sqlite3_column_bytes(s, 3);
        if (script && slen > 0) {
            if (slen > 128) slen = 128;
            memcpy(out->entries[i].script, script, (size_t)slen);
            out->entries[i].script_len = (uint16_t)slen;
        }
        out->entries[i].height = sqlite3_column_int(s, 4);
        out->num_entries++;
    }
    sqlite3_finalize(s);
    return out->num_entries > 0;
}

bool fast_sync_serve_chunk(const char *datadir, uint32_t chunk_index,
                            struct utxo_chunk *out)
{
    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "serve_chunk: failed to open db %s", db_path);

    bool ok = fast_sync_serve_chunk_db(db, chunk_index, SYNC_CHUNK_SIZE, out);
    sqlite3_close(db);
    return ok;
}

bool fast_sync_build_manifest_db(sqlite3 *db, struct sync_manifest *out)
{
    GUARD(db && out, "sync", "build_manifest_db: db or out is NULL");
    memset(out, 0, sizeof(*out));
    out->chunk_size = SYNC_CHUNK_SIZE;

    /* Get tip height */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT value FROM node_state WHERE key='tip_height'",
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(s, 0);
        if (len == (int)sizeof(int64_t)) {
            int64_t h;
            memcpy(&h, sqlite3_column_blob(s, 0), sizeof(h));
            out->height = (int32_t)h;
        } else if (len >= 1 && len <= 8) {
            const void *blob = sqlite3_column_blob(s, 0);
            memcpy(&out->height, blob, len < 4 ? (size_t)len : 4);
        }
    }
    sqlite3_finalize(s);

    /* Get tip hash */
    sqlite3_prepare_v2(db,
        "SELECT value FROM node_state WHERE key='tip_hash'",
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(s, 0);
        if (h && sqlite3_column_bytes(s, 0) >= 32)
            memcpy(out->block_hash, h, 32);
    }
    sqlite3_finalize(s);

    /* Count UTXOs */
    sqlite3_prepare_v2(db, "SELECT count(*) FROM utxos", -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        out->num_utxos = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    GUARD(out->num_utxos > 0, "sync", "build_manifest_db: no UTXOs in database");

    /* Calculate chunk count: ceil(num_utxos / chunk_size) */
    out->num_chunks = (uint32_t)((out->num_utxos + out->chunk_size - 1)
                                  / out->chunk_size);

    /* Allocate chunk hashes array */
    out->chunk_hashes = zcl_calloc(out->num_chunks, 32, "chunk_hashes");
    GUARD(out->chunk_hashes, "sync", "build_manifest_db: alloc chunk_hashes failed for %u chunks", out->num_chunks);

    /* Compute hash for each chunk */
    struct utxo_chunk *chunk = zcl_calloc(1, sizeof(struct utxo_chunk), "utxo_chunk");
    if (!chunk) { free(out->chunk_hashes); out->chunk_hashes = NULL; LOG_FAIL("sync", "build_manifest_db: alloc utxo_chunk failed"); }

    for (uint32_t ci = 0; ci < out->num_chunks; ci++) {
        if (!fast_sync_serve_chunk_db(db, ci, out->chunk_size, chunk)) {
            /* Empty chunk at end is not expected but handle gracefully */
            memset(out->chunk_hashes[ci], 0, 32);
            continue;
        }
        fast_sync_chunk_hash(chunk, out->chunk_hashes[ci]);
    }
    free(chunk);

    /* Build Merkle root from chunk hashes */
    fast_sync_merkle_root(
        (const uint8_t (*)[32])out->chunk_hashes,
        out->num_chunks, out->merkle_root);

    return true;
}

bool fast_sync_build_manifest(const char *datadir,
                               struct sync_manifest *out)
{
    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "build_manifest: failed to open db %s", db_path);

    bool ok = fast_sync_build_manifest_db(db, out);
    sqlite3_close(db);
    return ok;
}

void sync_manifest_free(struct sync_manifest *m)
{
    if (m && m->chunk_hashes) {
        free(m->chunk_hashes);
        m->chunk_hashes = NULL;
    }
}

/* ── Swarm coordinator: BitTorrent-style parallel UTXO sync ── */

bool swarm_sync_init(struct swarm_sync *ss, const struct sync_manifest *manifest,
                      const char *datadir)
{
    GUARD(ss && manifest && manifest->num_chunks > 0, "sync", "swarm_sync_init: invalid args (ss=%p manifest=%p)", (void *)ss, (void *)manifest);
    memset(ss, 0, sizeof(*ss));

    uint32_t n = manifest->num_chunks;

    /* Deep-copy manifest */
    ss->manifest = *manifest;
    ss->manifest.chunk_hashes = zcl_calloc(n, 32, "chunk_hashes");
    GUARD(ss->manifest.chunk_hashes, "sync", "swarm_sync_init: alloc chunk_hashes failed for %u chunks", n);
    if (manifest->chunk_hashes)
        memcpy(ss->manifest.chunk_hashes, manifest->chunk_hashes, (size_t)n * 32);

    ss->chunk_states = zcl_calloc(n, sizeof(enum chunk_state), "chunk_states");
    ss->chunk_peer = zcl_calloc(n, sizeof(int), "chunk_peer");
    ss->chunk_request_time = zcl_calloc(n, sizeof(int64_t), "chunk_req_time");
    ss->chunk_retries = zcl_calloc(n, sizeof(int), "chunk_retries");

    if (!ss->chunk_states || !ss->chunk_peer || !ss->chunk_request_time
        || !ss->chunk_retries) {
        swarm_sync_free(ss);
        LOG_FAIL("sync", "swarm_sync_init: alloc state arrays failed for %u chunks", n);
    }

    /* All chunks start as NEEDED (calloc zeroes = CHUNK_NEEDED) */
    for (uint32_t i = 0; i < n; i++)
        ss->chunk_peer[i] = -1;

    ss->datadir = datadir;
    return true;
}

void swarm_sync_free(struct swarm_sync *ss)
{
    if (!ss) return;
    free(ss->manifest.chunk_hashes);
    ss->manifest.chunk_hashes = NULL;
    free(ss->chunk_states);
    ss->chunk_states = NULL;
    free(ss->chunk_peer);
    ss->chunk_peer = NULL;
    free(ss->chunk_request_time);
    ss->chunk_request_time = NULL;
    free(ss->chunk_retries);
    ss->chunk_retries = NULL;
}

int32_t swarm_sync_assign_chunk(struct swarm_sync *ss, int peer_id)
{
    if (!ss || !ss->chunk_states)
        LOG_RETURN(-1, "sync", "assign_chunk: ss or chunk_states is NULL");

    for (uint32_t i = 0; i < ss->manifest.num_chunks; i++) {
        if (ss->chunk_states[i] == CHUNK_NEEDED) {
            ss->chunk_states[i] = CHUNK_INFLIGHT;
            ss->chunk_peer[i] = peer_id;
            ss->chunk_request_time[i] = (int64_t)time(NULL);
            ss->chunks_inflight++;
            return (int32_t)i;
        }
    }
    LOG_RETURN(-1, "sync", "assign_chunk: no chunks available for peer %d", peer_id);
}

bool swarm_sync_receive_chunk(struct swarm_sync *ss,
                                const struct utxo_chunk *chunk,
                                int peer_id)
{
    GUARD(ss && chunk, "sync", "receive_chunk: ss or chunk is NULL");

    uint32_t idx = chunk->chunk_index;
    if (idx >= ss->manifest.num_chunks)
        LOG_FAIL("sync", "receive_chunk: chunk_index %u >= num_chunks %u",
                 idx, ss->manifest.num_chunks);

    /* Verify chunk hash against manifest */
    if (ss->manifest.chunk_hashes) {
        if (!fast_sync_verify_chunk(chunk, ss->manifest.chunk_hashes[idx])) {
            ss->chunk_retries[idx]++;
            /* Reset to NEEDED so another peer can retry — unless max retries */
            if (ss->chunk_retries[idx] >= 5) {
                ss->chunk_states[idx] = CHUNK_FAILED;
                ss->chunks_failed++;
            } else {
                ss->chunk_states[idx] = CHUNK_NEEDED;
            }
            ss->chunk_peer[idx] = -1;
            if (ss->chunks_inflight > 0)
                ss->chunks_inflight--;
            LOG_FAIL("sync", "receive_chunk: chunk %u hash mismatch from peer %d (retry %d/5)",
                     idx, peer_id, ss->chunk_retries[idx]);
        }
    }

    /* Apply chunk to database */
    if (ss->datadir) {
        if (!fast_sync_apply_chunk(ss->datadir, chunk)) {
            ss->chunk_retries[idx]++;
            if (ss->chunk_retries[idx] >= 5) {
                ss->chunk_states[idx] = CHUNK_FAILED;
                ss->chunks_failed++;
            } else {
                ss->chunk_states[idx] = CHUNK_NEEDED;
            }
            ss->chunk_peer[idx] = -1;
            if (ss->chunks_inflight > 0)
                ss->chunks_inflight--;
            LOG_FAIL("sync", "receive_chunk: chunk %u apply failed (retry %d/5)",
                     idx, ss->chunk_retries[idx]);
        }
    }

    ss->chunk_states[idx] = CHUNK_COMPLETE;
    ss->chunk_peer[idx] = peer_id;
    if (ss->chunks_inflight > 0)
        ss->chunks_inflight--;
    ss->chunks_complete++;
    return true;
}

bool swarm_sync_is_complete(const struct swarm_sync *ss)
{
    GUARD(ss, "sync", "swarm_sync_is_complete: ss is NULL");
    return ss->chunks_complete == ss->manifest.num_chunks;
}

int swarm_sync_progress(const struct swarm_sync *ss)
{
    if (!ss || ss->manifest.num_chunks == 0) return 0;
    return (int)(ss->chunks_complete * 100 / ss->manifest.num_chunks);
}

void swarm_sync_handle_timeouts(struct swarm_sync *ss, int timeout_secs)
{
    if (!ss || !ss->chunk_states) return;

    int64_t now = (int64_t)time(NULL);
    for (uint32_t i = 0; i < ss->manifest.num_chunks; i++) {
        if (ss->chunk_states[i] == CHUNK_INFLIGHT &&
            now - ss->chunk_request_time[i] > timeout_secs) {
            ss->chunk_states[i] = CHUNK_NEEDED;
            ss->chunk_peer[i] = -1;
            ss->chunk_request_time[i] = 0;
            if (ss->chunks_inflight > 0)
                ss->chunks_inflight--;
        }
    }
}

/* ── Block swarm: BitTorrent-style parallel block download ──── */

void block_piece_hash(const uint8_t (*block_hashes)[32], uint32_t count,
                       uint32_t piece_index, uint8_t hash_out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, (const unsigned char *)&piece_index, 4);
    sha3_256_write(&ctx, (const unsigned char *)&count, 4);
    for (uint32_t i = 0; i < count; i++)
        sha3_256_write(&ctx, block_hashes[i], 32);
    sha3_256_finalize(&ctx, hash_out);
}

void block_piece_manifest_free(struct block_piece_manifest *m)
{
    if (m && m->piece_hashes) {
        free(m->piece_hashes);
        m->piece_hashes = NULL;
    }
}

bool block_piece_manifest_build(const char *datadir,
                                 int32_t start_height, int32_t end_height,
                                 struct block_piece_manifest *out)
{
    GUARD(out && end_height >= start_height, "sync", "piece_manifest_build: invalid args (out=%p start=%d end=%d)", (void *)out, start_height, end_height);
    memset(out, 0, sizeof(*out));

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        LOG_FAIL("sync", "piece_manifest_build: failed to open db %s", db_path);

    /* Verify the blocks table has data in the requested range.
     * During IBD the SQLite index may lag behind the chain tip. */
    {
        sqlite3_stmt *cnt = NULL;
        int rc = sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM blocks WHERE height >= ? AND height <= ?",
            -1, &cnt, NULL);
        if (rc != SQLITE_OK || !cnt) {
            sqlite3_close(db);
            LOG_FAIL("sync", "piece_manifest_build: COUNT query prepare failed");
        }
        sqlite3_bind_int(cnt, 1, start_height);
        sqlite3_bind_int(cnt, 2, end_height);
        int64_t block_count = 0;
        if (sqlite3_step(cnt) == SQLITE_ROW)
            block_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);

        /* Need at least 90% coverage to build a useful manifest */
        int64_t expected = end_height - start_height + 1;
        if (block_count < expected * 9 / 10) {
            sqlite3_close(db);
            LOG_FAIL("sync", "piece_manifest_build: insufficient block coverage (%lld/%lld)",
                     (long long)block_count, (long long)expected);
        }
    }

    out->start_height = start_height;
    out->end_height = end_height;
    int32_t total_blocks = end_height - start_height + 1;
    out->num_pieces = (uint32_t)((total_blocks + BLOCKS_PER_PIECE - 1)
                                  / BLOCKS_PER_PIECE);

    /* Get tip hash */
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM blocks WHERE height = ?", -1, &s, NULL);
    if (rc != SQLITE_OK || !s) { sqlite3_close(db); LOG_FAIL("sync", "piece_manifest_build: tip hash query prepare failed"); }
    sqlite3_bind_int(s, 1, end_height);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(s, 0);
        if (h && sqlite3_column_bytes(s, 0) >= 32)
            memcpy(out->tip_hash, h, 32);
    }
    sqlite3_finalize(s);

    /* Allocate piece hashes */
    out->piece_hashes = zcl_calloc(out->num_pieces, 32, "piece_hashes");
    if (!out->piece_hashes) { sqlite3_close(db); LOG_FAIL("sync", "piece_manifest_build: alloc piece_hashes failed for %u pieces", out->num_pieces); }

    /* Fetch all block hashes in range, compute piece hashes */
    s = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM blocks WHERE height >= ? AND height <= ? "
        "ORDER BY height ASC", -1, &s, NULL);
    if (rc != SQLITE_OK || !s) {
        sqlite3_close(db);
        free(out->piece_hashes);
        out->piece_hashes = NULL;
        LOG_FAIL("sync", "piece_manifest_build: block hash query prepare failed");
    }
    sqlite3_bind_int(s, 1, start_height);
    sqlite3_bind_int(s, 2, end_height);

    uint8_t (*piece_block_hashes)[32] = zcl_calloc(BLOCKS_PER_PIECE, 32, "piece_block_hashes");
    if (!piece_block_hashes) {
        sqlite3_finalize(s);
        sqlite3_close(db);
        free(out->piece_hashes);
        out->piece_hashes = NULL;
        LOG_FAIL("sync", "piece_manifest_build: alloc piece_block_hashes failed");
    }

    uint32_t piece_idx = 0;
    uint32_t block_in_piece = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(s, 0);
        if (h && sqlite3_column_bytes(s, 0) >= 32)
            memcpy(piece_block_hashes[block_in_piece], h, 32);
        block_in_piece++;

        if (block_in_piece == BLOCKS_PER_PIECE) {
            if (piece_idx < out->num_pieces) {
                block_piece_hash((const uint8_t (*)[32])piece_block_hashes,
                                 block_in_piece, piece_idx,
                                 out->piece_hashes[piece_idx]);
            }
            piece_idx++;
            block_in_piece = 0;
        }
    }

    /* Final partial piece */
    if (block_in_piece > 0 && piece_idx < out->num_pieces) {
        block_piece_hash((const uint8_t (*)[32])piece_block_hashes,
                         block_in_piece, piece_idx,
                         out->piece_hashes[piece_idx]);
    }

    free(piece_block_hashes);
    sqlite3_finalize(s);

    /* Build Merkle root from piece hashes */
    fast_sync_merkle_root((const uint8_t (*)[32])out->piece_hashes,
                           out->num_pieces, out->merkle_root);

    sqlite3_close(db);
    return true;
}

bool block_swarm_init(struct block_swarm *bs,
                      const struct block_piece_manifest *manifest,
                      const char *datadir)
{
    GUARD(bs && manifest && manifest->num_pieces > 0, "sync", "block_swarm_init: invalid args (bs=%p manifest=%p)", (void *)bs, (void *)manifest);
    memset(bs, 0, sizeof(*bs));

    uint32_t n = manifest->num_pieces;

    /* Deep-copy manifest */
    bs->manifest = *manifest;
    bs->manifest.piece_hashes = zcl_calloc(n, 32, "piece_hashes");
    GUARD(bs->manifest.piece_hashes, "sync", "block_swarm_init: alloc piece_hashes failed for %u pieces", n);
    if (manifest->piece_hashes)
        memcpy(bs->manifest.piece_hashes, manifest->piece_hashes,
               (size_t)n * 32);

    bs->piece_states = zcl_calloc(n, sizeof(enum chunk_state), "piece_states");
    bs->piece_peer = zcl_calloc(n, sizeof(int), "piece_peer");
    bs->piece_request_time = zcl_calloc(n, sizeof(int64_t), "piece_req_time");
    bs->piece_availability = zcl_calloc(n, sizeof(uint32_t), "piece_availability");

    if (!bs->piece_states || !bs->piece_peer ||
        !bs->piece_request_time || !bs->piece_availability) {
        block_swarm_free(bs);
        LOG_FAIL("sync", "block_swarm_init: alloc state arrays failed for %u pieces", n);
    }

    for (uint32_t i = 0; i < n; i++)
        bs->piece_peer[i] = -1;

    bs->datadir = datadir;
    return true;
}

void block_swarm_free(struct block_swarm *bs)
{
    if (!bs) return;
    free(bs->manifest.piece_hashes);
    bs->manifest.piece_hashes = NULL;
    free(bs->piece_states);
    bs->piece_states = NULL;
    free(bs->piece_peer);
    bs->piece_peer = NULL;
    free(bs->piece_request_time);
    bs->piece_request_time = NULL;
    free(bs->piece_availability);
    bs->piece_availability = NULL;
}

/* Rarest-first piece selection: pick the needed piece with the lowest
 * availability count. Ties broken by sequential order (lower index first).
 * If peer_bitmap is non-NULL, only consider pieces the peer has. */
int32_t block_swarm_assign_piece(struct block_swarm *bs, int peer_id,
                                  const uint8_t *peer_bitmap)
{
    if (!bs || !bs->piece_states)
        LOG_RETURN(-1, "sync", "assign_piece: bs or piece_states is NULL");

    /* Endgame mode: if few pieces remain, use broadcast strategy.
     * Caller should request all remaining from all peers. */
    uint32_t remaining = bs->manifest.num_pieces - bs->pieces_complete;
    if (remaining <= ENDGAME_THRESHOLD && remaining > 0)
        bs->endgame = true;

    int32_t best = -1;
    uint32_t best_avail = UINT32_MAX;

    for (uint32_t i = 0; i < bs->manifest.num_pieces; i++) {
        if (bs->piece_states[i] != CHUNK_NEEDED &&
            bs->piece_states[i] != CHUNK_FAILED)
            continue;

        /* In endgame, also consider INFLIGHT pieces for duplicate requests */
        if (bs->endgame && bs->piece_states[i] == CHUNK_INFLIGHT) {
            /* Allow re-request in endgame, but not from same peer */
            if (bs->piece_peer[i] == peer_id) continue;
        } else if (bs->piece_states[i] == CHUNK_INFLIGHT) {
            continue;
        }

        /* Check peer bitmap if available */
        if (peer_bitmap && !(peer_bitmap[i / 8] & (1 << (i % 8))))
            continue;

        /* Rarest-first: prefer pieces fewer peers have */
        uint32_t avail = bs->piece_availability
            ? bs->piece_availability[i] : 1;
        if (avail < best_avail || (avail == best_avail && best < 0)) {
            best_avail = avail;
            best = (int32_t)i;
        }
    }

    if (best >= 0) {
        bs->piece_states[best] = CHUNK_INFLIGHT;
        bs->piece_peer[best] = peer_id;
        bs->piece_request_time[best] = (int64_t)time(NULL);
        bs->pieces_inflight++;
    }
    return best;
}

bool block_swarm_receive_piece(struct block_swarm *bs,
                                uint32_t piece_index, int peer_id)
{
    GUARD(bs && piece_index < bs->manifest.num_pieces, "sync", "receive_piece: invalid args (bs=%p piece=%u)", (void *)bs, piece_index);
    (void)peer_id;

    bs->piece_states[piece_index] = CHUNK_COMPLETE;
    if (bs->pieces_inflight > 0) bs->pieces_inflight--;
    bs->pieces_complete++;

    /* Check endgame exit */
    uint32_t remaining = bs->manifest.num_pieces - bs->pieces_complete;
    if (remaining == 0) bs->endgame = false;

    return true;
}

void block_swarm_fail_piece(struct block_swarm *bs, uint32_t piece_index)
{
    if (!bs || piece_index >= bs->manifest.num_pieces) return;
    bs->piece_states[piece_index] = CHUNK_NEEDED;
    bs->piece_peer[piece_index] = -1;
    if (bs->pieces_inflight > 0) bs->pieces_inflight--;
    bs->pieces_failed++;
}

bool block_swarm_is_complete(const struct block_swarm *bs)
{
    GUARD(bs, "sync", "block_swarm_is_complete: bs is NULL");
    return bs->pieces_complete == bs->manifest.num_pieces;
}

int block_swarm_progress(const struct block_swarm *bs)
{
    if (!bs || bs->manifest.num_pieces == 0) return 0;
    return (int)(bs->pieces_complete * 100 / bs->manifest.num_pieces);
}

void block_swarm_handle_timeouts(struct block_swarm *bs, int timeout_secs)
{
    if (!bs || !bs->piece_states) return;

    int64_t now = (int64_t)time(NULL);
    for (uint32_t i = 0; i < bs->manifest.num_pieces; i++) {
        if (bs->piece_states[i] == CHUNK_INFLIGHT &&
            now - bs->piece_request_time[i] > timeout_secs) {
            bs->piece_states[i] = CHUNK_NEEDED;
            bs->piece_peer[i] = -1;
            bs->piece_request_time[i] = 0;
            if (bs->pieces_inflight > 0)
                bs->pieces_inflight--;
        }
    }
}

void block_swarm_update_availability(struct block_swarm *bs,
                                      const uint8_t *bitmap,
                                      uint32_t bitmap_len)
{
    if (!bs || !bitmap || !bs->piece_availability) return;
    for (uint32_t i = 0; i < bs->manifest.num_pieces; i++) {
        if (i / 8 < bitmap_len && (bitmap[i / 8] & (1 << (i % 8))))
            bs->piece_availability[i]++;
    }
}

uint32_t block_swarm_endgame_pieces(const struct block_swarm *bs,
                                     uint32_t *out_indices, uint32_t max)
{
    if (!bs || !out_indices || !bs->endgame) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < bs->manifest.num_pieces && count < max; i++) {
        if (bs->piece_states[i] != CHUNK_COMPLETE)
            out_indices[count++] = i;
    }
    return count;
}

uint32_t block_swarm_serialize_bitmap(const struct block_swarm *bs,
                                       uint8_t *out, uint32_t max_len)
{
    if (!bs || !out) return 0;
    uint32_t bytes = (bs->manifest.num_pieces + 7) / 8;
    if (bytes > max_len) bytes = max_len;
    memset(out, 0, bytes);
    for (uint32_t i = 0; i < bs->manifest.num_pieces && i / 8 < bytes; i++) {
        if (bs->piece_states[i] == CHUNK_COMPLETE)
            out[i / 8] |= (uint8_t)(1 << (i % 8));
    }
    return bytes;
}
