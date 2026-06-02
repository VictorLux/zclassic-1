/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "util/service_state.h"

#include "util/log_macros.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic int g_service_state = SERVICE_STATE_BOOT;
static pthread_mutex_t g_reason_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_reason[128] = "boot";

static const char *const k_names[SERVICE_STATE__COUNT] = {
    [SERVICE_STATE_BOOT]             = "boot",
    [SERVICE_STATE_RESTORE]          = "restore",
    [SERVICE_STATE_RECONCILE]        = "reconcile",
    [SERVICE_STATE_DEGRADED_SERVING] = "degraded_serving",
    [SERVICE_STATE_SYNCING]          = "syncing",
    [SERVICE_STATE_HEALTHY]          = "healthy",
    [SERVICE_STATE_REPAIRING]        = "repairing",
};

const char *service_state_name(enum service_state s)
{
    if ((int)s < 0 || (int)s >= SERVICE_STATE__COUNT || !k_names[s])
        return "unknown";
    return k_names[s];
}

enum service_state service_state_current(void)
{
    return (enum service_state)atomic_load(&g_service_state);
}

const char *service_state_reason(void)
{
    return g_reason;
}

void service_state_advance(enum service_state next, const char *reason)
{
    if ((int)next < 0 || (int)next >= SERVICE_STATE__COUNT) {
        LOG_WARN("service_state",
                 "[service_state] ignoring out-of-range target %d",
                 (int)next);
        return;
    }

    enum service_state prev =
        (enum service_state)atomic_exchange(&g_service_state, (int)next);

    pthread_mutex_lock(&g_reason_lock);
    if (reason && reason[0]) {
        strncpy(g_reason, reason, sizeof(g_reason) - 1);
        g_reason[sizeof(g_reason) - 1] = '\0';
    }
    pthread_mutex_unlock(&g_reason_lock);

    if (prev != next)
        LOG_INFO("service_state", "[service_state] %s -> %s (%s)",
                 service_state_name(prev), service_state_name(next),
                 reason && reason[0] ? reason : "-");
}
