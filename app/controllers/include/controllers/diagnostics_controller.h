/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics controller — hosts read-only introspection RPC methods
 * for AI agents and power-dev users:
 *
 *   dumpstate <subsystem> [key]   — generic in-process state dump,
 *                                   dispatches to <subsystem>_dump_state_json
 *   getnodelog <pattern> ...      — reverse-scan node.log with regex/level
 *   dbquery <sql> [limit]         — SELECT-only SQLite passthrough
 *
 * These primitives let an MCP client (Claude Code) inspect runtime
 * state without one dedicated tool per question. */

#ifndef ZCL_DIAGNOSTICS_CONTROLLER_H
#define ZCL_DIAGNOSTICS_CONTROLLER_H

struct rpc_table;
struct main_state;

/* Wire main_state for subsystems that look up chain state (block_index
 * dumps, lastboot, etc). Call once after main_state is initialized. */
void diagnostics_controller_set_state(struct main_state *ms,
                                      const char *datadir);

void register_diagnostics_rpc_commands(struct rpc_table *t);

#endif /* ZCL_DIAGNOSTICS_CONTROLLER_H */
