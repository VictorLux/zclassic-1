/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord-style ORM base for C23.
 *
 * Pattern:
 *   struct db_block blk = {.height = 100, ...};
 *   if (!db_block_save(&ndb, &blk))
 *       printf("Error: %s\n", ndb.last_error);
 *
 * CRUD convention for every model:
 *   _save()         — INSERT OR REPLACE (create/update)
 *   _find()         — SELECT by primary key
 *   _find_by_*()    — SELECT by indexed column
 *   _delete()       — DELETE by primary key
 *   _count()        — SELECT COUNT(*)
 *   _each()         — iterate all rows via callback
 *   _where_*()      — filtered queries return arrays
 *
 * Lifecycle:
 *   validate → before_save → SQL INSERT/UPDATE → after_save
 *   before_destroy → SQL DELETE → after_destroy
 *
 * Relationships:
 *   db_block_transactions()  — Block has_many Transactions
 *   db_tx_block()            — Transaction belongs_to Block
 *   db_utxo_transaction()    — UTXO belongs_to Transaction
 *   db_wallet_utxo_key()     — WalletUTXO belongs_to WalletKey
 *   db_sapling_note_key()    — SaplingNote belongs_to SaplingKey
 */

#ifndef ZCL_DB_ACTIVERECORD_H
#define ZCL_DB_ACTIVERECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Maximum error message length */
#define AR_ERROR_MAX 256

/* Maximum callbacks per model per hook */
#define AR_MAX_CALLBACKS 4

/* Validation result — accumulates errors like ActiveModel::Errors */
struct ar_errors {
    char messages[8][AR_ERROR_MAX];
    int count;
};

static inline void ar_errors_clear(struct ar_errors *e)
{
    e->count = 0;
}

static inline void ar_errors_add(struct ar_errors *e, const char *field,
                                  const char *msg)
{
    if (e->count >= 8) return;
    snprintf(e->messages[e->count], AR_ERROR_MAX, "%s %s", field, msg);
    e->count++;
}

static inline bool ar_errors_any(const struct ar_errors *e)
{
    return e->count > 0;
}

static inline const char *ar_errors_full(const struct ar_errors *e)
{
    return e->count > 0 ? e->messages[0] : "";
}

/* All error messages joined by "; " */
static inline void ar_errors_full_messages(const struct ar_errors *e,
                                            char *buf, size_t buflen)
{
    if (e->count == 0) { buf[0] = '\0'; return; }
    buf[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < e->count && off < buflen - 1; i++) {
        if (i > 0) {
            int n = snprintf(buf + off, buflen - off, "; ");
            off += (size_t)n;
        }
        int n = snprintf(buf + off, buflen - off, "%s", e->messages[i]);
        off += (size_t)n;
    }
}

/* Validation macros — use in validate_* functions */

#define validates_presence_of(errors, record, field) do { \
    static const uint8_t _zero[sizeof((record)->field)] = {0}; \
    if (memcmp(&(record)->field, _zero, sizeof((record)->field)) == 0) \
        ar_errors_add(errors, #field, "can't be blank"); \
} while (0)

#define validates_range(errors, record, field, min_val, max_val) do { \
    if ((record)->field < (min_val) || (record)->field > (max_val)) \
        ar_errors_add(errors, #field, "is out of range"); \
} while (0)

#define validates_positive(errors, record, field) do { \
    if ((record)->field <= 0) \
        ar_errors_add(errors, #field, "must be positive"); \
} while (0)

#define validates_blob_size(errors, data, len, expected, name) do { \
    if ((len) != (expected)) \
        ar_errors_add(errors, name, "has wrong size"); \
} while (0)

#define validates_non_negative(errors, record, field) do { \
    if ((record)->field < 0) \
        ar_errors_add(errors, #field, "must be non-negative"); \
} while (0)

#define validates_length_of(errors, record, field, min_len, max_len) do { \
    if ((record)->field < (min_len) || (record)->field > (max_len)) \
        ar_errors_add(errors, #field, "has wrong length"); \
} while (0)

#define validates_inclusion_of(errors, record, field, vals, nvals) do { \
    bool _found = false; \
    for (size_t _i = 0; _i < (nvals); _i++) \
        if ((record)->field == (vals)[_i]) { _found = true; break; } \
    if (!_found) \
        ar_errors_add(errors, #field, "is not included in the list"); \
} while (0)

#define validates_max(errors, record, field, max_val) do { \
    if ((record)->field > (max_val)) \
        ar_errors_add(errors, #field, "exceeds maximum"); \
} while (0)

#define validates_min(errors, record, field, min_val) do { \
    if ((record)->field < (min_val)) \
        ar_errors_add(errors, #field, "below minimum"); \
} while (0)

#define validates_not_zero(errors, record, field) do { \
    if ((record)->field == 0) \
        ar_errors_add(errors, #field, "can't be zero"); \
} while (0)

/* Validates a money amount is in consensus range [0, MAX_MONEY].
 * Use for ZCL values: UTXO amounts, fees, balances. */
#define validates_money_range(errors, record, field, max_money) do { \
    if ((record)->field < 0 || (record)->field > (max_money)) \
        ar_errors_add(errors, #field, "is out of money range"); \
} while (0)

/* Validates string field is not empty. */
#define validates_string_present(errors, str, name) do { \
    if (!(str) || (str)[0] == '\0') \
        ar_errors_add(errors, name, "can't be blank"); \
} while (0)

/* Validates a custom condition with a custom message. */
#define validates_custom(errors, cond, field, msg) do { \
    if (!(cond)) \
        ar_errors_add(errors, field, msg); \
} while (0)

/* ── DRY Macros ────────────────────────────────────────────────── */

/* Define a model's callback registry with lazy init.
 * Usage: DEFINE_MODEL_CALLBACKS(utxo) generates db_utxo_callbacks(). */
#define DEFINE_MODEL_CALLBACKS(model) \
    static struct ar_callbacks model##_cbs; \
    static bool model##_cbs_init = false; \
    struct ar_callbacks *db_##model##_callbacks(void) { \
        if (!model##_cbs_init) { \
            ar_callbacks_init(&model##_cbs); \
            model##_cbs_init = true; \
        } \
        return &model##_cbs; \
    }

/* Safe malloc with NULL check — returns false from enclosing function. */
#define AR_MALLOC_OR_FAIL(ptr, size) do { \
    (ptr) = malloc(size); \
    if (!(ptr)) return false; \
} while (0)

/* Safe blob read from SQLite with size validation. */
#define AR_READ_BLOB(stmt, col, dest, expected_len) do { \
    int _blen = sqlite3_column_bytes(stmt, col); \
    const void *_bdata = sqlite3_column_blob(stmt, col); \
    if (_bdata && _blen >= (int)(expected_len)) \
        memcpy(dest, _bdata, expected_len); \
    else \
        memset(dest, 0, expected_len); \
} while (0)

/* Safe string read from SQLite. */
#define AR_READ_STR(stmt, col, dest, max_len) do { \
    const char *_s = (const char *)sqlite3_column_text(stmt, col); \
    if (_s) { \
        size_t _l = strlen(_s); \
        if (_l >= (max_len)) _l = (max_len) - 1; \
        memcpy(dest, _s, _l); \
        (dest)[_l] = 0; \
    } else { \
        (dest)[0] = 0; \
    } \
} while (0)

/* ── SQLite Statement Macros ──────────────────────────────────── *
 * Eliminate prepare/bind/step/finalize boilerplate.
 *
 * Usage:
 *   AR_PREPARE(ndb, s, "SELECT * FROM blocks WHERE height = ?");
 *   AR_BIND_INT(s, 1, height);
 *   if (!AR_STEP_ROW(s)) { AR_FINALIZE(s); return false; }
 *   int h = AR_COL_INT(s, 0);
 *   AR_FINALIZE(s);
 */

/* Prepare a statement. Sets ndb->last_error on failure, returns false. */
#define AR_PREPARE(ndb, stmt, sql) do { \
    if (sqlite3_prepare_v2((ndb)->db, sql, -1, &(stmt), NULL) != SQLITE_OK) { \
        snprintf((ndb)->last_error, sizeof((ndb)->last_error), \
                 "prepare failed: %s", sqlite3_errmsg((ndb)->db)); \
        return false; \
    } \
} while (0)

/* Prepare, but don't return — for functions that need custom error handling. */
#define AR_PREPARE_OR(ndb, stmt, sql, fail_action) do { \
    if (sqlite3_prepare_v2((ndb)->db, sql, -1, &(stmt), NULL) != SQLITE_OK) { \
        snprintf((ndb)->last_error, sizeof((ndb)->last_error), \
                 "prepare failed: %s", sqlite3_errmsg((ndb)->db)); \
        fail_action; \
    } \
} while (0)

/* Bind helpers */
#define AR_BIND_INT(stmt, pos, val) \
    sqlite3_bind_int64(stmt, pos, (int64_t)(val))

#define AR_BIND_BLOB(stmt, pos, data, len) \
    sqlite3_bind_blob(stmt, pos, data, (int)(len), SQLITE_STATIC)

#define AR_BIND_TEXT(stmt, pos, str) \
    sqlite3_bind_text(stmt, pos, str, -1, SQLITE_STATIC)

#define AR_BIND_DOUBLE(stmt, pos, val) \
    sqlite3_bind_double(stmt, pos, val)

#define AR_BIND_NULL(stmt, pos) \
    sqlite3_bind_null(stmt, pos)

/* Step and check result */
#define AR_STEP_ROW(stmt)  (sqlite3_step(stmt) == SQLITE_ROW)
#define AR_STEP_DONE(stmt) (sqlite3_step(stmt) == SQLITE_DONE)

/* Column readers */
#define AR_COL_INT(stmt, col)    sqlite3_column_int64(stmt, col)
#define AR_COL_DOUBLE(stmt, col) sqlite3_column_double(stmt, col)
#define AR_COL_TEXT(stmt, col)   ((const char *)sqlite3_column_text(stmt, col))
#define AR_COL_BYTES(stmt, col)  sqlite3_column_bytes(stmt, col)

/* Finalize */
#define AR_FINALIZE(stmt) do { \
    if (stmt) { sqlite3_finalize(stmt); (stmt) = NULL; } \
} while (0)

/* Execute a simple SQL statement (no bindings, no result). */
#define AR_EXEC(ndb, sql) do { \
    char *_err = NULL; \
    if (sqlite3_exec((ndb)->db, sql, NULL, NULL, &_err) != SQLITE_OK) { \
        if (_err) { \
            snprintf((ndb)->last_error, sizeof((ndb)->last_error), \
                     "exec failed: %s", _err); \
            sqlite3_free(_err); \
        } \
    } \
} while (0)

/* ── Validate + Save lifecycle macro ──────────────────────────── *
 * Standard Rails-like lifecycle: validate → before_save → SQL → after_save.
 * Use in _save() implementations to eliminate boilerplate.
 *
 * Usage:
 *   AR_VALIDATE_AND_SAVE(ndb, record, model_name, validate_fn, sql_fn)
 */
#define AR_LOG_VALIDATION_FAILURE(model, errors) do { \
    char _msgs[512]; \
    ar_errors_full_messages(errors, _msgs, sizeof(_msgs)); \
    fprintf(stderr, "%s validation FAILED: %s\n", model, _msgs); \
} while (0)

/* ── Callback System ───────────────────────────────────────────── */

/* Callback signature: returns false to halt the operation.
 * before_save returning false prevents the save.
 * before_destroy returning false prevents the delete. */
typedef bool (*ar_before_cb)(void *record, void *ctx);
typedef void (*ar_after_cb)(void *record, void *ctx);

/* Async callback — queued for background execution.
 * Does not block the save/destroy operation. */
typedef void (*ar_async_cb)(void *record_copy, size_t record_size, void *ctx);

/* Per-model callback registry.
 * Each model type that wants callbacks declares a static instance. */
struct ar_callbacks {
    ar_before_cb before_validate[AR_MAX_CALLBACKS];
    ar_before_cb before_save[AR_MAX_CALLBACKS];
    ar_after_cb  after_save[AR_MAX_CALLBACKS];
    ar_before_cb before_destroy[AR_MAX_CALLBACKS];
    ar_after_cb  after_destroy[AR_MAX_CALLBACKS];
    ar_after_cb  after_validate[AR_MAX_CALLBACKS];
    ar_async_cb  after_save_async[AR_MAX_CALLBACKS];
    ar_async_cb  after_destroy_async[AR_MAX_CALLBACKS];
    int n_before_validate;
    int n_after_validate;
    int n_before_save;
    int n_after_save;
    int n_before_destroy;
    int n_after_destroy;
    int n_after_save_async;
    int n_after_destroy_async;
    size_t record_size;  /* size of the record struct for async copy */
    void *ctx;
};

static inline void ar_callbacks_init(struct ar_callbacks *cb)
{
    memset(cb, 0, sizeof(*cb));
}

static inline void ar_callbacks_set_ctx(struct ar_callbacks *cb, void *ctx)
{
    cb->ctx = ctx;
}

static inline bool ar_register_before_validate(struct ar_callbacks *cb,
                                                ar_before_cb fn)
{
    if (cb->n_before_validate >= AR_MAX_CALLBACKS) return false;
    cb->before_validate[cb->n_before_validate++] = fn;
    return true;
}

static inline bool ar_register_after_validate(struct ar_callbacks *cb,
                                               ar_after_cb fn)
{
    if (cb->n_after_validate >= AR_MAX_CALLBACKS) return false;
    cb->after_validate[cb->n_after_validate++] = fn;
    return true;
}

static inline bool ar_register_before_save(struct ar_callbacks *cb,
                                            ar_before_cb fn)
{
    if (cb->n_before_save >= AR_MAX_CALLBACKS) return false;
    cb->before_save[cb->n_before_save++] = fn;
    return true;
}

static inline bool ar_register_after_save(struct ar_callbacks *cb,
                                           ar_after_cb fn)
{
    if (cb->n_after_save >= AR_MAX_CALLBACKS) return false;
    cb->after_save[cb->n_after_save++] = fn;
    return true;
}

static inline bool ar_register_before_destroy(struct ar_callbacks *cb,
                                               ar_before_cb fn)
{
    if (cb->n_before_destroy >= AR_MAX_CALLBACKS) return false;
    cb->before_destroy[cb->n_before_destroy++] = fn;
    return true;
}

static inline bool ar_register_after_destroy(struct ar_callbacks *cb,
                                              ar_after_cb fn)
{
    if (cb->n_after_destroy >= AR_MAX_CALLBACKS) return false;
    cb->after_destroy[cb->n_after_destroy++] = fn;
    return true;
}

static inline bool ar_register_after_save_async(struct ar_callbacks *cb,
                                                 ar_async_cb fn)
{
    if (cb->n_after_save_async >= AR_MAX_CALLBACKS) return false;
    cb->after_save_async[cb->n_after_save_async++] = fn;
    return true;
}

static inline bool ar_register_after_destroy_async(struct ar_callbacks *cb,
                                                    ar_async_cb fn)
{
    if (cb->n_after_destroy_async >= AR_MAX_CALLBACKS) return false;
    cb->after_destroy_async[cb->n_after_destroy_async++] = fn;
    return true;
}

static inline void ar_set_record_size(struct ar_callbacks *cb, size_t sz)
{
    cb->record_size = sz;
}

/* Run callbacks — return false if any before_ callback returns false */

static inline bool ar_run_before_validate(struct ar_callbacks *cb, void *record)
{
    for (int i = 0; i < cb->n_before_validate; i++)
        if (!cb->before_validate[i](record, cb->ctx)) return false;
    return true;
}

static inline void ar_run_after_validate(struct ar_callbacks *cb, void *record)
{
    for (int i = 0; i < cb->n_after_validate; i++)
        cb->after_validate[i](record, cb->ctx);
}

static inline bool ar_run_before_save(struct ar_callbacks *cb, void *record)
{
    for (int i = 0; i < cb->n_before_save; i++)
        if (!cb->before_save[i](record, cb->ctx)) return false;
    return true;
}

static inline void ar_run_after_save(struct ar_callbacks *cb, void *record)
{
    for (int i = 0; i < cb->n_after_save; i++)
        cb->after_save[i](record, cb->ctx);
}

static inline bool ar_run_before_destroy(struct ar_callbacks *cb, void *record)
{
    for (int i = 0; i < cb->n_before_destroy; i++)
        if (!cb->before_destroy[i](record, cb->ctx)) return false;
    return true;
}

static inline void ar_run_after_destroy(struct ar_callbacks *cb, void *record)
{
    for (int i = 0; i < cb->n_after_destroy; i++)
        cb->after_destroy[i](record, cb->ctx);
}

/* Run async callbacks — copies record and dispatches.
 * In the current implementation, runs synchronously (inline).
 * A future thread-pool dispatch would replace the loop body. */
static inline void ar_run_after_save_async(struct ar_callbacks *cb, void *record)
{
    if (cb->n_after_save_async == 0) return;
    for (int i = 0; i < cb->n_after_save_async; i++)
        cb->after_save_async[i](record, cb->record_size, cb->ctx);
}

static inline void ar_run_after_destroy_async(struct ar_callbacks *cb, void *record)
{
    if (cb->n_after_destroy_async == 0) return;
    for (int i = 0; i < cb->n_after_destroy_async; i++)
        cb->after_destroy_async[i](record, cb->record_size, cb->ctx);
}

/* ── Relationship Macros ───────────────────────────────────────── */

/* These document model relationships. Actual query functions are
 * declared in the respective model headers. */

/* has_many: parent model has multiple child records.
 * Convention: db_<parent>_<children>(ndb, pk, *out, max) → count */

/* belongs_to: child model references a parent by foreign key.
 * Convention: db_<child>_<parent>(ndb, fk, *out) → bool */

/* ── Router ────────────────────────────────────────────────────── */

/* RPC route with before_action filters.
 * A route maps method name → handler, with optional filters. */
#define AR_MAX_FILTERS 4

struct ar_route;

typedef bool (*ar_filter_fn)(const char *method, void *ctx);

struct ar_route {
    const char *method;
    const char *category;
    bool (*handler)(const void *params, bool help, void *result);
    ar_filter_fn before_filters[AR_MAX_FILTERS];
    int n_filters;
};

struct ar_router {
    struct ar_route routes[256];
    size_t num_routes;
    ar_filter_fn global_filters[AR_MAX_FILTERS];
    int n_global_filters;
};

static inline void ar_router_init(struct ar_router *r)
{
    memset(r, 0, sizeof(*r));
}

static inline bool ar_router_add_filter(struct ar_router *r,
                                          ar_filter_fn fn)
{
    if (r->n_global_filters >= AR_MAX_FILTERS) return false;
    r->global_filters[r->n_global_filters++] = fn;
    return true;
}

static inline bool ar_router_add_route(struct ar_router *r,
                                        const char *method,
                                        const char *category,
                                        bool (*handler)(const void *, bool, void *))
{
    if (r->num_routes >= 256) return false;
    struct ar_route *route = &r->routes[r->num_routes++];
    route->method = method;
    route->category = category;
    route->handler = handler;
    route->n_filters = 0;
    return true;
}

static inline bool ar_route_add_filter(struct ar_router *r,
                                        const char *method,
                                        ar_filter_fn fn)
{
    for (size_t i = 0; i < r->num_routes; i++) {
        if (strcmp(r->routes[i].method, method) == 0) {
            struct ar_route *route = &r->routes[i];
            if (route->n_filters >= AR_MAX_FILTERS) return false;
            route->before_filters[route->n_filters++] = fn;
            return true;
        }
    }
    return false;
}

static inline const struct ar_route *ar_router_find(const struct ar_router *r,
                                                      const char *method)
{
    for (size_t i = 0; i < r->num_routes; i++)
        if (strcmp(r->routes[i].method, method) == 0)
            return &r->routes[i];
    return NULL;
}

/* Dispatch: run global filters → route filters → handler */
static inline bool ar_router_dispatch(struct ar_router *r,
                                       const char *method,
                                       const void *params,
                                       bool help,
                                       void *result,
                                       void *filter_ctx)
{
    const struct ar_route *route = ar_router_find(r, method);
    if (!route) return false;

    for (int i = 0; i < r->n_global_filters; i++)
        if (!r->global_filters[i](method, filter_ctx)) return false;

    for (int i = 0; i < route->n_filters; i++)
        if (!route->before_filters[i](method, filter_ctx)) return false;

    return route->handler(params, help, result);
}

#endif
