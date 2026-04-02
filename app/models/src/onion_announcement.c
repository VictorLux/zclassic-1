/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "models/onion_announcement.h"
#include <ctype.h>
#include <string.h>
#include <time.h>

DEFINE_MODEL_CALLBACKS(onion_announcement)

static void onion_trim_ascii(char *str)
{
    size_t len;
    size_t start = 0;
    size_t end;

    if (!str || str[0] == '\0')
        return;
    len = strlen(str);
    while (start < len && isspace((unsigned char)str[start]))
        start++;
    end = len;
    while (end > start && isspace((unsigned char)str[end - 1]))
        end--;
    if (start > 0)
        memmove(str, str + start, end - start);
    str[end - start] = '\0';
}

static void onion_lower_ascii(char *str)
{
    if (!str)
        return;
    for (; *str; ++str)
        *str = (char)tolower((unsigned char)*str);
}

static bool onion_announcement_before_save(void *record, void *ctx)
{
    struct db_onion_announcement *ann = record;

    (void)ctx;
    if (!ann)
        return false;
    onion_trim_ascii(ann->onion_address);
    onion_trim_ascii(ann->script_hex);
    onion_lower_ascii(ann->onion_address);
    onion_lower_ascii(ann->script_hex);
    return true;
}

static struct ar_callbacks *onion_announcement_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_onion_announcement_callbacks();
    static bool callbacks_ready = false;

    if (!callbacks_ready) {
        ar_register_before_save(cbs, onion_announcement_before_save);
        callbacks_ready = true;
    }
    return cbs;
}

static bool onion_string_printable(const char *str)
{
    if (!str || str[0] == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)str; *p; ++p) {
        if (!isprint(*p))
            return false;
    }
    return true;
}

bool db_onion_announcement_validate(const struct db_onion_announcement *a,
                                    struct ar_errors *errors)
{
    size_t alen;

    ar_errors_clear(errors);
    validates_string_present(errors, a->onion_address, "onion_address");
    validates_non_negative(errors, a, announced_at);
    validates_custom(errors,
        strlen(a->onion_address) <= ONION_ADDRESS_MAX,
        "onion_address", "exceeds max length 127");
    validates_custom(errors,
        strlen(a->script_hex) <= ONION_SCRIPT_HEX_MAX,
        "script_hex", "exceeds max length 511");
    validates_custom(errors,
        onion_string_printable(a->onion_address),
        "onion_address", "contains non-printable characters");
    validates_custom(errors,
        onion_string_printable(a->script_hex) || a->script_hex[0] == '\0',
        "script_hex", "contains non-printable characters");
    alen = strlen(a->onion_address);
    validates_custom(errors,
        alen >= 7 &&
        strcmp(a->onion_address + alen - 6, ".onion") == 0,
        "onion_address", "must end with .onion");
    return !ar_errors_any(errors);
}

bool db_onion_announcement_save(struct node_db *ndb,
                                const struct db_onion_announcement *a)
{
    sqlite3_stmt *s = NULL;
    struct ar_callbacks *cbs;

    if (!ndb || !ndb->open || !a)
        return false;
    if (a->announced_at == 0)
        ((struct db_onion_announcement *)a)->announced_at = (int64_t)time(NULL);

    cbs = onion_announcement_callbacks_ready();
    AR_VALIDATE_RECORD(cbs, "onion_announcement", a,
                       db_onion_announcement_validate);
    if (!ar_run_before_save(cbs, (void *)a))
        return false;

    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO onion_announcements "
            "(onion_address,announced_at,script_hex) VALUES (?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_TEXT(s, 1, a->onion_address);
    AR_BIND_INT(s, 2, a->announced_at);
    AR_BIND_TEXT(s, 3, a->script_hex);
    if (!AR_STEP_DONE(s)) {
        AR_FINALIZE(s);
        return false;
    }
    AR_FINALIZE(s);
    ar_run_after_save(cbs, (void *)a);
    return true;
}

bool db_onion_announcement_exists(struct node_db *ndb,
                                  const char *onion_address)
{
    sqlite3_stmt *s = NULL;
    bool exists = false;

    if (!ndb || !ndb->open || !onion_address)
        return false;

    if (sqlite3_prepare_v2(ndb->db,
            "SELECT 1 FROM onion_announcements WHERE onion_address=?",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_TEXT(s, 1, onion_address);
    exists = AR_STEP_ROW(s);
    AR_FINALIZE(s);
    return exists;
}

int db_onion_announcement_recent(struct node_db *ndb,
                                 struct db_onion_announcement *out,
                                 size_t max)
{
    sqlite3_stmt *s = NULL;
    int count = 0;

    if (!ndb || !ndb->open || !out || max == 0)
        return 0;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT onion_address,announced_at,script_hex "
            "FROM onion_announcements "
            "ORDER BY announced_at DESC, onion_address ASC LIMIT ?",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;

    AR_BIND_INT(s, 1, (int)max);
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_STR(s, 0, out[count].onion_address,
                    sizeof(out[count].onion_address));
        out[count].announced_at = AR_COL_INT(s, 1);
        AR_READ_STR(s, 2, out[count].script_hex, sizeof(out[count].script_hex));
        count++;
    }
    AR_FINALIZE(s);
    return count;
}
