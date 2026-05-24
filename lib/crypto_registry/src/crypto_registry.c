/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "crypto_registry/crypto_registry.h"
#include "json/json.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic(const struct crypto_scheme *) g_schemes[CRYPTO_SCHEME_MAX];
static atomic_size_t g_count;
static atomic_size_t g_count_hash;
static atomic_size_t g_count_sig;
static atomic_size_t g_count_zk;

static atomic_size_t *counter_for_kind(enum crypto_scheme_kind kind)
{
    switch (kind) {
    case CRYPTO_KIND_HASH: return &g_count_hash;
    case CRYPTO_KIND_SIG:  return &g_count_sig;
    case CRYPTO_KIND_ZK:   return &g_count_zk;
    default:               return NULL;
    }
}

bool crypto_registry_register(const struct crypto_scheme *scheme)
{
    if (!scheme || scheme->id <= 0 || scheme->id >= CRYPTO_SCHEME_MAX ||
        !scheme->name || !scheme->impl)
        return false;

    atomic_size_t *kind_counter = counter_for_kind(scheme->kind);
    if (!kind_counter)
        return false;

    const struct crypto_scheme *expected = NULL;
    if (!atomic_compare_exchange_strong(&g_schemes[scheme->id],
                                        &expected, scheme))
        return false;

    atomic_fetch_add(&g_count, 1);
    atomic_fetch_add(kind_counter, 1);
    return true;
}

const struct crypto_scheme *crypto_registry_lookup(enum crypto_scheme_id id)
{
    if (id <= 0 || id >= CRYPTO_SCHEME_MAX)
        return NULL;
    return atomic_load(&g_schemes[id]);
}

bool crypto_registry_is_usable(enum crypto_scheme_id id)
{
    const struct crypto_scheme *scheme = crypto_registry_lookup(id);
    if (!scheme)
        return false;
    return scheme->status == CRYPTO_STATUS_ACTIVE ||
           scheme->status == CRYPTO_STATUS_DEPRECATED;
}

size_t crypto_registry_count(void)
{
    return atomic_load(&g_count);
}

size_t crypto_registry_count_by_kind(enum crypto_scheme_kind kind)
{
    atomic_size_t *counter = counter_for_kind(kind);
    return counter ? atomic_load(counter) : 0;
}

static const char *kind_name(enum crypto_scheme_kind kind)
{
    switch (kind) {
    case CRYPTO_KIND_HASH: return "hash";
    case CRYPTO_KIND_SIG:  return "sig";
    case CRYPTO_KIND_ZK:   return "zk";
    default:               return "unknown";
    }
}

static const char *status_name(enum crypto_scheme_status status)
{
    switch (status) {
    case CRYPTO_STATUS_ACTIVE:       return "active";
    case CRYPTO_STATUS_DEPRECATED:   return "deprecated";
    case CRYPTO_STATUS_RETIRED:      return "retired";
    case CRYPTO_STATUS_UNREGISTERED: return "unregistered";
    default:                         return "unknown";
    }
}

bool crypto_registry_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;

    json_set_object(out);
    json_push_kv_int(out, "total_registered",
                     (int64_t)crypto_registry_count());

    struct json_value by_kind;
    json_init(&by_kind);
    json_set_object(&by_kind);
    json_push_kv_int(&by_kind, "hash",
                     (int64_t)crypto_registry_count_by_kind(CRYPTO_KIND_HASH));
    json_push_kv_int(&by_kind, "sig",
                     (int64_t)crypto_registry_count_by_kind(CRYPTO_KIND_SIG));
    json_push_kv_int(&by_kind, "zk",
                     (int64_t)crypto_registry_count_by_kind(CRYPTO_KIND_ZK));
    json_push_kv_int(&by_kind, "ed25519_pending",
                     crypto_registry_lookup(CRYPTO_SIG_ED25519) ? 0 : 1);
    json_push_kv(out, "by_kind", &by_kind);

    struct json_value schemes;
    json_init(&schemes);
    json_set_array(&schemes);
    for (int i = 1; i < CRYPTO_SCHEME_MAX; i++) {
        const struct crypto_scheme *scheme = atomic_load(&g_schemes[i]);
        if (!scheme)
            continue;

        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_int(&obj, "id", scheme->id);
        json_push_kv_str(&obj, "name", scheme->name);
        json_push_kv_str(&obj, "kind", kind_name(scheme->kind));
        json_push_kv_str(&obj, "status", status_name(scheme->status));
        json_push_kv_str(&obj, "impl", scheme->impl);
        json_push_back(&schemes, &obj);
    }
    json_push_kv(out, "schemes", &schemes);
    return true;
}
