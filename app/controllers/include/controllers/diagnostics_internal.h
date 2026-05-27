/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal seam shared across the diagnostics controller family. The
 * diagnostics concern was split out of one mega-module into focused,
 * single-responsibility controller files (registry, cutover, nodelog,
 * dbquery, probe, projection-diff). They share two things:
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

/* The native chain-evidence dump, registered in g_dumpers and also
 * consumed directly by the cutover preflight snapshot. `out` must be a
 * fresh json_value; the function sets it to an object. */
bool diag_chain_evidence_dump_state_json(struct json_value *out,
                                         const char *key);

/* cutover_controller.c */
bool diag_rpc_cutovermode(const struct json_value *params, bool help,
                          struct json_value *result);
/* cutover_controller_preflight.c — split out of cutover_controller.c to
 * keep each file under the app/ file-size ceiling. */
bool diag_rpc_cutoverpreflight(const struct json_value *params, bool help,
                               struct json_value *result);

/* Canary-state snapshot, shared between cutover_controller.c (the
 * cutovermode RPC) and cutover_controller_preflight.c. Defined in
 * cutover_controller.c. `out` is set to an object; `health` may be NULL. */
struct node_health_snapshot;
void cutover_push_canary_state(struct json_value *out,
                               const struct node_health_snapshot *health);

/* nodelog_controller.c */
bool diag_rpc_getnodelog(const struct json_value *params, bool help,
                         struct json_value *result);

/* dbquery_controller.c */
bool diag_rpc_dbquery(const struct json_value *params, bool help,
                      struct json_value *result);

/* probe_controller.c */
bool diag_rpc_probezclassicd(const struct json_value *params, bool help,
                             struct json_value *result);

/* projection_diff_controller.c */
bool diag_rpc_getmirrorstatus(const struct json_value *params, bool help,
                              struct json_value *result);
bool diag_rpc_peersprojectiondiff(const struct json_value *params, bool help,
                                  struct json_value *result);
bool diag_rpc_mempoolprojectiondiff(const struct json_value *params, bool help,
                                    struct json_value *result);
bool diag_rpc_znamprojectiondiff(const struct json_value *params, bool help,
                                 struct json_value *result);
bool diag_rpc_walletprojectiondiff(const struct json_value *params, bool help,
                                   struct json_value *result);
bool diag_rpc_contactsprojectiondiff(const struct json_value *params,
                                     bool help, struct json_value *result);
bool diag_rpc_onionannouncementsprojectiondiff(const struct json_value *params,
                                               bool help,
                                               struct json_value *result);
bool diag_rpc_hodlhistoryprojectiondiff(const struct json_value *params,
                                        bool help, struct json_value *result);

#endif /* ZCL_DIAGNOSTICS_INTERNAL_H */
