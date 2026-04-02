/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Event log and sync state machine RPCs.
 * Canonical source for: eventlog, syncstate */

#include "controllers/event_controller.h"
#include "controllers/strong_params.h"
#include "config/boot.h"
#include "services/node_health_service.h"
#include "event/event.h"
#include "json/json.h"
#include "rpc/server.h"
#include <stdlib.h>
#include <string.h>

static bool rpc_eventlog(const struct json_value *params, bool help,
                         struct json_value *result)
{
    RPC_HELP(help, result,
        "eventlog ( count )\n"
        "\nReturn recent events from the system event log.\n"
        "Every P2P message, state transition, block validation,\n"
        "and error is captured in a lock-free ring buffer.\n"
        "\nArguments:\n"
        "1. count     (numeric, optional, default=200) Number of events\n"
        "\nResult:\n"
        "  { \"sync_state\": \"...\", \"events\": [...] }\n");

    int count = 200;
    if (params && params->type == JSON_ARR && params->num_children > 0) {
        const struct json_value *v = &params->children[0];
        if (v->type == JSON_INT) count = (int)v->val.i;
        else if (v->type == JSON_REAL) count = (int)v->val.d;
    }
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;

    size_t buf_size = (size_t)count * 256 + 256;
    if (buf_size > 16 * 1024 * 1024) buf_size = 16 * 1024 * 1024;
    char *buf = malloc(buf_size);
    if (!buf) {
        json_set_str(result, "out of memory");
        return false;
    }

    size_t w = 0;
    w += (size_t)snprintf(buf + w, 256, "{\"sync_state\":\"%s\",\"events\":",
                           sync_state_name(sync_get_state()));
    w += event_dump_json(buf + w, buf_size - w, (size_t)count);
    if (w + 1 < buf_size) buf[w++] = '}';
    buf[w] = '\0';

    json_read(result, buf, w);
    free(buf);
    return true;
}

static bool rpc_syncstate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "syncstate\n"
        "\nReturn the current sync state machine state.\n"
        "\nResult:\n"
        "  { \"state\": \"...\", \"state_id\": N }\n");

    json_set_object(result);
    json_push_kv_str(result, "state", sync_state_name(sync_get_state()));
    json_push_kv_int(result, "state_id", (int64_t)sync_get_state());
    json_push_kv_bool(result, "utxo_replay_active",
                      atomic_load(&g_utxo_replay_active));
    json_push_kv_int(result, "utxo_replay_height",
                     (int64_t)atomic_load(&g_utxo_replay_height));
    return true;
}

static bool rpc_healthcheck(const struct json_value *params, bool help,
                             struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "healthcheck\n"
        "\nReturn node health status — single pass/fail for monitoring.\n"
        "\nResult:\n"
        "  { \"healthy\": true/false, \"sync_state\": \"...\",\n"
        "    \"checks\": { ... } }\n");

    json_set_object(result);

    struct node_health_snapshot health;
    node_health_collect(&health, NULL);
    json_push_kv_str(result, "sync_state", sync_state_name(health.sync_state));

    /* Individual health checks */
    struct json_value checks = {0};
    json_set_object(&checks);

    json_push_kv_bool(&checks, "synced", health.synced);
    json_push_kv_bool(&checks, "has_peers", health.has_peers);
    json_push_kv_int(&checks, "peer_count", (int64_t)health.peer_count);

    json_push_kv_bool(result, "healthy", health.healthy);
    json_push_kv(result, "checks", &checks);
    json_free(&checks);

    return true;
}

void register_event_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "eventlog",     rpc_eventlog,     true },
        { "control", "syncstate",    rpc_syncstate,    true },
        { "control", "healthcheck",  rpc_healthcheck,  true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
