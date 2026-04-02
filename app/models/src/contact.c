/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "models/contact.h"
#include <ctype.h>
#include <string.h>
#include <time.h>

DEFINE_MODEL_CALLBACKS(contact)

static void contact_trim_ascii(char *str)
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

static bool contact_before_save(void *record, void *ctx)
{
    struct db_contact *contact = record;

    (void)ctx;
    if (!contact)
        return false;
    contact_trim_ascii(contact->address);
    contact_trim_ascii(contact->name);
    return true;
}

static struct ar_callbacks *contact_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_contact_callbacks();
    static bool callbacks_ready = false;

    if (!callbacks_ready) {
        ar_register_before_save(cbs, contact_before_save);
        callbacks_ready = true;
    }
    return cbs;
}

static bool contact_string_printable(const char *str)
{
    if (!str || str[0] == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)str; *p; ++p) {
        if (!isprint(*p))
            return false;
    }
    return true;
}

bool db_contact_validate(const struct db_contact *c, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_string_present(errors, c->address, "address");
    validates_string_present(errors, c->name, "name");
    validates_non_negative(errors, c, last_used);
    validates_custom(errors,
        strlen(c->address) <= CONTACT_ADDRESS_MAX,
        "address", "exceeds max length 255");
    validates_custom(errors,
        strlen(c->name) <= CONTACT_NAME_MAX,
        "name", "exceeds max length 63");
    validates_custom(errors,
        contact_string_printable(c->address),
        "address", "contains non-printable characters");
    validates_custom(errors,
        contact_string_printable(c->name),
        "name", "contains non-printable characters");
    return !ar_errors_any(errors);
}

bool db_contact_save(struct node_db *ndb, const struct db_contact *c)
{
    sqlite3_stmt *s = NULL;
    struct ar_callbacks *cbs;

    if (!ndb || !ndb->open || !c)
        return false;
    if (c->last_used == 0)
        ((struct db_contact *)c)->last_used = (int64_t)time(NULL);

    cbs = contact_callbacks_ready();
    AR_VALIDATE_RECORD(cbs, "contact", c, db_contact_validate);
    if (!ar_run_before_save(cbs, (void *)c))
        return false;

    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO contacts (address,name,last_used) "
            "VALUES (?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_TEXT(s, 1, c->address);
    AR_BIND_TEXT(s, 2, c->name);
    AR_BIND_INT(s, 3, c->last_used);
    if (!AR_STEP_DONE(s)) {
        AR_FINALIZE(s);
        return false;
    }
    AR_FINALIZE(s);
    ar_run_after_save(cbs, (void *)c);
    return true;
}

int db_contact_recent(struct node_db *ndb, struct db_contact *out, size_t max)
{
    sqlite3_stmt *s = NULL;
    int count = 0;

    if (!ndb || !ndb->open || !out || max == 0)
        return 0;

    if (sqlite3_prepare_v2(ndb->db,
            "SELECT address,name,last_used FROM contacts "
            "ORDER BY last_used DESC LIMIT ?",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;

    AR_BIND_INT(s, 1, (int)max);
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_STR(s, 0, out[count].address, sizeof(out[count].address));
        AR_READ_STR(s, 1, out[count].name, sizeof(out[count].name));
        out[count].last_used = AR_COL_INT(s, 2);
        count++;
    }
    AR_FINALIZE(s);
    return count;
}
