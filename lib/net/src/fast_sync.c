/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast P2P sync: UTXO snapshot transfer between zclassic23 nodes. */

#include "net/fast_sync.h"
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

bool fast_sync_build_offer(const char *datadir,
                            struct snapshot_offer *offer)
{
    if (!offer) return false;
    memset(offer, 0, sizeof(*offer));

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return false;

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

    /* Compute UTXO root */
    fast_sync_compute_utxo_root_db(db, offer->utxo_root);

    sqlite3_close(db);
    return offer->height > 0;
}

/* Internal: compute root from open db handle */
void fast_sync_compute_utxo_root_db(sqlite3 *db, uint8_t root_out[32])
{
    /* Rolling SHA-256 hash of all UTXOs ordered by (txid, vout).
     * This is a simple commitment — not a full Merkle tree yet,
     * but sufficient for snapshot verification. */
    struct sha256_ctx ctx;
    sha256_init(&ctx);

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT txid, vout, value, height FROM utxos ORDER BY txid, vout",
        -1, &s, NULL);

    uint64_t count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const void *txid = sqlite3_column_blob(s, 0);
        int32_t vout = sqlite3_column_int(s, 1);
        int64_t value = sqlite3_column_int64(s, 2);
        int32_t height = sqlite3_column_int(s, 3);

        if (txid) sha256_write(&ctx, txid, 32);
        sha256_write(&ctx, (const unsigned char *)&vout, 4);
        sha256_write(&ctx, (const unsigned char *)&value, 8);
        sha256_write(&ctx, (const unsigned char *)&height, 4);
        count++;
    }
    sqlite3_finalize(s);

    /* Finalize: double-SHA256 */
    unsigned char h1[32];
    sha256_finalize(&ctx, h1);
    sha256_init(&ctx);
    sha256_write(&ctx, h1, 32);
    sha256_finalize(&ctx, root_out);
}

bool fast_sync_compute_utxo_root(const char *datadir,
                                  uint8_t root_out[32])
{
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return false;

    fast_sync_compute_utxo_root_db(db, root_out);
    sqlite3_close(db);
    return true;
}

bool fast_sync_serve_snapshot(const char *datadir,
                               int from_height,
                               chunk_callback cb, void *ctx)
{
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT txid, vout, value, script, height "
        "FROM utxos ORDER BY txid, vout",
        -1, &s, NULL);

    struct utxo_chunk *chunk = calloc(1, sizeof(struct utxo_chunk));
    if (!chunk) { sqlite3_finalize(s); sqlite3_close(db); return false; }

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
    if (!pow) return false;

    /* Timestamp must be within 5 minutes */
    int64_t now = (int64_t)time(NULL);
    if (pow->timestamp < now - 300 || pow->timestamp > now + 60)
        return false;

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
        if (hash[i] != 0) return false;
    if (bits % 8 > 0) {
        uint8_t mask = (uint8_t)(0xFF << (8 - bits % 8));
        if (hash[bits / 8] & mask) return false;
    }
    return true;
}

bool fast_sync_solve_pow(const uint8_t peer_id[32], struct fast_sync_pow *pow)
{
    if (!pow) return false;
    memcpy(pow->peer_id, peer_id, 32);
    pow->timestamp = (int64_t)time(NULL);
    pow->nonce = 0;

    while (pow->nonce < UINT64_MAX) {
        if (fast_sync_verify_pow(pow))
            return true;
        pow->nonce++;
    }
    return false;
}

/* ── Rate limiting ───────────────────────────────────────── */

bool fast_sync_rate_check(struct fast_sync_rate_limiter *rl,
                           const uint8_t ip[16])
{
    int64_t now = (int64_t)time(NULL);

    /* Find or create entry for this IP */
    for (size_t i = 0; i < rl->num_entries; i++) {
        if (memcmp(rl->entries[i].ip, ip, 16) == 0) {
            /* Reset window if >1 hour old */
            if (now - rl->entries[i].window_start > 3600) {
                rl->entries[i].window_start = now;
                rl->entries[i].chunks_sent = 0;
            }
            if (rl->entries[i].chunks_sent >= FAST_SYNC_MAX_CHUNKS_PER_HOUR)
                return false;
            rl->entries[i].chunks_sent++;
            return true;
        }
    }

    /* New IP */
    if (rl->num_entries < 256) {
        size_t idx = rl->num_entries++;
        memcpy(rl->entries[idx].ip, ip, 16);
        rl->entries[idx].window_start = now;
        rl->entries[idx].chunks_sent = 1;
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
    return true;
}

bool fast_sync_apply_chunk(const char *datadir,
                            const struct utxo_chunk *chunk)
{
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        return false;

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxos (txid,vout,value,script,script_type,height) "
        "VALUES (?,?,?,?,0,?)", -1, &ins, NULL);

    for (uint32_t i = 0; i < chunk->num_entries; i++) {
        sqlite3_reset(ins);
        sqlite3_bind_blob(ins, 1, chunk->entries[i].txid, 32, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, (int)chunk->entries[i].vout);
        sqlite3_bind_int64(ins, 3, chunk->entries[i].value);
        sqlite3_bind_blob(ins, 4, chunk->entries[i].script,
                           chunk->entries[i].script_len, SQLITE_STATIC);
        sqlite3_bind_int(ins, 5, chunk->entries[i].height);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
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
    if (!chunk || !expected_hash) return false;
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
    uint8_t (*layer)[32] = calloc(padded, 32);
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

    uint8_t (*layer)[32] = calloc(padded, 32);
    uint8_t (*proof)[32] = calloc(depth, 32);
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
    if (!chunk_hash || !merkle_root) return false;

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
    if (!db || !out) return false;
    memset(out, 0, sizeof(*out));
    out->chunk_index = chunk_index;

    sqlite3_stmt *s = NULL;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT txid, vout, value, script, height "
             "FROM utxos ORDER BY txid, vout "
             "LIMIT %u OFFSET %u",
             chunk_size, chunk_index * chunk_size);

    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return false;

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
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return false;

    bool ok = fast_sync_serve_chunk_db(db, chunk_index, SYNC_CHUNK_SIZE, out);
    sqlite3_close(db);
    return ok;
}

bool fast_sync_build_manifest_db(sqlite3 *db, struct sync_manifest *out)
{
    if (!db || !out) return false;
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

    if (out->num_utxos == 0) return false;

    /* Calculate chunk count: ceil(num_utxos / chunk_size) */
    out->num_chunks = (uint32_t)((out->num_utxos + out->chunk_size - 1)
                                  / out->chunk_size);

    /* Allocate chunk hashes array */
    out->chunk_hashes = calloc(out->num_chunks, 32);
    if (!out->chunk_hashes) return false;

    /* Compute hash for each chunk */
    struct utxo_chunk *chunk = calloc(1, sizeof(struct utxo_chunk));
    if (!chunk) { free(out->chunk_hashes); out->chunk_hashes = NULL; return false; }

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
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return false;

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
    if (!ss || !manifest || manifest->num_chunks == 0) return false;
    memset(ss, 0, sizeof(*ss));

    uint32_t n = manifest->num_chunks;

    /* Deep-copy manifest */
    ss->manifest = *manifest;
    ss->manifest.chunk_hashes = calloc(n, 32);
    if (!ss->manifest.chunk_hashes) return false;
    if (manifest->chunk_hashes)
        memcpy(ss->manifest.chunk_hashes, manifest->chunk_hashes, (size_t)n * 32);

    ss->chunk_states = calloc(n, sizeof(enum chunk_state));
    ss->chunk_peer = calloc(n, sizeof(int));
    ss->chunk_request_time = calloc(n, sizeof(int64_t));

    if (!ss->chunk_states || !ss->chunk_peer || !ss->chunk_request_time) {
        swarm_sync_free(ss);
        return false;
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
}

int32_t swarm_sync_assign_chunk(struct swarm_sync *ss, int peer_id)
{
    if (!ss || !ss->chunk_states) return -1;

    for (uint32_t i = 0; i < ss->manifest.num_chunks; i++) {
        if (ss->chunk_states[i] == CHUNK_NEEDED) {
            ss->chunk_states[i] = CHUNK_INFLIGHT;
            ss->chunk_peer[i] = peer_id;
            ss->chunk_request_time[i] = (int64_t)time(NULL);
            ss->chunks_inflight++;
            return (int32_t)i;
        }
    }
    return -1;
}

bool swarm_sync_receive_chunk(struct swarm_sync *ss,
                                const struct utxo_chunk *chunk,
                                int peer_id)
{
    if (!ss || !chunk) return false;

    uint32_t idx = chunk->chunk_index;
    if (idx >= ss->manifest.num_chunks) return false;

    /* Verify chunk hash against manifest */
    if (ss->manifest.chunk_hashes) {
        if (!fast_sync_verify_chunk(chunk, ss->manifest.chunk_hashes[idx])) {
            ss->chunk_states[idx] = CHUNK_FAILED;
            ss->chunk_peer[idx] = -1;
            if (ss->chunks_inflight > 0)
                ss->chunks_inflight--;
            ss->chunks_failed++;
            return false;
        }
    }

    /* Apply chunk to database */
    if (ss->datadir) {
        if (!fast_sync_apply_chunk(ss->datadir, chunk)) {
            ss->chunk_states[idx] = CHUNK_FAILED;
            ss->chunk_peer[idx] = -1;
            if (ss->chunks_inflight > 0)
                ss->chunks_inflight--;
            ss->chunks_failed++;
            return false;
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
    if (!ss) return false;
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
