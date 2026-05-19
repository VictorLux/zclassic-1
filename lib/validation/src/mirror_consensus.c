/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "validation/mirror_consensus.h"

#include "event/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define MIRROR_AUTH_CAP 65536

struct mirror_auth_entry {
    bool valid;
    int height;
    struct uint256 hash;
};

static struct {
    pthread_mutex_t lock;
    struct mirror_auth_entry entries[MIRROR_AUTH_CAP];
    _Atomic int enabled;
    _Atomic int active_scopes;
    _Atomic int64_t overrides_total;
    _Atomic int64_t unsafe_overrides_total;
    _Atomic int64_t blockers_total;
    _Atomic int last_override_height;
    _Atomic int last_override_safe;
    char last_override_reason[128];
    char last_override_scope[32];
    char activation_blocker[128];
} g_mirror_consensus = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static _Thread_local int tls_mirror_scope_depth;

void mirror_consensus_set_enabled(bool enabled)
{
    atomic_store(&g_mirror_consensus.enabled, enabled ? 1 : 0);
}

static size_t auth_slot(int height)
{
    unsigned int h = height < 0 ? 0u : (unsigned int)height;
    return (size_t)(h % MIRROR_AUTH_CAP);
}

bool mirror_consensus_authorize_block(int height, const struct uint256 *hash)
{
    if (height < 0 || !hash)
        return false;
    pthread_mutex_lock(&g_mirror_consensus.lock);
    struct mirror_auth_entry *e = &g_mirror_consensus.entries[auth_slot(height)];
    e->valid = true;
    e->height = height;
    e->hash = *hash;
    pthread_mutex_unlock(&g_mirror_consensus.lock);
    return true;
}

bool mirror_consensus_is_authorized(int height, const struct uint256 *hash)
{
    if (height < 0 || !hash || !atomic_load(&g_mirror_consensus.enabled))
        return false;
    bool ok = false;
    pthread_mutex_lock(&g_mirror_consensus.lock);
    const struct mirror_auth_entry *e =
        &g_mirror_consensus.entries[auth_slot(height)];
    ok = e->valid && e->height == height && uint256_eq(&e->hash, hash);
    pthread_mutex_unlock(&g_mirror_consensus.lock);
    return ok;
}

void mirror_consensus_scope_enter(void)
{
    tls_mirror_scope_depth++;
    atomic_fetch_add(&g_mirror_consensus.active_scopes, 1);
}

void mirror_consensus_scope_leave(void)
{
    if (tls_mirror_scope_depth <= 0)
        return;
    tls_mirror_scope_depth--;
    int scopes = atomic_load(&g_mirror_consensus.active_scopes);
    while (scopes > 0 &&
           !atomic_compare_exchange_weak(&g_mirror_consensus.active_scopes,
                                         &scopes, scopes - 1)) {
    }
}

bool mirror_consensus_scope_active(void)
{
    return tls_mirror_scope_depth > 0;
}

bool mirror_consensus_authorized_current(int height,
                                         const struct uint256 *hash)
{
    return mirror_consensus_scope_active() &&
           mirror_consensus_is_authorized(height, hash);
}

void mirror_consensus_record_override(int height, const char *reason)
{
    const char *r = (reason && reason[0]) ? reason : "local_consensus_overridden";
    bool safe = mirror_consensus_scope_active() &&
                atomic_load(&g_mirror_consensus.enabled) != 0;
    int64_t overrides =
        atomic_fetch_add(&g_mirror_consensus.overrides_total, 1) + 1;
    if (!safe)
        atomic_fetch_add(&g_mirror_consensus.unsafe_overrides_total, 1);
    atomic_store(&g_mirror_consensus.last_override_height, height);
    atomic_store(&g_mirror_consensus.last_override_safe, safe ? 1 : 0);
    pthread_mutex_lock(&g_mirror_consensus.lock);
    snprintf(g_mirror_consensus.last_override_reason,
             sizeof(g_mirror_consensus.last_override_reason), "%s", r);
    snprintf(g_mirror_consensus.last_override_scope,
             sizeof(g_mirror_consensus.last_override_scope), "%s",
             safe ? "authorized_mirror_scope" : "unsafe_no_authorized_scope");
    g_mirror_consensus.activation_blocker[0] = '\0';
    pthread_mutex_unlock(&g_mirror_consensus.lock);
    event_emitf(EV_BLOCK_CHECK_PASSED, 0,
                "mirror_consensus_override h=%d reason=%s", height, r);
    event_emitf(EV_MIRROR_CONSENSUS_DECISION, 0,
                "op=override authority=local_consensus_validation "
                "trust=bounded_advisory_fallback allowed=true safe=%s h=%d "
                "reason=%s overrides=%lld unsafe=%lld blockers=%lld blk=-",
                safe ? "true" : "false",
                height, r,
                (long long)overrides,
                (long long)atomic_load(
                    &g_mirror_consensus.unsafe_overrides_total),
                (long long)atomic_load(&g_mirror_consensus.blockers_total));
    fprintf(stderr,
            "[mirror_consensus] override h=%d safe=%s reason=%s\n",
            height, safe ? "true" : "false", r);
}

void mirror_consensus_record_blocker(const char *reason)
{
    const char *r = reason ? reason : "";
    int64_t blockers =
        atomic_fetch_add(&g_mirror_consensus.blockers_total, 1) + 1;
    pthread_mutex_lock(&g_mirror_consensus.lock);
    snprintf(g_mirror_consensus.activation_blocker,
             sizeof(g_mirror_consensus.activation_blocker), "%s",
             r);
    pthread_mutex_unlock(&g_mirror_consensus.lock);
    event_emitf(EV_MIRROR_CONSENSUS_DECISION, 0,
                "op=blocker authority=local_consensus_validation "
                "trust=bounded_advisory_fallback allowed=false "
                "reason=%s blockers=%lld blk=%s",
                r, (long long)blockers, r[0] ? r : "-");
}

void mirror_consensus_clear_blocker(void)
{
    pthread_mutex_lock(&g_mirror_consensus.lock);
    g_mirror_consensus.activation_blocker[0] = '\0';
    pthread_mutex_unlock(&g_mirror_consensus.lock);
}

void mirror_consensus_stats_snapshot(struct mirror_consensus_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enabled = atomic_load(&g_mirror_consensus.enabled) != 0;
    out->override_active = atomic_load(&g_mirror_consensus.active_scopes) > 0;
    out->last_override_safe =
        atomic_load(&g_mirror_consensus.last_override_safe) != 0;
    out->overrides_total = atomic_load(&g_mirror_consensus.overrides_total);
    out->unsafe_overrides_total =
        atomic_load(&g_mirror_consensus.unsafe_overrides_total);
    out->blockers_total = atomic_load(&g_mirror_consensus.blockers_total);
    out->last_override_height =
        atomic_load(&g_mirror_consensus.last_override_height);
    pthread_mutex_lock(&g_mirror_consensus.lock);
    snprintf(out->last_override_reason, sizeof(out->last_override_reason),
             "%s", g_mirror_consensus.last_override_reason);
    snprintf(out->last_override_scope, sizeof(out->last_override_scope),
             "%s", g_mirror_consensus.last_override_scope);
    snprintf(out->activation_blocker, sizeof(out->activation_blocker),
             "%s", g_mirror_consensus.activation_blocker);
    pthread_mutex_unlock(&g_mirror_consensus.lock);
}

void mirror_consensus_reset_for_test(void)
{
    pthread_mutex_lock(&g_mirror_consensus.lock);
    memset(g_mirror_consensus.entries, 0, sizeof(g_mirror_consensus.entries));
    g_mirror_consensus.last_override_reason[0] = '\0';
    g_mirror_consensus.last_override_scope[0] = '\0';
    g_mirror_consensus.activation_blocker[0] = '\0';
    pthread_mutex_unlock(&g_mirror_consensus.lock);
    atomic_store(&g_mirror_consensus.enabled, 0);
    atomic_store(&g_mirror_consensus.active_scopes, 0);
    tls_mirror_scope_depth = 0;
    atomic_store(&g_mirror_consensus.overrides_total, 0);
    atomic_store(&g_mirror_consensus.unsafe_overrides_total, 0);
    atomic_store(&g_mirror_consensus.blockers_total, 0);
    atomic_store(&g_mirror_consensus.last_override_height, 0);
    atomic_store(&g_mirror_consensus.last_override_safe, 0);
}
