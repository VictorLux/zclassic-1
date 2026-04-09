/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Messaging (ZMSG) — P2P messaging implementation. */

#include "net/zmsg.h"
#include "core/serialize.h"
#include "crypto/sha3.h"
#include "models/database.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

/* ── Serialization ──────────────────────────────────────────────── */

bool zmsg_serialize(const struct zmsg_message *msg, struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, msg->msg_id, 32);
    ok &= stream_write_i64_le(s, msg->timestamp);

    /* sender: length-prefixed string */
    size_t slen = strlen(msg->sender);
    ok &= stream_write_u8(s, (uint8_t)(slen > 127 ? 127 : slen));
    ok &= stream_write(s, msg->sender, slen > 127 ? 127 : slen);

    /* recipient */
    size_t rlen = strlen(msg->recipient);
    ok &= stream_write_u8(s, (uint8_t)(rlen > 127 ? 127 : rlen));
    ok &= stream_write(s, msg->recipient, rlen > 127 ? 127 : rlen);

    /* body: 2-byte length prefix */
    size_t blen = strlen(msg->body);
    if (blen > ZMSG_MAX_BODY) blen = ZMSG_MAX_BODY;
    ok &= stream_write_u16_le(s, (uint16_t)blen);
    ok &= stream_write(s, msg->body, blen);

    return ok;
}

bool zmsg_deserialize(struct zmsg_message *msg, struct byte_stream *s)
{
    memset(msg, 0, sizeof(*msg));
    bool ok = true;

    ok &= stream_read(s, msg->msg_id, 32);
    ok &= stream_read_i64_le(s, &msg->timestamp);

    uint8_t slen = 0;
    ok &= stream_read_u8(s, &slen);
    if (!ok) return false;
    ok &= stream_read(s, msg->sender, slen);
    msg->sender[slen] = '\0';

    uint8_t rlen = 0;
    ok &= stream_read_u8(s, &rlen);
    if (!ok) return false;
    ok &= stream_read(s, msg->recipient, rlen);
    msg->recipient[rlen] = '\0';

    uint16_t blen = 0;
    ok &= stream_read_u16_le(s, &blen);
    if (!ok || blen > ZMSG_MAX_BODY) return false;
    ok &= stream_read(s, msg->body, blen);
    msg->body[blen] = '\0';

    return ok;
}

void zmsg_compute_id(const struct zmsg_message *msg, uint8_t out[32])
{
    struct sha3_256_ctx sha3;
    sha3_256_init(&sha3);
    sha3_256_write(&sha3, (const unsigned char *)&msg->timestamp, 8);
    sha3_256_write(&sha3, (const unsigned char *)msg->sender,
                   strlen(msg->sender));
    sha3_256_write(&sha3, (const unsigned char *)msg->body,
                   strlen(msg->body));
    sha3_256_finalize(&sha3, out);
}

/* ── In-Memory Store ────────────────────────────────────────────── */

static struct zmsg_message g_messages[ZMSG_MAX_STORED];
static int g_msg_count = 0;
static pthread_mutex_t g_zmsg_mutex = PTHREAD_MUTEX_INITIALIZER;

bool zmsg_store_add(const struct zmsg_message *msg)
{
    pthread_mutex_lock(&g_zmsg_mutex);

    /* Check for duplicate */
    for (int i = 0; i < g_msg_count; i++) {
        if (memcmp(g_messages[i].msg_id, msg->msg_id, 32) == 0) {
            pthread_mutex_unlock(&g_zmsg_mutex);
            return false;
        }
    }

    if (g_msg_count >= ZMSG_MAX_STORED) {
        /* Evict oldest */
        memmove(&g_messages[0], &g_messages[1],
                (ZMSG_MAX_STORED - 1) * sizeof(struct zmsg_message));
        g_msg_count = ZMSG_MAX_STORED - 1;
    }

    g_messages[g_msg_count] = *msg;
    g_msg_count++;
    pthread_mutex_unlock(&g_zmsg_mutex);
    return true;
}

int zmsg_store_list(struct zmsg_message *out, size_t max,
                    bool unread_only)
{
    pthread_mutex_lock(&g_zmsg_mutex);
    int count = 0;
    /* Return newest first */
    for (int i = g_msg_count - 1; i >= 0 && (size_t)count < max; i--) {
        if (unread_only && g_messages[i].read) continue;
        out[count++] = g_messages[i];
    }
    pthread_mutex_unlock(&g_zmsg_mutex);
    return count;
}

bool zmsg_store_mark_read(const uint8_t msg_id[32])
{
    pthread_mutex_lock(&g_zmsg_mutex);
    for (int i = 0; i < g_msg_count; i++) {
        if (memcmp(g_messages[i].msg_id, msg_id, 32) == 0) {
            g_messages[i].read = true;
            pthread_mutex_unlock(&g_zmsg_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_zmsg_mutex);
    return false;
}

int zmsg_store_count(void)
{
    pthread_mutex_lock(&g_zmsg_mutex);
    int c = g_msg_count;
    pthread_mutex_unlock(&g_zmsg_mutex);
    return c;
}

/* ── SQLite Persistence ─────────────────────────────────────────── */

bool db_zmsg_save(struct node_db *ndb, const struct zmsg_message *msg)
{
    if (!ndb || !ndb->open) return false;

    const char *sql =
        "INSERT OR IGNORE INTO zmsg_messages"
        "(msg_id,direction,channel,sender,recipient,body,"
        "timestamp,txid,read)"
        " VALUES(?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_blob(s, 1, msg->msg_id, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, msg->direction);
    sqlite3_bind_int(s, 3, msg->channel);
    sqlite3_bind_text(s, 4, msg->sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 5, msg->recipient, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 6, msg->body, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 7, msg->timestamp);

    /* Check if txid is non-zero */
    uint8_t zero[32] = {0};
    if (memcmp(msg->txid, zero, 32) != 0)
        sqlite3_bind_blob(s, 8, msg->txid, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 8);

    sqlite3_bind_int(s, 9, msg->read ? 1 : 0);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

static void row_to_zmsg(sqlite3_stmt *s, struct zmsg_message *out)
{
    memset(out, 0, sizeof(*out));
    const void *blob = sqlite3_column_blob(s, 0);
    if (blob) memcpy(out->msg_id, blob, 32);

    out->direction = sqlite3_column_int(s, 1);
    out->channel = sqlite3_column_int(s, 2);

    const char *str = (const char *)sqlite3_column_text(s, 3);
    if (str) snprintf(out->sender, sizeof(out->sender), "%s", str);

    str = (const char *)sqlite3_column_text(s, 4);
    if (str) snprintf(out->recipient, sizeof(out->recipient), "%s", str);

    str = (const char *)sqlite3_column_text(s, 5);
    if (str) snprintf(out->body, sizeof(out->body), "%s", str);

    out->timestamp = sqlite3_column_int64(s, 6);

    blob = sqlite3_column_blob(s, 7);
    if (blob) memcpy(out->txid, blob, 32);

    out->read = sqlite3_column_int(s, 8) != 0;
}

int db_zmsg_list(struct node_db *ndb, struct zmsg_message *out,
                 size_t max, bool unread_only)
{
    if (!ndb || !ndb->open) return 0;

    const char *sql = unread_only
        ? "SELECT msg_id,direction,channel,sender,recipient,body,"
          "timestamp,txid,read FROM zmsg_messages "
          "WHERE read=0 ORDER BY timestamp DESC LIMIT ?"
        : "SELECT msg_id,direction,channel,sender,recipient,body,"
          "timestamp,txid,read FROM zmsg_messages "
          "ORDER BY timestamp DESC LIMIT ?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int(s, 1, (int)max);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && (size_t)count < max) {
        row_to_zmsg(s, &out[count]);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

bool db_zmsg_mark_read(struct node_db *ndb, const uint8_t msg_id[32])
{
    if (!ndb || !ndb->open) return false;

    const char *sql = "UPDATE zmsg_messages SET read=1 WHERE msg_id=?";
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_blob(s, 1, msg_id, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
