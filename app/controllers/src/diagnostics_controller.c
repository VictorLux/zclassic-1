/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics controller — the routing glue for the read-only
 * introspection RPC family used by AI agents and power-dev users.
 *
 * The concern was split out of one ~2,550-LOC mega-module into focused,
 * single-responsibility controller files; this file is now just the
 * dispatch table that wires their RPC handlers into the rpc_table:
 *
 *   diagnostics_registry.c          dumpstate / zcl_state + g_dumpers[]
 *                                   + controller-level state ownership
 *   cutover_controller.c            cutovermode / cutoverpreflight
 *   nodelog_controller.c            getnodelog
 *   dbquery_controller.c            dbquery
 *   probe_controller.c              probezclassicd
 *   projection_diff_controller.c    getmirrorstatus + *projectiondiff
 *
 * The public state-wiring + subsystem-CSV API (diagnostics_controller.h)
 * is implemented in diagnostics_registry.c, which owns g_diag. */

#include "controllers/diagnostics_controller.h"
#include "controllers/diagnostics_internal.h"

#include "rpc/server.h"

/* ── Registration ────────────────────────────────────────────────── */

void register_diagnostics_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "dumpstate",     diag_rpc_dumpstate,     true },
        { "control", "cutovermode",   diag_rpc_cutovermode,   true },
        { "control", "cutoverpreflight", diag_rpc_cutoverpreflight, true },
        { "control", "getnodelog",    diag_rpc_getnodelog,    true },
        { "control", "dbquery",       diag_rpc_dbquery,       true },
        { "control", "probezclassicd", diag_rpc_probezclassicd, true },
        { "control", "getmirrorstatus", diag_rpc_getmirrorstatus, true },
        { "control", "peersprojectiondiff", diag_rpc_peersprojectiondiff, true },
        { "control", "mempoolprojectiondiff", diag_rpc_mempoolprojectiondiff, true },
        { "control", "znamprojectiondiff",  diag_rpc_znamprojectiondiff,  true },
        { "control", "walletprojectiondiff", diag_rpc_walletprojectiondiff, true },
        { "control", "contactsprojectiondiff",
          diag_rpc_contactsprojectiondiff, true },
        { "control", "onionannouncementsprojectiondiff",
          diag_rpc_onionannouncementsprojectiondiff, true },
        { "control", "hodlhistoryprojectiondiff",
          diag_rpc_hodlhistoryprojectiondiff, true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
