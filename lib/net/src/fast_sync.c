/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast P2P sync: UTXO snapshot transfer between zclassic23 nodes. */

#include "net/fast_sync.h"
#include "models/database.h"
#include "models/utxo.h"
#include "core/hash.h"
#include "crypto/sha256.h"
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

    /* SHA256(peer_id || timestamp || nonce) must have leading zeros */
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_write(&ctx, pow->peer_id, 32);
    sha256_write(&ctx, (const unsigned char *)&pow->timestamp, 8);
    sha256_write(&ctx, (const unsigned char *)&pow->nonce, 8);
    unsigned char hash[32];
    sha256_finalize(&ctx, hash);

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
