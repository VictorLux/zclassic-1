/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * RPC commands for the event log and sync state machine. */

#include "controllers/event_controller.h"
#include "controllers/strong_params.h"
#include "event/event.h"
#include "json/json.h"
#include "rpc/server.h"
#include <stdlib.h>
#include <string.h>

/* ── eventlog RPC ────────────────────────────────────────── */

static bool rpc_eventlog(const struct json_value *params, bool help,
                         struct json_value *result)
{
    RPC_HELP(help, result,
        "eventlog ( count )\n"
        "Returns the last `count` events from the event log ring buffer.\n"
        "\nArguments:\n"
        "1. count    (numeric, optional, default=200) Number of events\n"
        "\nResult: array of event objects");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    int count = 200;
    if (json_size(params) >= 1) {
        count = (int)rpc_require_int(&p, 0, "count");
        if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }
        if (count < 1) count = 1;
        if (count > 65536) count = 65536;
    }

    /* Dump events to JSON buffer */
    size_t buf_size = (size_t)count * 256 + 256;
    if (buf_size > 16 * 1024 * 1024) buf_size = 16 * 1024 * 1024;
    char *buf = malloc(buf_size);
    if (!buf) {
        json_set_str(result, "out of memory");
        return false;
    }

    size_t len = event_dump_json(buf, buf_size, (size_t)count);

    /* Parse the JSON array into the result value */
    bool ok = json_read(result, buf, len);
    free(buf);

    if (!ok) {
        json_set_str(result, "failed to parse event log");
        return false;
    }
    return true;
}

/* ── syncstate RPC ───────────────────────────────────────── */

static bool rpc_syncstate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "syncstate\n"
        "Returns the current sync state machine state.\n"
        "\nResult: string (idle, finding_peers, headers_download, "
        "blocks_download, connecting_blocks, at_tip, reorg, "
        "snapshot_receive, failed)");

    json_set_str(result, sync_state_name(sync_get_state()));
    return true;
}

/* ── Registration ────────────────────────────────────────── */

void register_event_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "eventlog",   rpc_eventlog,   true },
        { "control", "syncstate",  rpc_syncstate,  true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
