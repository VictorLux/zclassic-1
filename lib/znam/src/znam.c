/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Names (ZNAM) — parser, builder, and SQLite persistence.
 * Follows the same OP_RETURN encoding pattern as ZSLP. */

#include "znam/znam.h"
#include "models/activerecord.h"
#include "models/database.h"
#include "models/znam.h"
#include "platform/clock.h"
#include "storage/znam_projection.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "script/op_return_push.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

/* Script push helpers (read_push/push_data) live in
 * script/op_return_push.h — same encoding as ZSLP. */

/* ── Name validation ────────────────────────────────────────────── */

bool znam_validate_name(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > ZNAM_NAME_MAX) return false;
    if (name[0] == '-' || name[len - 1] == '-') return false;

    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

/* ── Parser ─────────────────────────────────────────────────────── */

bool znam_parse(const uint8_t *script, size_t script_len,
                struct znam_message *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->command = ZNAM_CMD_INVALID;

    const uint8_t *p = script;
    const uint8_t *end = script + script_len;

    /* Must start with OP_RETURN (0x6a) */
    if (p >= end || *p != 0x6a) return false;
    p++;

    /* Field 0: lokad_id — must be "ZNAM" (4 bytes) */
    const uint8_t *data;
    size_t len;
    p = read_push(p, end, &data, &len);
    if (!p || len != 4 || memcmp(data, ZNAM_LOKAD_BYTES, 4) != 0)
        return false;

    /* Field 1: version — must be 1 */
    p = read_push(p, end, &data, &len);
    if (!p || len != 1 || data[0] != 1) return false;

    /* Field 2: command */
    p = read_push(p, end, &data, &len);
    if (!p || len != 1) return false;

    uint8_t cmd = data[0];
    if (cmd < 1 || cmd > 6) return false;
    msg->command = (enum znam_command)cmd;

    /* Field 3: name (always present) */
    p = read_push(p, end, &data, &len);
    if (!p || len == 0 || len > ZNAM_NAME_MAX) return false;
    memcpy(msg->name, data, len);
    msg->name[len] = '\0';

    if (!znam_validate_name(msg->name)) {
        msg->command = ZNAM_CMD_INVALID;
        return false;
    }

    switch (msg->command) {
    case ZNAM_CMD_REGISTER:
    case ZNAM_CMD_UPDATE:
    case ZNAM_CMD_SET_RECORD:
        /* Field 4: target_type */
        p = read_push(p, end, &data, &len);
        if (!p || len != 1) return false;
        if (data[0] < 1 || data[0] > ZNAM_TYPE_CONTENT) return false;
        msg->target_type = data[0];

        /* Field 5: target_value */
        p = read_push(p, end, &data, &len);
        if (!p || len == 0 || len > ZNAM_VALUE_MAX) return false;
        memcpy(msg->target_value, data, len);
        msg->target_value[len] = '\0';
        return true;

    case ZNAM_CMD_TRANSFER:
        /* Field 4: new_owner address */
        p = read_push(p, end, &data, &len);
        if (!p || len == 0 || len > 63) return false;
        memcpy(msg->new_owner, data, len);
        msg->new_owner[len] = '\0';
        return true;

    case ZNAM_CMD_RENEW:
        /* No additional fields */
        return true;

    case ZNAM_CMD_SET_TEXT:
        /* Field 4: text key */
        p = read_push(p, end, &data, &len);
        if (!p || len == 0 || len > ZNAM_TEXT_KEY_MAX) return false;
        memcpy(msg->text_key, data, len);
        msg->text_key[len] = '\0';

        /* Field 5: text value */
        p = read_push(p, end, &data, &len);
        if (!p || len > ZNAM_TEXT_VAL_MAX) return false;
        memcpy(msg->text_value, data, len);
        msg->text_value[len] = '\0';
        return true;

    default:
        return false;
    }
}

/* ── Builders ───────────────────────────────────────────────────── */

static size_t znam_build_header(uint8_t *out, uint8_t command,
                                const char *name)
{
    size_t off = 0;
    out[off++] = 0x6a; /* OP_RETURN */

    off += push_data(out + off, (const uint8_t *)ZNAM_LOKAD_BYTES, 4);

    uint8_t version = 1;
    off += push_data(out + off, &version, 1);

    off += push_data(out + off, &command, 1);

    off += push_data(out + off, (const uint8_t *)name, strlen(name));
    return off;
}

size_t znam_build_register(uint8_t *out, size_t out_len,
                           const char *name, uint8_t target_type,
                           const char *target_value)
{
    if (!znam_validate_name(name) || !target_value) return 0;
    /* lift the literal-3 cap to ZNAM_TYPE_CONTENT so REGISTER
     * accepts the multi-coin types (BTC/LTC/DOGE) and CONTENT hash
     * that the parser and znam_build_set_record already round-trip. */
    if (target_type < 1 || target_type > ZNAM_TYPE_CONTENT) return 0;
    (void)out_len;

    size_t off = znam_build_header(out, ZNAM_CMD_REGISTER, name);
    off += push_data(out + off, &target_type, 1);
    off += push_data(out + off, (const uint8_t *)target_value,
                     strlen(target_value));
    return off;
}

size_t znam_build_update(uint8_t *out, size_t out_len,
                         const char *name, uint8_t target_type,
                         const char *target_value)
{
    if (!znam_validate_name(name) || !target_value) return 0;
    /* lift the literal-3 cap to ZNAM_TYPE_CONTENT (parser parity). */
    if (target_type < 1 || target_type > ZNAM_TYPE_CONTENT) return 0;
    (void)out_len;

    size_t off = znam_build_header(out, ZNAM_CMD_UPDATE, name);
    off += push_data(out + off, &target_type, 1);
    off += push_data(out + off, (const uint8_t *)target_value,
                     strlen(target_value));
    return off;
}

size_t znam_build_transfer(uint8_t *out, size_t out_len,
                           const char *name, const char *new_owner)
{
    if (!znam_validate_name(name) || !new_owner) return 0;
    (void)out_len;

    size_t off = znam_build_header(out, ZNAM_CMD_TRANSFER, name);
    off += push_data(out + off, (const uint8_t *)new_owner,
                     strlen(new_owner));
    return off;
}

size_t znam_build_renew(uint8_t *out, size_t out_len,
                        const char *name)
{
    if (!znam_validate_name(name)) return 0;
    (void)out_len;

    return znam_build_header(out, ZNAM_CMD_RENEW, name);
}

/* ENS-inspired: set additional address record for a coin type */
size_t znam_build_set_record(uint8_t *out, size_t out_len,
                             const char *name, uint8_t target_type,
                             const char *target_value)
{
    if (!znam_validate_name(name) || !target_value) return 0;
    if (target_type < 1 || target_type > ZNAM_TYPE_CONTENT) return 0;
    (void)out_len;

    size_t off = znam_build_header(out, ZNAM_CMD_SET_RECORD, name);
    off += push_data(out + off, &target_type, 1);
    off += push_data(out + off, (const uint8_t *)target_value,
                     strlen(target_value));
    return off;
}

/* ENS-inspired: set arbitrary text record (key-value) */
size_t znam_build_set_text(uint8_t *out, size_t out_len,
                           const char *name, const char *key,
                           const char *value)
{
    if (!znam_validate_name(name) || !key || !key[0]) return 0;
    if (strlen(key) > ZNAM_TEXT_KEY_MAX) return 0;
    if (value && strlen(value) > ZNAM_TEXT_VAL_MAX) return 0;
    (void)out_len;

    size_t off = znam_build_header(out, ZNAM_CMD_SET_TEXT, name);
    off += push_data(out + off, (const uint8_t *)key, strlen(key));
    off += push_data(out + off, (const uint8_t *)(value ? value : ""),
                     value ? strlen(value) : 0);
    return off;
}

/* ── SQLite Persistence ─────────────────────────────────────────── */

bool db_znam_save(struct node_db *ndb, const struct znam_entry *entry)
{
    if (!ndb || !ndb->open) LOG_FAIL("znam", "db_znam_save: db not open");
    if (!entry) LOG_FAIL("znam", "db_znam_save: entry is NULL");

    struct ar_callbacks *cbs = db_znam_entry_callbacks();
    AR_VALIDATE_RECORD(cbs, "znam_entry", entry, db_znam_entry_validate);
    if (!ar_run_before_save(cbs, (void *)entry))
        return false;

    const char *sql =
        "INSERT OR REPLACE INTO znam_names"
        "(name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid)"
        " VALUES(?,?,?,?,?,?,?)";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) LOG_FAIL("znam", "db_znam_save: prepare failed: %s", sqlite3_errmsg(ndb->db));

    sqlite3_bind_text(s, 1, entry->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, entry->owner_address, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 3, entry->target_type);
    sqlite3_bind_text(s, 4, entry->target_value, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 5, entry->reg_txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 6, entry->reg_height);
    sqlite3_bind_blob(s, 7, entry->last_update_txid, 32, SQLITE_STATIC);

    bool ok = AR_STEP_DONE(s);
    sqlite3_finalize(s);
    if (ok) ar_run_after_save(cbs, (void *)entry);
    if (ok && znam_projection_event_log()) {
        /* Projection event emit. Always emit REGISTER — the projection
         * uses INSERT OR REPLACE so re-registers are idempotent and the
         * primary-target fields stay in sync without a separate UPDATE. */
        if (!znam_projection_emit_register(
                entry->name, entry->owner_address, entry->target_type,
                entry->target_value, entry->reg_txid, entry->reg_height,
                (uint32_t)(clock_now_wall_ms() / 1000), 0)) {
            fprintf(stderr,  // obs-ok:znam-projection-emit
                    "znam projection emit failed for register\n");
        }
    }
    return ok;
}

static void row_to_znam(sqlite3_stmt *s, struct znam_entry *out)
{
    memset(out, 0, sizeof(*out));
    const char *name = (const char *)sqlite3_column_text(s, 0);
    if (name) snprintf(out->name, sizeof(out->name), "%s", name);

    const char *owner = (const char *)sqlite3_column_text(s, 1);
    if (owner) snprintf(out->owner_address, sizeof(out->owner_address),
                        "%s", owner);

    out->target_type = (uint8_t)sqlite3_column_int(s, 2);

    const char *val = (const char *)sqlite3_column_text(s, 3);
    if (val) snprintf(out->target_value, sizeof(out->target_value),
                      "%s", val);

    const void *blob = sqlite3_column_blob(s, 4);
    if (blob) memcpy(out->reg_txid, blob, 32);

    out->reg_height = (int32_t)sqlite3_column_int(s, 5);

    blob = sqlite3_column_blob(s, 6);
    if (blob) memcpy(out->last_update_txid, blob, 32);
}

bool db_znam_find(struct node_db *ndb, const char *name,
                  struct znam_entry *out)
{
    if (!ndb || !ndb->open) return false;

    const char *sql =
        "SELECT name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid"
        " FROM znam_names WHERE name=?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    bool found = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        row_to_znam(s, out);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

int db_znam_list(struct node_db *ndb, struct znam_entry *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;

    const char *sql =
        "SELECT name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid"
        " FROM znam_names ORDER BY reg_height DESC LIMIT ?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int(s, 1, (int)max);
    int count = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && (size_t)count < max) {
        row_to_znam(s, &out[count]);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

int db_znam_list_by_owner(struct node_db *ndb, const char *owner,
                          struct znam_entry *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;

    const char *sql =
        "SELECT name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid"
        " FROM znam_names WHERE owner_address=? ORDER BY name LIMIT ?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(s, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)max);
    int count = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && (size_t)count < max) {
        row_to_znam(s, &out[count]);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Text Records (ENS TextResolver) ───────────────────────────── */

bool db_znam_text_save(struct node_db *ndb, const char *name,
                       const char *key, const char *value)
{
    if (!ndb || !ndb->open) LOG_FAIL("znam", "db_znam_text_save: db not open");
    if (!name || !key) LOG_FAIL("znam", "db_znam_text_save: name/key NULL");

    struct znam_text_record rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.name,  sizeof(rec.name),  "%s", name);
    snprintf(rec.key,   sizeof(rec.key),   "%s", key);
    if (value) snprintf(rec.value, sizeof(rec.value), "%s", value);

    struct ar_callbacks *cbs = db_znam_text_callbacks();
    AR_VALIDATE_RECORD(cbs, "znam_text", &rec, db_znam_text_validate);
    if (!ar_run_before_save(cbs, &rec))
        return false;

    const char *sql =
        "INSERT OR REPLACE INTO znam_text_records(name,key,value) VALUES(?,?,?)";
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) LOG_FAIL("znam", "db_znam_text_save: prepare failed: %s", sqlite3_errmsg(ndb->db));
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, value, -1, SQLITE_STATIC);
    bool ok = AR_STEP_DONE(s);
    sqlite3_finalize(s);
    if (ok) ar_run_after_save(cbs, &rec);
    if (ok && znam_projection_event_log()) {
        /* Projection event emit. update_txid unknown at this layer; the
         * legacy caller didn't track it, so pass zeros — consumers should
         * tolerate (it is only used to bump last_update_txid for audit). */
        static const uint8_t zero_txid[32] = {0};
        if (!znam_projection_emit_update_text(name, key, value, zero_txid)) {
            fprintf(stderr,  // obs-ok:znam-projection-emit
                    "znam projection emit failed for text update\n");
        }
    }
    return ok;
}

bool db_znam_text_get(struct node_db *ndb, const char *name,
                      const char *key, char *value_out, size_t max)
{
    if (!ndb || !ndb->open) return false;
    const char *sql =
        "SELECT value FROM znam_text_records WHERE name=? AND key=?";
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    bool found = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) snprintf(value_out, max, "%s", v);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

int db_znam_text_list(struct node_db *ndb, const char *name,
                      struct znam_text_record *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    const char *sql =
        "SELECT name,key,value FROM znam_text_records WHERE name=? LIMIT ?";
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)max);
    int count = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        const char *n = (const char *)sqlite3_column_text(s, 0);
        if (n) snprintf(out[count].name, sizeof(out[count].name), "%s", n);
        const char *k = (const char *)sqlite3_column_text(s, 1);
        if (k) snprintf(out[count].key, sizeof(out[count].key), "%s", k);
        const char *v = (const char *)sqlite3_column_text(s, 2);
        if (v) snprintf(out[count].value, sizeof(out[count].value), "%s", v);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

/* ── Multi-Coin Address Records (ENS AddrResolver) ─────────────── */

bool db_znam_addr_save(struct node_db *ndb, const char *name,
                       uint8_t coin_type, const char *address)
{
    if (!ndb || !ndb->open) LOG_FAIL("znam", "db_znam_addr_save: db not open");
    if (!name || !address) LOG_FAIL("znam", "db_znam_addr_save: name/address NULL");

    struct znam_addr_record rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.name,    sizeof(rec.name),    "%s", name);
    rec.coin_type = coin_type;
    snprintf(rec.address, sizeof(rec.address), "%s", address);

    struct ar_callbacks *cbs = db_znam_addr_callbacks();
    AR_VALIDATE_RECORD(cbs, "znam_addr", &rec, db_znam_addr_validate);
    if (!ar_run_before_save(cbs, &rec))
        return false;

    const char *sql =
        "INSERT OR REPLACE INTO znam_addr_records(name,coin_type,address) VALUES(?,?,?)";
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) LOG_FAIL("znam", "db_znam_addr_save: prepare failed: %s", sqlite3_errmsg(ndb->db));
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, coin_type);
    sqlite3_bind_text(s, 3, address, -1, SQLITE_STATIC);
    bool ok = AR_STEP_DONE(s);
    sqlite3_finalize(s);
    if (ok) ar_run_after_save(cbs, &rec);
    if (ok && znam_projection_event_log()) {
        static const uint8_t zero_txid[32] = {0};
        if (!znam_projection_emit_update_addr(name, coin_type, address,
                                              zero_txid)) {
            fprintf(stderr,  // obs-ok:znam-projection-emit
                    "znam projection emit failed for addr update\n");
        }
    }
    return ok;
}

bool db_znam_addr_get(struct node_db *ndb, const char *name,
                      uint8_t coin_type, char *addr_out, size_t max)
{
    if (!ndb || !ndb->open) return false;
    const char *sql =
        "SELECT address FROM znam_addr_records WHERE name=? AND coin_type=?";
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, coin_type);
    bool found = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(s, 0);
        if (a) snprintf(addr_out, max, "%s", a);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}
