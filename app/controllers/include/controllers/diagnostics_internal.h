/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal seam shared across the diagnostics controller family. The
 * diagnostics concern was split out of one mega-module into focused,
 * single-responsibility controller files (registry, nodelog, dbquery, probe).
 * They share two things:
 *
 *   - the controller-level state (`main_state` + `datadir`), owned by
 *     diagnostics_registry.c and reachable here via accessors;
 *   - each file's RPC handler prototypes, so the routing table in
 *     diagnostics_controller.c can register them.
 *
 * This header is internal to app/controllers; it is not part of the
 * public diagnostics_controller.h API. */

#ifndef ZCL_DIAGNOSTICS_INTERNAL_H
#define ZCL_DIAGNOSTICS_INTERNAL_H

#include <stdbool.h>

struct json_value;
struct main_state;

/* Wired controller-level state, owned by diagnostics_registry.c.
 * `diag_main_state()` returns NULL until set_state() runs; `diag_datadir()`
 * returns "" until then. */
struct main_state *diag_main_state(void);
const char *diag_datadir(void);

/* RPC handlers, one per concern file. Signatures match rpc_handler_fn. */

/* diagnostics_registry.c */
bool diag_rpc_dumpstate(const struct json_value *params, bool help,
                        struct json_value *result);

/* The native chain-evidence dump, registered in g_dumpers. `out` must be
 * a fresh json_value; the function sets it to an object. */
bool diag_chain_evidence_dump_state_json(struct json_value *out,
                                         const char *key);

/* nodelog_controller.c */
bool diag_rpc_getnodelog(const struct json_value *params, bool help,
                         struct json_value *result);

/* dbquery_controller.c */
bool diag_rpc_dbquery(const struct json_value *params, bool help,
                      struct json_value *result);

/* probe_controller.c */
bool diag_rpc_probezclassicd(const struct json_value *params, bool help,
                             struct json_value *result);

/* projection_diff_controller.c — getmirrorstatus is a keeper (legacy
 * mirror monitor); the per-table *projectiondiff RPC surfaces are deleted
 * (stage 4). The implementing .c file is removed in a later stage. */
bool diag_rpc_getmirrorstatus(const struct json_value *params, bool help,
                              struct json_value *result);

#endif /* ZCL_DIAGNOSTICS_INTERNAL_H */
