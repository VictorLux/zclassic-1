/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: FileService
 *
 * validates :ip, presence: true
 * validates :port, not_zero: true
 *
 * after_save -> emit EV_MODEL_SAVED */

#include "models/file_service.h"
#include "event/event.h"
#include <string.h>
#include <time.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(file_service)

/* ── Validation ────────────────────────────────────────────────── */

bool db_file_service_validate(const struct db_file_service *fs,
                              struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, fs, ip);
    validates_not_zero(errors, fs, port);
    return !ar_errors_any(errors);
}

/* ── Row Deserialization ──────────────────────────────────────── */

static void row_to_file_service(sqlite3_stmt *s,
                                struct db_file_service *out)
{
    AR_READ_BLOB(s, 0, out->ip, 16);
    out->port = (uint16_t)AR_COL_INT(s, 1);
    out->p2p_port = (uint16_t)AR_COL_INT(s, 2);
    out->last_seen = AR_COL_INT(s, 3);
    out->is_zcl23 = AR_COL_INT(s, 4) != 0;
}

/* ── Save (cached stmt) ──────────────────────────────────────── */

bool db_file_service_save(struct node_db *ndb,
                          const struct db_file_service *fs)
{
    if (!ndb->open) return false;

    /* Auto-timestamp */
    if (fs->last_seen == 0)
        ((struct db_file_service *)fs)->last_seen = (int64_t)time(NULL);

    struct ar_errors errors;
    if (!db_file_service_validate(fs, &errors)) {
        AR_LOG_VALIDATION_FAILURE("file_service", &errors);
        return false;
    }

    struct ar_callbacks *cbs = db_file_service_callbacks();
    if (!ar_run_before_save(cbs, (void *)fs)) return false;

    sqlite3_stmt *s = ndb->stmt_file_service_save;
    AR_RESET(s);
    AR_BIND_BLOB(s, 1, fs->ip, 16);
    AR_BIND_INT(s, 2, fs->port);
    AR_BIND_INT(s, 3, fs->p2p_port);
    AR_BIND_INT(s, 4, fs->last_seen);
    AR_BIND_INT(s, 5, fs->is_zcl23 ? 1 : 0);

    bool ok = AR_STEP_DONE(s);
    if (ok) ar_run_after_save(cbs, (void *)fs);
    return ok;
}

/* ── Find (cached stmt) ──────────────────────────────────────── */

bool db_file_service_find_by_addr(struct node_db *ndb,
                                  const uint8_t ip[16], uint16_t port,
                                  struct db_file_service *out)
{
    if (!ndb->open) return false;

    sqlite3_stmt *s = ndb->stmt_file_service_find;
    AR_RESET(s);
    AR_BIND_BLOB(s, 1, ip, 16);
    AR_BIND_INT(s, 2, port);

    if (!AR_STEP_ROW(s)) return false;
    memset(out, 0, sizeof(*out));
    row_to_file_service(s, out);
    return true;
}

/* ── Delete ────────────────────────────────────────────────────── */

bool db_file_service_delete(struct node_db *ndb,
                            const uint8_t ip[16], uint16_t port)
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_file_service_callbacks();
    struct db_file_service fs;
    memset(&fs, 0, sizeof(fs));
    memcpy(fs.ip, ip, 16);
    fs.port = port;
    if (!ar_run_before_destroy(cbs, &fs)) return false;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "DELETE FROM file_services WHERE ip=? AND port=?",
        -1, &s, NULL);
    if (!s) return false;
    AR_BIND_BLOB(s, 1, ip, 16);
    AR_BIND_INT(s, 2, port);
    bool ok = AR_STEP_DONE(s);
    AR_FINALIZE(s);

    if (ok) ar_run_after_destroy(cbs, &fs);
    return ok;
}

/* ── Count ─────────────────────────────────────────────────────── */

int db_file_service_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT COUNT(*) FROM file_services", -1, &s, NULL);
    int c = 0;
    if (s && AR_STEP_ROW(s))
        c = (int)AR_COL_INT(s, 0);
    AR_FINALIZE(s);
    return c;
}

/* ── Recent ────────────────────────────────────────────────────── */

int db_file_service_recent(struct node_db *ndb,
                           struct db_file_service *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT ip, port, p2p_port, last_seen, is_zcl23"
        " FROM file_services ORDER BY last_seen DESC LIMIT ?",
        -1, &s, NULL);
    if (!s) return 0;
    AR_BIND_INT(s, 1, (int)max);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        row_to_file_service(s, &out[count]);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}
