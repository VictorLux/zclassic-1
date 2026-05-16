/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord models for ZCL Names (ZNAM).
 *
 * Three sibling models — one per table:
 *   znam_names         (struct znam_entry)
 *   znam_text_records  (struct znam_text_record)
 *   znam_addr_records  (struct znam_addr_record)
 *
 * Persistence SQL lives in lib/znam/src/znam.c; this file owns the
 * integrity contract for the three on-chain-derived tables. A
 * malformed znam row at rest means a malformed OP_RETURN was
 * accepted earlier in the pipeline — these validators are the last
 * checkpoint before the row is written. */

#include "models/znam.h"
#include <string.h>

DEFINE_MODEL_CALLBACKS(znam_entry)
DEFINE_MODEL_CALLBACKS(znam_text)
DEFINE_MODEL_CALLBACKS(znam_addr)

/* Names: lowercase alphanumeric + hyphens (ENS-like; first-come-first-served). */
static bool is_valid_znam_name(const char *s, size_t max_len)
{
    if (!s || !*s) return false;
    size_t len = strnlen(s, max_len + 1);
    if (len == 0 || len > max_len) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '-'))
            return false;
    }
    /* No leading/trailing hyphen */
    if (s[0] == '-' || s[len - 1] == '-') return false;
    return true;
}

bool db_znam_entry_validate(const struct znam_entry *entry,
                            struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!entry) {
        ar_errors_add(errors, "entry", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};

    validates_custom(errors,
        is_valid_znam_name(entry->name, ZNAM_NAME_MAX),
        "name", "is not a valid ZNAM name");
    validates_presence_of(errors, entry, owner_address);
    validates_range(errors, entry, target_type,
                    ZNAM_TYPE_ONION, ZNAM_TYPE_CONTENT);
    validates_presence_of(errors, entry, target_value);
    validates_custom(errors,
        strnlen(entry->target_value, ZNAM_VALUE_MAX + 1) <= ZNAM_VALUE_MAX,
        "target_value", "exceeds ZNAM_VALUE_MAX");
    validates_custom(errors,
        memcmp(entry->reg_txid, zero32, 32) != 0,
        "reg_txid", "can't be all zero");
    validates_non_negative(errors, entry, reg_height);

    return !ar_errors_any(errors);
}

bool db_znam_text_validate(const struct znam_text_record *rec,
                           struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!rec) {
        ar_errors_add(errors, "rec", "is NULL");
        return false;
    }

    validates_custom(errors,
        is_valid_znam_name(rec->name, ZNAM_NAME_MAX),
        "name", "is not a valid ZNAM name");
    validates_presence_of(errors, rec, key);
    validates_custom(errors,
        strnlen(rec->key, ZNAM_TEXT_KEY_MAX + 1) <= ZNAM_TEXT_KEY_MAX,
        "key", "exceeds ZNAM_TEXT_KEY_MAX");
    /* value may be empty (deletion via empty string) */
    validates_custom(errors,
        strnlen(rec->value, ZNAM_TEXT_VAL_MAX + 1) <= ZNAM_TEXT_VAL_MAX,
        "value", "exceeds ZNAM_TEXT_VAL_MAX");

    return !ar_errors_any(errors);
}

bool db_znam_addr_validate(const struct znam_addr_record *rec,
                           struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!rec) {
        ar_errors_add(errors, "rec", "is NULL");
        return false;
    }

    validates_custom(errors,
        is_valid_znam_name(rec->name, ZNAM_NAME_MAX),
        "name", "is not a valid ZNAM name");
    validates_range(errors, rec, coin_type,
                    ZNAM_TYPE_ONION, ZNAM_TYPE_CONTENT);
    validates_presence_of(errors, rec, address);
    validates_custom(errors,
        strnlen(rec->address, ZNAM_VALUE_MAX + 1) <= ZNAM_VALUE_MAX,
        "address", "exceeds ZNAM_VALUE_MAX");

    return !ar_errors_any(errors);
}
