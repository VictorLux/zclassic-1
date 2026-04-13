/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Market: Crypto-incentivized P2P file sharing.
 *
 * In-memory offer cache + SQLite persistence + serialization.
 * Gossip logic: receive offers, decrement TTL, re-broadcast. */

#include "net/file_market.h"
#include "core/serialize.h"
#include "models/database.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include "util/log_macros.h"

/* ── In-Memory Offer Cache ──────────────────────────────────────── */

static struct file_offer g_offers[FILE_MARKET_MAX_OFFERS];
static int g_offer_count = 0;
static pthread_mutex_t g_market_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Serialization ──────────────────────────────────────────────── */

bool file_offer_serialize(const struct file_offer *offer,
                          struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, offer->root_hash, 32);

    /* filename: length-prefixed, max 255 bytes */
    size_t namelen = strlen(offer->filename);
    if (namelen > 255) namelen = 255;
    ok &= stream_write_u8(s, (uint8_t)namelen);
    ok &= stream_write(s, offer->filename, namelen);

    ok &= stream_write_u64_le(s, offer->size_bytes);
    ok &= stream_write_u32_le(s, offer->num_chunks);
    ok &= stream_write_i64_le(s, offer->price_per_mb);
    ok &= stream_write(s, offer->z_addr, 43);
    ok &= stream_write(s, offer->peer_ip, 16);
    ok &= stream_write_u16_le(s, offer->peer_port);
    ok &= stream_write_u8(s, offer->ttl);
    return ok;
}

bool file_offer_deserialize(struct file_offer *offer,
                            struct byte_stream *s)
{
    memset(offer, 0, sizeof(*offer));
    bool ok = true;

    ok &= stream_read(s, offer->root_hash, 32);

    uint8_t namelen = 0;
    ok &= stream_read_u8(s, &namelen);
    if (!ok) LOG_FAIL("file_market", "file_offer_deserialize: stream read failed at name length");
    ok &= stream_read(s, offer->filename, namelen);
    offer->filename[namelen] = '\0';

    ok &= stream_read_u64_le(s, &offer->size_bytes);
    ok &= stream_read_u32_le(s, &offer->num_chunks);
    ok &= stream_read_i64_le(s, &offer->price_per_mb);
    ok &= stream_read(s, offer->z_addr, 43);
    ok &= stream_read(s, offer->peer_ip, 16);
    ok &= stream_read_u16_le(s, &offer->peer_port);
    ok &= stream_read_u8(s, &offer->ttl);

    if (ok) offer->last_seen = (int64_t)time(NULL);
    return ok;
}

bool file_challenge_serialize(const struct file_challenge *chal,
                              struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, chal->root_hash, 32);
    ok &= stream_write_u32_le(s, chal->chunk_index);
    return ok;
}

bool file_challenge_deserialize(struct file_challenge *chal,
                                struct byte_stream *s)
{
    memset(chal, 0, sizeof(*chal));
    bool ok = true;
    ok &= stream_read(s, chal->root_hash, 32);
    ok &= stream_read_u32_le(s, &chal->chunk_index);
    return ok;
}

bool file_proof_serialize(const struct file_proof *proof,
                          struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, proof->root_hash, 32);
    ok &= stream_write_u32_le(s, proof->chunk_index);
    ok &= stream_write(s, proof->chunk_hash, 32);
    return ok;
}

bool file_proof_deserialize(struct file_proof *proof,
                            struct byte_stream *s)
{
    memset(proof, 0, sizeof(*proof));
    bool ok = true;
    ok &= stream_read(s, proof->root_hash, 32);
    ok &= stream_read_u32_le(s, &proof->chunk_index);
    ok &= stream_read(s, proof->chunk_hash, 32);
    return ok;
}

bool file_payment_serialize(const struct file_payment *pay,
                            struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, pay->root_hash, 32);
    ok &= stream_write(s, pay->txid, 32);
    ok &= stream_write_u32_le(s, pay->chunks_paid);
    ok &= stream_write_u32_le(s, pay->chunk_start);
    return ok;
}

bool file_payment_deserialize(struct file_payment *pay,
                              struct byte_stream *s)
{
    memset(pay, 0, sizeof(*pay));
    bool ok = true;
    ok &= stream_read(s, pay->root_hash, 32);
    ok &= stream_read(s, pay->txid, 32);
    ok &= stream_read_u32_le(s, &pay->chunks_paid);
    ok &= stream_read_u32_le(s, &pay->chunk_start);
    return ok;
}

/* ── Offer Cache ────────────────────────────────────────────────── */

bool file_market_add_offer(const struct file_offer *offer)
{
    if (!offer)
        LOG_FAIL("file_market", "add_offer: null offer");
    if (offer->ttl == 0 || offer->num_chunks == 0)
        LOG_FAIL("file_market", "add_offer: invalid offer ttl=%u chunks=%u",
                 offer->ttl, offer->num_chunks);

    pthread_mutex_lock(&g_market_mutex);

    /* Check for existing offer with same root_hash — update if newer */
    for (int i = 0; i < g_offer_count; i++) {
        if (memcmp(g_offers[i].root_hash, offer->root_hash, 32) == 0) {
            g_offers[i] = *offer;
            g_offers[i].last_seen = (int64_t)time(NULL);
            pthread_mutex_unlock(&g_market_mutex);
            return false; /* updated, not new */
        }
    }

    /* Add new offer */
    if (g_offer_count >= FILE_MARKET_MAX_OFFERS) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < g_offer_count; i++) {
            if (g_offers[i].last_seen < g_offers[oldest].last_seen)
                oldest = i;
        }
        g_offers[oldest] = *offer;
        g_offers[oldest].last_seen = (int64_t)time(NULL);
        pthread_mutex_unlock(&g_market_mutex);
        return true;
    }

    g_offers[g_offer_count] = *offer;
    g_offers[g_offer_count].last_seen = (int64_t)time(NULL);
    g_offer_count++;
    pthread_mutex_unlock(&g_market_mutex);
    return true;
}

int file_market_get_offers(struct file_offer *out, size_t max)
{
    pthread_mutex_lock(&g_market_mutex);
    int count = g_offer_count;
    if ((size_t)count > max) count = (int)max;
    memcpy(out, g_offers, count * sizeof(struct file_offer));
    pthread_mutex_unlock(&g_market_mutex);
    return count;
}

bool file_market_find_offer(const uint8_t root_hash[32],
                            struct file_offer *out)
{
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_offer_count; i++) {
        if (memcmp(g_offers[i].root_hash, root_hash, 32) == 0) {
            *out = g_offers[i];
            pthread_mutex_unlock(&g_market_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return false;
}

int file_market_prune(int64_t max_age)
{
    int64_t cutoff = (int64_t)time(NULL) - max_age;
    int pruned = 0;

    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_offer_count; ) {
        if (g_offers[i].last_seen < cutoff) {
            g_offers[i] = g_offers[g_offer_count - 1];
            g_offer_count--;
            pruned++;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return pruned;
}

int file_market_count(void)
{
    pthread_mutex_lock(&g_market_mutex);
    int c = g_offer_count;
    pthread_mutex_unlock(&g_market_mutex);
    return c;
}

/* ── Download Sessions ──────────────────────────────────────────── */

#define MAX_DOWNLOADS 16
static struct file_download g_downloads[MAX_DOWNLOADS];
static int g_download_count = 0;

int file_market_start_download(const uint8_t root_hash[32],
                               const char *output_path)
{
    struct file_offer offer;
    if (!file_market_find_offer(root_hash, &offer))
        LOG_ERR("file_market", "start_download: offer not found for root_hash");

    pthread_mutex_lock(&g_market_mutex);
    if (g_download_count >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&g_market_mutex);
        LOG_ERR("file_market", "start_download: max downloads reached (%d)", MAX_DOWNLOADS);
    }

    int idx = g_download_count++;
    memset(&g_downloads[idx], 0, sizeof(g_downloads[idx]));
    g_downloads[idx].offer = offer;
    g_downloads[idx].state = FDL_CHALLENGING;
    if (output_path)
        snprintf(g_downloads[idx].output_path, sizeof(g_downloads[idx].output_path),
                 "%s", output_path);
    pthread_mutex_unlock(&g_market_mutex);
    return idx;
}

bool file_market_get_download(const uint8_t root_hash[32],
                              struct file_download *out)
{
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_download_count; i++) {
        if (memcmp(g_downloads[i].offer.root_hash, root_hash, 32) == 0) {
            *out = g_downloads[i];
            pthread_mutex_unlock(&g_market_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return false;
}

/* ── SQLite Persistence ─────────────────────────────────────────── */

bool db_file_offer_save(struct node_db *ndb,
                        const struct file_offer *offer)
{
    if (!ndb || !ndb->open) LOG_FAIL("file_market", "db_file_offer_save: db not open");

    const char *sql =
        "INSERT OR REPLACE INTO file_offers"
        "(root_hash,filename,size_bytes,num_chunks,price_per_mb,"
        "z_addr,peer_ip,peer_port,last_seen,ttl)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) LOG_FAIL("file_market", "db_file_offer_save: prepare failed: %s", sqlite3_errmsg(ndb->db));

    sqlite3_bind_blob(s, 1, offer->root_hash, 32, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, offer->filename, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, (int64_t)offer->size_bytes);
    sqlite3_bind_int(s, 4, (int)offer->num_chunks);
    sqlite3_bind_int64(s, 5, offer->price_per_mb);
    sqlite3_bind_blob(s, 6, offer->z_addr, 43, SQLITE_STATIC);
    sqlite3_bind_blob(s, 7, offer->peer_ip, 16, SQLITE_STATIC);
    sqlite3_bind_int(s, 8, offer->peer_port);
    sqlite3_bind_int64(s, 9, offer->last_seen ? offer->last_seen : (int64_t)time(NULL));
    sqlite3_bind_int(s, 10, offer->ttl);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

static void row_to_file_offer(sqlite3_stmt *s, struct file_offer *out)
{
    memset(out, 0, sizeof(*out));
    const void *blob = sqlite3_column_blob(s, 0);
    if (blob) memcpy(out->root_hash, blob, 32);

    const char *name = (const char *)sqlite3_column_text(s, 1);
    if (name) snprintf(out->filename, sizeof(out->filename), "%s", name);

    out->size_bytes = (uint64_t)sqlite3_column_int64(s, 2);
    out->num_chunks = (uint32_t)sqlite3_column_int(s, 3);
    out->price_per_mb = sqlite3_column_int64(s, 4);

    blob = sqlite3_column_blob(s, 5);
    if (blob) memcpy(out->z_addr, blob, 43);

    blob = sqlite3_column_blob(s, 6);
    if (blob) memcpy(out->peer_ip, blob, 16);

    out->peer_port = (uint16_t)sqlite3_column_int(s, 7);
    out->last_seen = sqlite3_column_int64(s, 8);
    out->ttl = (uint8_t)sqlite3_column_int(s, 9);
}

int db_file_offer_list(struct node_db *ndb,
                       struct file_offer *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;

    const char *sql =
        "SELECT root_hash,filename,size_bytes,num_chunks,price_per_mb,"
        "z_addr,peer_ip,peer_port,last_seen,ttl"
        " FROM file_offers ORDER BY last_seen DESC LIMIT ?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int(s, 1, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        row_to_file_offer(s, &out[count]);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

bool db_file_offer_find(struct node_db *ndb,
                        const uint8_t root_hash[32],
                        struct file_offer *out)
{
    if (!ndb || !ndb->open) LOG_FAIL("file_market", "db_file_offer_find: db not open");

    const char *sql =
        "SELECT root_hash,filename,size_bytes,num_chunks,price_per_mb,"
        "z_addr,peer_ip,peer_port,last_seen,ttl"
        " FROM file_offers WHERE root_hash=?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) LOG_FAIL("file_market", "db_file_offer_find: prepare failed: %s", sqlite3_errmsg(ndb->db));

    sqlite3_bind_blob(s, 1, root_hash, 32, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        row_to_file_offer(s, out);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

int db_file_offer_prune(struct node_db *ndb, int64_t max_age)
{
    if (!ndb || !ndb->open) return 0;

    int64_t cutoff = (int64_t)time(NULL) - max_age;
    char sql[128];
    snprintf(sql, sizeof(sql),
             "DELETE FROM file_offers WHERE last_seen < %lld",
             (long long)cutoff);

    char *err = NULL;
    int rc = sqlite3_exec(ndb->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return 0;
    }
    return sqlite3_changes(ndb->db);
}
