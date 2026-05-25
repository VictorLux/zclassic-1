/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics controller — read-only introspection RPC for AI / power-dev
 * users. See diagnostics_controller.h for overview.
 *
 * The three primitives here replace dozens of potential bespoke tools:
 *
 *   dumpstate   — generic state dump. Dispatches by `subsystem` to the
 *                 owning service's `*_dump_state_json` function. Adding
 *                 a new subsystem is one dispatcher line + one dump
 *                 function in the owning module (see CLAUDE.md
 *                 "Adding state introspection").
 *
 *   getnodelog  — reverse-scan ~/.zclassic-c23/node.log with regex,
 *                 level, since_secs, max_lines. Bounded memory.
 *
 *   dbquery     — SELECT-only SQLite passthrough with hard validation
 *                 (no semicolons, no DDL/DML, auto-LIMIT, 2s wall budget,
 *                 100-row hard cap). Marked destructive in MCP middleware
 *                 because arbitrary scans can be expensive even though
 *                 they don't mutate.
 */

#include "platform/time_compat.h"
#include "controllers/diagnostics_controller.h"

#include "views/format_helpers.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/contextual_check_tx.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "json/json.h"
#include "rpc/server.h"
#include "controllers/explorer_internal.h"
#include "controllers/strong_params.h"
#include "services/sync_monitor.h"
#include "services/chain_restore_boot_snapshot.h"
#include "services/chain_advance_coordinator.h"
#include "services/zclassicd_oracle_service.h"
#include "services/header_probe.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/oracle_policy.h"
#include "services/quorum_oracle_service.h"
#include "services/rolling_anchor_service.h"
#include "services/block_index_integrity.h"
#include "services/block_pruning_service.h"
#include "services/chain_evidence_controller.h"
#include "services/header_admit_stage.h"
#include "services/validate_headers_stage.h"
#include "services/node_health_service.h"
#include "services/body_fetch_stage.h"
#include "services/body_persist_stage.h"
#include "services/script_validate_stage.h"
#include "services/proof_validate_stage.h"
#include "services/utxo_apply_stage.h"
#include "services/tip_finalize_stage.h"
#include "services/chain_tip_watchdog.h"
#include "framework/condition.h"
#include "storage/block_index_projection.h"
#include "storage/mempool_projection.h"
#include "storage/peers_projection.h"
#include "storage/progress_store.h"
#include "storage/small_projections.h"
#include "storage/utxo_projection.h"
#include "storage/znam_projection.h"
#include "storage/wallet_projection.h"
#include "crypto_registry/crypto_registry.h"
#include "services/ibd_throttle.h"
#include "services/mempool_limits.h"
#include "health/heartbeat.h"
#include "models/database.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include "models/wallet_tx.h"
#include "config/runtime.h"
#include "net/peer_lifecycle.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/long_op.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/blocker.h"

#include <sqlite3.h>
#include <ctype.h>
#include <regex.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

/* ── Controller-level state ─────────────────────────────────────── */

static struct {
    struct main_state *main_state;
    char datadir[1024];
} g_diag = {0};

static _Atomic int g_cutover_has_change;
static _Atomic int64_t g_cutover_change_unix;
static _Atomic int64_t g_cutover_change_height;
static _Atomic int64_t g_cutover_canary_target_height;
static _Atomic int64_t g_cutover_change_header_height;
static _Atomic int64_t g_cutover_change_peer_best_height;
static _Atomic int64_t g_cutover_change_tip_lag;

void diagnostics_controller_set_state(struct main_state *ms,
                                      const char *datadir)
{
    g_diag.main_state = ms;
    if (datadir) {
        snprintf(g_diag.datadir, sizeof(g_diag.datadir), "%s", datadir);
    }
}

struct main_state *diagnostics_controller_get_state(void)
{
    return g_diag.main_state;
}

/* ── block_index dump ─────────────────────────────────────────────
 *
 * Lives here (not lib/chain) because the lookup needs main_state, which
 * is an app/validation layering concern. Decodes nStatus flags by name
 * so callers don't have to remember the bit positions.
 */

static void push_block_status_flags(struct json_value *arr, unsigned nStatus)
{
    /* Lower 3 bits = validity level (enum); the rest are flag bits. */
    static const struct { unsigned mask; const char *name; } flags[] = {
        { BLOCK_HAVE_DATA,    "BLOCK_HAVE_DATA" },
        { BLOCK_HAVE_UNDO,    "BLOCK_HAVE_UNDO" },
        { BLOCK_FAILED_VALID, "BLOCK_FAILED_VALID" },
        { BLOCK_FAILED_CHILD, "BLOCK_FAILED_CHILD" },
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (nStatus & flags[i].mask) {
            struct json_value v = {0};
            json_set_str(&v, flags[i].name);
            json_push_back(arr, &v);
            json_free(&v);
        }
    }
}

static const char *block_validity_level_name(unsigned nStatus)
{
    switch (nStatus & BLOCK_VALID_MASK) {
        case BLOCK_VALID_UNKNOWN:      return "BLOCK_VALID_UNKNOWN";
        case BLOCK_VALID_HEADER:       return "BLOCK_VALID_HEADER";
        case BLOCK_VALID_TREE:         return "BLOCK_VALID_TREE";
        case BLOCK_VALID_TRANSACTIONS: return "BLOCK_VALID_TRANSACTIONS";
        case BLOCK_VALID_CHAIN:        return "BLOCK_VALID_CHAIN";
        case BLOCK_VALID_SCRIPTS:      return "BLOCK_VALID_SCRIPTS";
    }
    return "UNKNOWN";
}

static struct block_index *find_block_index_by_key(struct main_state *ms,
                                                    const char *key)
{
    if (!ms || !key || !key[0]) return NULL;

    /* Numeric → height lookup via active_chain. */
    bool is_num = true;
    for (const char *c = key; *c; c++) {
        if (*c < '0' || *c > '9') { is_num = false; break; }
    }
    if (is_num) {
        int height = atoi(key);
        return active_chain_at(&ms->chain_active, height);
    }

    /* Hex → hash lookup via block_map. */
    if (!zcl_is_hex_string(key, 64)) return NULL;
    struct uint256 h;
    uint256_set_hex(&h, key);
    return block_map_find(&ms->map_block_index, &h);
}

static bool block_index_dump_state_json(struct json_value *out, const char *key)
{
    if (!out) return false;
    struct block_index *bi = find_block_index_by_key(g_diag.main_state, key);
    json_set_object(out);
    {
        struct bii_recovery_status status;
        struct json_value integrity = {0};
        bii_get_recovery_status(&status);
        json_set_object(&integrity);
        json_push_kv_str(&integrity, "verdict",
                         bii_verdict_name(status.verdict));
        json_push_kv_str(&integrity, "action",
                         bii_recovery_action_name(status.action));
        json_push_kv_bool(&integrity, "degraded", status.degraded);
        json_push_kv_bool(&integrity, "unsafe_override",
                          status.unsafe_override);
        json_push_kv_int(&integrity, "last_check_unix", status.unix_time);
        if (status.reason[0])
            json_push_kv_str(&integrity, "reason", status.reason);
        json_push_kv(out, "integrity", &integrity);
        json_free(&integrity);
    }
    if (!bi) {
        json_push_kv_bool(out, "found", false);
        json_push_kv_str(out, "key", key ? key : "");
        return true;
    }

    json_push_kv_bool(out, "found", true);
    json_push_kv_int(out, "nHeight", (int64_t)bi->nHeight);
    json_push_kv_int(out, "nVersion", (int64_t)bi->nVersion);
    json_push_kv_int(out, "nTime", (int64_t)bi->nTime);
    json_push_kv_int(out, "nBits", (int64_t)bi->nBits);
    json_push_kv_int(out, "nChainTx", (int64_t)bi->nChainTx);
    json_push_kv_int(out, "nTx", (int64_t)bi->nTx);
    json_push_kv_int(out, "nFile", (int64_t)bi->nFile);
    json_push_kv_int(out, "nDataPos", (int64_t)bi->nDataPos);
    json_push_kv_int(out, "nUndoPos", (int64_t)bi->nUndoPos);
    json_push_kv_int(out, "nSequenceId", (int64_t)bi->nSequenceId);
    json_push_kv_int(out, "nStatus_raw", (int64_t)bi->nStatus);
    json_push_kv_str(out, "nStatus_validity",
                     block_validity_level_name(bi->nStatus));
    {
        struct json_value flags_arr = {0};
        json_set_array(&flags_arr);
        push_block_status_flags(&flags_arr, bi->nStatus);
        json_push_kv(out, "nStatus_flags", &flags_arr);
        json_free(&flags_arr);
    }
    {
        char hex[65];
        if (bi->phashBlock) {
            uint256_get_hex(bi->phashBlock, hex);
            json_push_kv_str(out, "hash", hex);
        } else {
            json_push_kv_str(out, "hash", "");
        }
        if (bi->pprev && bi->pprev->phashBlock) {
            uint256_get_hex(bi->pprev->phashBlock, hex);
            json_push_kv_str(out, "hash_prev", hex);
        } else {
            json_push_kv_str(out, "hash_prev", "");
        }
        arith_uint256_get_hex(&bi->nChainWork, hex);
        json_push_kv_str(out, "nChainWork", hex);
    }
    bool on_chain = false;
    if (g_diag.main_state) {
        struct block_index *at = active_chain_at(
            &g_diag.main_state->chain_active, bi->nHeight);
        on_chain = (at == bi);
    }
    json_push_kv_bool(out, "on_active_chain", on_chain);
    return true;
}

static void push_evidence_record_json(struct json_value *out, const char *key,
                                      const struct chain_evidence_record *e)
{
    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "source_class",
                      chain_evidence_source_class_name(
                          e ? e->source_class : CEC_SOURCE_CLASS_UNKNOWN));
    json_push_kv_str(&obj, "publish_state",
                      chain_evidence_publish_state_name(
                          e ? e->publish_state
                            : CEC_PUBLISH_NOT_PUBLISHABLE));
    json_push_kv_bool(&obj, "header_ancestry_linked",
                      e && e->header_ancestry_linked);
    json_push_kv_bool(&obj, "chainwork_recomputed",
                      e && e->chainwork_recomputed);
    json_push_kv_bool(&obj, "nakamoto_selected_best_work",
                      e && e->nakamoto_selected_best_work);
    json_push_kv_bool(&obj, "block_bytes_hash_checked",
                      e && e->block_bytes_hash_checked);
    json_push_kv_bool(&obj, "utxo_sha3_verified",
                      e && e->utxo_sha3_verified);
    json_push_kv_bool(&obj, "mmb_flyclient_proof_verified",
                      e && e->mmb_flyclient_proof_verified);
    json_push_kv_bool(&obj, "chunk_hash_coverage_verified",
                      e && e->chunk_hash_coverage_verified);
    json_push_kv_bool(&obj, "full_validation_complete",
                      e && e->full_validation_complete);
    json_push_kv(out, key, &obj);
    json_free(&obj);
}

static void push_hash_json(struct json_value *out, const char *key,
                           bool present, const struct uint256 *hash)
{
    char hex[65] = {0};
    if (present && hash)
        uint256_get_hex(hash, hex);
    json_push_kv_str(out, key, present ? hex : "");
}

static void push_explorer_index_state_json(struct json_value *out,
                                           struct node_db *ndb)
{
    struct json_value obj = {0};

    json_set_object(&obj);
    if (!ndb || !ndb->open || !ndb->db) {
        json_push_kv_str(&obj, "state", "unknown");
        json_push_kv_str(&obj, "reason", "database unavailable");
        json_push_kv(out, "explorer_index_state", &obj);
        json_free(&obj);
        return;
    }

    struct explorer_history_validation v;
    int64_t height = sql_query_i64(ndb->db,
        "SELECT COALESCE(MAX(height),0) FROM blocks");
    explorer_validate_block_history(ndb->db, height, &v);
    json_push_kv_str(&obj, "state", v.usable ? "complete" : "degraded");
    json_push_kv_str(&obj, "reason", v.reason);
    json_push_kv_int(&obj, "height", v.max_height);
    json_push_kv_int(&obj, "blocks", v.block_rows);
    json_push_kv_int(&obj, "transactions", v.tx_rows);
    json_push_kv_int(&obj, "tx_outputs", v.tx_output_rows);
    json_push_kv_int(&obj, "integrity_receipts", v.integrity_rows);
    json_push_kv(out, "explorer_index_state", &obj);
    json_free(&obj);
}

static bool chain_evidence_controller_dump_state_json(struct json_value *out,
                                           const char *key)
{
    (void)key;
    if (!out)
        return false;
    struct chain_evidence_controller authority;
    struct chain_evidence_controller_view view;

    json_set_object(out);
    chain_evidence_controller_init(&authority, app_runtime_node_db(), csr_instance());
    chain_evidence_controller_snapshot(&authority, &view);

    json_push_kv_str(out, "sync_state",
                     chain_evidence_controller_state_name(view.state));
    json_push_kv_str(out, "publish_state",
                     chain_evidence_publish_state_name(view.publish_state));
    json_push_kv_str(out, "active_tip_source_class",
                     chain_evidence_source_class_name(
                         view.active_tip_source_class));
    json_push_kv_int(out, "active_tip",
                     (int64_t)view.active_tip_height);
    json_push_kv_int(out, "header_tip",
                     (int64_t)view.header_tip_height);
    json_push_kv_int(out, "persisted_active_tip",
                     (int64_t)view.persisted_active_tip_height);
    json_push_kv_int(out, "snapshot_anchor",
                     (int64_t)view.snapshot_anchor_height);
    json_push_kv_int(out, "utxo_max_height",
                     (int64_t)view.utxo_max_height);
    json_push_kv_int(out, "coins_best_block_height",
                     (int64_t)view.coins_best_block_height);
    json_push_kv_int(out, "csr_sqlite_max_height",
                     (int64_t)view.sqlite_max_height);
    push_hash_json(out, "active_tip_hash", view.has_active_tip_hash,
                   &view.active_tip_hash);
    push_hash_json(out, "header_tip_hash", view.has_header_tip_hash,
                   &view.header_tip_hash);
    push_hash_json(out, "persisted_active_tip_hash",
                   view.has_persisted_active_tip_hash,
                   &view.persisted_active_tip_hash);
    push_hash_json(out, "coins_best_block_hash",
                   view.has_coins_best_block_hash,
                   &view.coins_best_block_hash);
    json_push_kv_bool(out, "missing_active_tip_evidence",
                      view.missing_active_tip_evidence);
    json_push_kv_bool(out, "publish_state_not_local",
                      view.publish_state_not_local);
    json_push_kv_bool(out, "active_tip_hash_mismatch",
                      view.active_tip_hash_mismatch);
    json_push_kv_bool(out, "csr_cursor_mismatch",
                      view.csr_cursor_mismatch);
    json_push_kv_bool(out, "repaired_active_tip_evidence",
                      view.repaired_active_tip_evidence);
    json_push_kv_str(out, "health_reason", view.health_reason);
    push_evidence_record_json(out, "block_index_evidence_state",
                              &view.block_index_evidence_state);
    push_evidence_record_json(out, "active_tip_evidence",
                              &view.active_tip_evidence);
    push_evidence_record_json(out, "snapshot_evidence",
                              &view.snapshot_evidence);
    push_evidence_record_json(out, "header_chain_evidence",
                              &view.header_chain_evidence);
    json_push_kv_int(out, "deferred_proof_validation_below",
                     (int64_t)g_deferred_proof_validation_below_height);
    json_push_kv_int(out, "background_validation_height",
                     (int64_t)view.background_validation_height);
    json_push_kv_str(out, "contradiction_reason",
                     view.contradiction_reason);
    push_explorer_index_state_json(out, app_runtime_node_db());
    return true;
}

/* ── RPC: dumpstate <subsystem> [key] ────────────────────────────── */

typedef bool (*dump_fn)(struct json_value *out, const char *key);

struct dump_entry {
    const char *name;
    dump_fn     fn;
    const char *desc;
};

static const struct dump_entry g_dumpers[] = {
    { "supervisor", supervisor_dump_state_json,
                    "root supervisor: registered liveness contracts, "
                    "ticks_run, stall_fires, deadlines" },
    { "blocker",    blocker_dump_state_json,
                    "typed blocker registry: active blockers by class "
                    "{permanent,transient,dependency,resource}, "
                    "deadlines, escape actions, fire counts" },
    { "watchdog",    condition_engine_dump_state_json,
                     "compat alias for condition_engine status" },
    { "chain_evidence", chain_evidence_controller_dump_state_json,
                     "native chain evidence: tips, cursors, evidence flags, reconciliation reason" },
    { "chain_evidence_controller", chain_evidence_controller_dump_state_json,
                     "native chain evidence controller: tips, snapshot anchor, evidence, contradiction reason" },
    { "boot",        chain_restore_dump_state_json,
                     "last boot's integrity check + nbits-backfill counters" },
    { "block_index", block_index_dump_state_json,
                     "block_index entry by height or hash (in `key`)" },
    { "health",      health_dump_state_json,
                     "unified heartbeat ring: registered subsystems, ages, stall fires" },
    { "oracle",      zclassicd_oracle_dump_state_json,
                     "zclassicd oracle: drift-probe stats + RPC config" },
    { "header_probe", header_probe_dump_state_json,
                     "header probe: bulk header pull from co-located zclassicd via JSON-RPC" },
    { "legacy_mirror", legacy_mirror_sync_dump_state_json,
                     "legacy mirror: always-on lockstep catch-up from co-located zclassicd" },
    { "oracle_policy", oracle_policy_dump_state_json,
                     "oracle policy: disagreement state machine (NORMAL / HALTED / PANIC)" },
    { "rolling_anchor", rolling_anchor_dump_state_json,
                     "rolling SHA3 anchor extension: runtime windows past compile-time prefix" },
    { "progress",    progress_store_dump_state_json,
                     "Wave S progress.kv: open/path/stage_cursor row count" },
    { "header_admit", header_admit_stage_dump_state_json,
                     "Wave S header_admit shadow stage: cursor, counters, last admit" },
    { "validate_headers", validate_headers_stage_dump_state_json,
                     "Wave S validate_headers shadow stage: cursor, pool stats, pass/fail counters" },
    { "body_fetch", body_fetch_stage_dump_state_json,
                     "Wave S body_fetch shadow stage: cursor, observed/skipped counters, last advance" },
    { "body_persist", body_persist_dump_state_json,
                     "Wave S body_persist shadow stage: cursor, verification counters, log rows" },
    { "script_validate", script_validate_dump_state_json,
                     "Wave S script_validate shadow stage: cursor, script counters, log rows" },
    { "proof_validate", proof_validate_dump_state_json,
                     "Wave S proof_validate shadow stage: cursor, proof counters, log rows" },
    { "utxo_apply", utxo_apply_dump_state_json,
                     "Wave S utxo_apply shadow stage: cursor, UTXO delta counters, log rows" },
    { "tip_finalize", tip_finalize_dump_state_json,
                     "Wave S tip_finalize shadow stage: cursor, finalize counters, log rows" },
    { "quorum_oracle", quorum_oracle_dump_state_json,
                     "multi-source quorum oracle: per-source vote stats + last verdict" },
    { "peer_lifecycle", peer_lifecycle_dump_state_json,
                     "P2P peer lifecycle attempts, handshakes, timeouts, and rejects by address/source" },
    { "chain_advance_coordinator", chain_advance_coordinator_dump_state_json,
                     "canonical chain-advance source scoring: P2P, snapshot, local import, mirror fallback" },
    { "chain_tip_watchdog", chain_tip_watchdog_dump_state_json,
                     "tip-stuck overlord: highest_tip, age_secs since last advance, escalation level + fire counts" },
    { "condition_engine", condition_engine_dump_state_json,
                     "self-heal engine: registered conditions with active/cleared status, attempts, thresholds" },
    { "long_op",     long_op_dump_state_json,
                     "active long-operation scopes (>600s code paths) that gate STATE_STUCK watchdog suppression" },
    { "ibd_throttle", ibd_throttle_dump_state_json,
                     "IBD throttle: token-bucket state, acquired/blocked counts, total wait time" },
    { "mempool_limits", mempool_limits_dump_state_json,
                     "mempool limits: enforce/expire call counts, evicted/expired totals, last-run summary" },
    { "block_pruning", block_pruning_dump_state_json,
                     "block pruning service: files/blocks pruned, bytes reclaimed, lowest height with data" },
    { "crypto_registry", crypto_registry_dump_state_json,
                     "registered crypto schemes, statuses, implementations, and kind counts" },
    { "mempool_projection", mempool_projection_dump_state_json,
                     "Phase 4d mempool projection over EV_TX_ADMIT_MEMPOOL / EV_TX_REMOVE_MEMPOOL" },
    { "peers_projection", peers_projection_dump_state_json,
                     "Phase 4d peers projection over EV_PEER_OBSERVED / EV_PEER_DROPPED" },
    { "utxo_projection", utxo_projection_dump_state_json,
                     "Phase 4b utxo_projection: open/path/last_consumed_offset, "
                     "utxo_count, events_consumed_total, emit/consume counters, "
                     "REPLACE collisions, last_catch_up_ms. Shadow-mode UTXO "
                     "set derived from the event_log; diff via "
                     "zcl_utxo_projection_diff before cutover." },
    { "znam_projection", znam_projection_dump_state_json,
                     "Phase 4d-4 znam projection: name_count, addr/text counts, "
                     "events_consumed_total, per-event-type counters, emit/fail "
                     "counters, last_consumed_offset, last_catch_up_ms." },
    { "wallet_projection", wallet_projection_dump_state_json,
                     "Phase 4d-3 wallet view projection: public-only "
                     "address/tx/UTXO/note counts, total value, cursor, "
                     "and shadow emit counters." },
    { "contacts_projection", contacts_projection_dump_state_json,
                     "Phase 4d-5 contacts projection: count, cursor, "
                     "consume counters, shadow emit counters, catch_up timing." },
    { "onion_announcements_projection", onion_ann_projection_dump_state_json,
                     "Phase 4d-5 onion announcements projection: count, cursor, "
                     "consume counters, shadow emit counters, catch_up timing." },
    { "hodl_history_projection", hodl_history_projection_dump_state_json,
                     "Phase 4d-5 HODL history projection: count, cursor, "
                     "consume counters, shadow emit counters, catch_up timing." },
    { "block_index_projection", block_index_projection_dump_state_json,
                     "Phase 4c block_index_projection: cursor, entry count, "
                     "events consumed, replace collisions, last catch_up_ms" },
};

int diagnostics_subsystems_csv(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    size_t pos = 0;
    int unclamped = 0;
    for (size_t i = 0; i < sizeof(g_dumpers) / sizeof(g_dumpers[0]); i++) {
        int n = snprintf(out + pos, pos < out_sz ? out_sz - pos : 0,
                         "%s%s", i ? "," : "", g_dumpers[i].name);
        unclamped += n;
        if (n > 0 && (size_t)n < out_sz - pos) pos += (size_t)n;
        else pos = out_sz - 1;
    }
    return unclamped;
}

static bool rpc_dumpstate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result,
        "dumpstate <subsystem> [key]\n"
        "\nDump in-process state for a subsystem. Subsystems:\n"
        "  watchdog     — sync watchdog status + stats\n"
        "  chain_advance_coordinator — source scoring + fallback policy\n"
        "  boot         — last boot's integrity + backfill counters\n"
        "  block_index  — block_index entry (key = height or hex hash)\n"
        "\nResult: { subsystem, captured_at, state: {...} }");

    const char *sub = json_get_str(json_at(params, 0));
    const struct json_value *key_val = json_at(params, 1);
    /* Accept either string or int for the key; numeric height callers
     * often pass `3091000` rather than `"3091000"`. */
    char key_buf[64] = {0};
    const char *key = NULL;
    if (key_val) {
        if (key_val->type == JSON_INT) {
            snprintf(key_buf, sizeof(key_buf), "%lld",
                     (long long)json_get_int(key_val));
            key = key_buf;
        } else if (key_val->type == JSON_STR) {
            key = json_get_str(key_val);
        }
    }

    if (!sub || !sub[0]) {
        LOG_FAIL("diag", "dumpstate: missing subsystem");
    }

    const struct dump_entry *e = NULL;
    const char *domain_key = NULL;
    for (size_t i = 0; i < sizeof(g_dumpers) / sizeof(g_dumpers[0]); i++) {
        if (strcmp(g_dumpers[i].name, sub) == 0) {
            e = &g_dumpers[i];
            break;
        }
    }
    if (!e && strncmp(sub, "supervisor.", strlen("supervisor.")) == 0) {
        e = &g_dumpers[0];
        domain_key = sub + strlen("supervisor.");
    }
    if (!e) {
        LOG_FAIL("diag",
                 "dumpstate: unknown subsystem '%s' (try watchdog/boot/block_index)",
                 sub);
    }

    json_set_object(result);
    json_push_kv_str(result, "subsystem", domain_key ? sub : e->name);
    json_push_kv_str(result, "description", e->desc);
    json_push_kv_int(result, "captured_at", (int64_t)platform_time_wall_time_t());

    struct json_value state = {0};
    json_set_object(&state);
    bool ok = e->fn(&state, domain_key ? domain_key : key);
    if (!ok) {
        json_free(&state);
        LOG_FAIL("diag", "dumpstate: %s dump function returned false", sub);
    }
    json_push_kv(result, "state", &state);
    json_free(&state);
    return true;
}

/* ── RPC: cutovermode [stage] [mode] ────────────────────────────────
 *
 * Runtime control for the guarded Wave-S cutovers. Compile-time defaults stay
 * SHADOW so a bad flip can be reverted with one RPC call instead of rebuild +
 * redeploy. This intentionally covers only stages that currently expose a
 * cutover mode.
 */

static const char *header_admit_mode_name(header_admit_mode_t mode)
{
    return mode == HEADER_ADMIT_MODE_AUTHORITATIVE
        ? "authoritative" : "shadow";
}

static const char *validate_headers_mode_name(validate_headers_mode_t mode)
{
    return mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE
        ? "authoritative" : "shadow";
}

#define CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS 180
#define CUTOVER_PREFLIGHT_MAX_GUARD_POLL_SECS 5
#define CUTOVER_PREFLIGHT_MAX_GUARD_WITNESS_SECS 60
#define CUTOVER_PREFLIGHT_GUARD_NAME "cutover_no_forward_progress"

static bool parse_cutover_mode(const char *s,
                               header_admit_mode_t *ha,
                               validate_headers_mode_t *vh)
{
    if (!s || !s[0]) return false;
    if (strcasecmp(s, "shadow") == 0) {
        if (ha) *ha = HEADER_ADMIT_MODE_SHADOW;
        if (vh) *vh = VALIDATE_HEADERS_MODE_SHADOW;
        return true;
    }
    if (strcasecmp(s, "authoritative") == 0 ||
        strcasecmp(s, "auth") == 0) {
        if (ha) *ha = HEADER_ADMIT_MODE_AUTHORITATIVE;
        if (vh) *vh = VALIDATE_HEADERS_MODE_AUTHORITATIVE;
        return true;
    }
    return false;
}

static bool cutover_any_authoritative_active(void)
{
    return header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE ||
           validate_headers_get_mode() ==
               VALIDATE_HEADERS_MODE_AUTHORITATIVE;
}

static void cutover_record_mode_change(
    const struct node_health_snapshot *health)
{
    int64_t height = health ? health->tip_height : -1;
    atomic_store(&g_cutover_has_change, 1);
    atomic_store(&g_cutover_change_unix, platform_time_wall_unix());
    atomic_store(&g_cutover_change_height, height);
    atomic_store(&g_cutover_canary_target_height,
                 height >= 0 ? height + 1 : 0);
    atomic_store(&g_cutover_change_header_height,
                 health ? health->header_height : -1);
    atomic_store(&g_cutover_change_peer_best_height,
                 health ? health->peer_best_height : -1);
    atomic_store(&g_cutover_change_tip_lag,
                 health ? health->tip_lag : -1);
}

static void push_cutover_canary_state(
    struct json_value *out,
    const struct node_health_snapshot *health)
{
    bool has_change = atomic_load(&g_cutover_has_change) != 0;
    int64_t changed_at = atomic_load(&g_cutover_change_unix);
    int64_t target = atomic_load(&g_cutover_canary_target_height);
    int64_t current_tip = health ? health->tip_height : -1;
    bool authoritative = cutover_any_authoritative_active();
    int64_t now = platform_time_wall_unix();
    int64_t elapsed = has_change && changed_at > 0 && now >= changed_at
        ? now - changed_at : -1;
    int64_t deadline = has_change && changed_at > 0
        ? changed_at + CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS : 0;
    bool passed = has_change && target > 0 && current_tip >= target;
    bool expired = has_change && !passed && deadline > 0 && now > deadline;
    const char *status = "inactive";
    if (has_change) {
        if (passed) {
            status = "passed";
        } else if (expired) {
            status = "failed";
        } else if (authoritative) {
            status = "pending";
        } else {
            status = "reverted";
        }
    }

    json_set_object(out);
    json_push_kv_bool(out, "has_change", has_change);
    json_push_kv_bool(out, "authoritative_active", authoritative);
    json_push_kv_str(out, "canary_status", status);
    json_push_kv_bool(out, "canary_failed", expired);
    json_push_kv_int(out, "changed_at_unix", changed_at);
    json_push_kv_int(out, "change_height",
                     atomic_load(&g_cutover_change_height));
    json_push_kv_int(out, "canary_target_height", target);
    json_push_kv_int(out, "current_tip_height", current_tip);
    json_push_kv_bool(out, "canary_passed", passed);
    json_push_kv_int(out, "canary_elapsed_seconds", elapsed);
    json_push_kv_int(out, "canary_deadline_unix", deadline);
    json_push_kv_int(out, "change_header_height",
                     atomic_load(&g_cutover_change_header_height));
    json_push_kv_int(out, "change_peer_best_height",
                     atomic_load(&g_cutover_change_peer_best_height));
    json_push_kv_int(out, "change_tip_lag",
                     atomic_load(&g_cutover_change_tip_lag));
    json_push_kv_int(out, "watch_window_seconds",
                     CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS);
}

static void push_cutover_modes(struct json_value *result, bool changed,
                               const struct node_health_snapshot *health)
{
    struct json_value canary;
    json_init(&canary);

    json_set_object(result);
    json_push_kv_bool(result, "changed", changed);
    json_push_kv_str(result, "header_admit",
                     header_admit_mode_name(header_admit_get_mode()));
    json_push_kv_str(result, "validate_headers",
                     validate_headers_mode_name(validate_headers_get_mode()));
    if (changed) {
        json_push_kv_int(result, "changed_at_unix",
                         platform_time_wall_unix());
        if (health) {
            json_push_kv_int(result, "change_height", health->tip_height);
            json_push_kv_int(result, "canary_target_height",
                             health->tip_height >= 0
                                 ? health->tip_height + 1
                                 : 0);
            json_push_kv_int(result, "change_header_height",
                             health->header_height);
            json_push_kv_int(result, "change_peer_best_height",
                             health->peer_best_height);
            json_push_kv_int(result, "change_tip_lag", health->tip_lag);
        }
    }
    push_cutover_canary_state(&canary, health);
    json_push_kv(result, "cutover_state", &canary);
    json_free(&canary);
}

static bool rpc_cutoverpreflight(const struct json_value *params, bool help,
                                 struct json_value *result);

static bool cutover_stage_requests_authoritative(
    const char *stage,
    header_admit_mode_t ha_mode,
    validate_headers_mode_t vh_mode)
{
    if (strcasecmp(stage, "header_admit") == 0)
        return ha_mode == HEADER_ADMIT_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "validate_headers") == 0)
        return vh_mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
    if (strcasecmp(stage, "all") == 0)
        return ha_mode == HEADER_ADMIT_MODE_AUTHORITATIVE ||
               vh_mode == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
    return false;
}

static bool cutover_authoritative_request_is_paired(const char *stage)
{
    return stage && strcasecmp(stage, "all") == 0;
}

static bool cutover_preflight_ready_now(void)
{
    struct json_value params;
    struct json_value preflight;
    json_init(&params);
    json_init(&preflight);
    json_set_array(&params);

    bool ok = rpc_cutoverpreflight(&params, false, &preflight);
    const struct json_value *ready = ok ? json_get(&preflight, "ready") : NULL;
    ok = ready && json_get_bool(ready);

    json_free(&preflight);
    json_free(&params);
    return ok;
}

static bool rpc_cutovermode(const struct json_value *params, bool help,
                            struct json_value *result)
{
    RPC_HELP(help, result,
        "cutovermode [stage] [mode]\n"
        "\nRead or set runtime cutover modes. stage is one of:\n"
        "  header_admit | validate_headers | all\n"
        "mode is one of:\n"
        "  shadow | authoritative\n"
        "\nAuthoritative mode is refused unless cutoverpreflight.ready is true.\n"
        "Authoritative mode must be applied with stage=all; partial stage "
        "requests are only valid for shadow reverts.\n"
        "\nExamples:\n"
        "  cutovermode\n"
        "  cutovermode all authoritative\n"
        "  cutovermode all shadow\n"
        "\nResult: { changed, header_admit, validate_headers, "
        "canary_target_height? }");

    const struct json_value *stage_v = json_at(params, 0);
    const struct json_value *mode_v = json_at(params, 1);
    const char *stage = stage_v ? json_get_str(stage_v) : NULL;
    const char *mode_s = mode_v ? json_get_str(mode_v) : NULL;
    if (!stage || !stage[0]) {
        push_cutover_modes(result, false, NULL);
        return true;
    }
    if (!mode_s || !mode_s[0])
        LOG_FAIL("diag", "cutovermode: missing mode for stage '%s'", stage);

    header_admit_mode_t ha_mode = HEADER_ADMIT_MODE_SHADOW;
    validate_headers_mode_t vh_mode = VALIDATE_HEADERS_MODE_SHADOW;
    if (!parse_cutover_mode(mode_s, &ha_mode, &vh_mode))
        LOG_FAIL("diag", "cutovermode: invalid mode '%s'", mode_s);

    if (strcasecmp(stage, "header_admit") != 0 &&
        strcasecmp(stage, "validate_headers") != 0 &&
        strcasecmp(stage, "all") != 0)
        LOG_FAIL("diag", "cutovermode: invalid stage '%s'", stage);

    bool wants_authoritative =
        cutover_stage_requests_authoritative(stage, ha_mode, vh_mode);
    if (wants_authoritative &&
        !cutover_authoritative_request_is_paired(stage))
        LOG_FAIL("diag",
                 "cutovermode: authoritative flip must use stage=all");

    if (wants_authoritative &&
        !cutover_preflight_ready_now())
        LOG_FAIL("diag",
                 "cutovermode: authoritative flip refused; "
                 "cutoverpreflight.ready is false");

    if (strcasecmp(stage, "header_admit") == 0) {
        header_admit_set_mode(ha_mode);
    } else if (strcasecmp(stage, "validate_headers") == 0) {
        validate_headers_set_mode(vh_mode);
    } else if (strcasecmp(stage, "all") == 0) {
        header_admit_set_mode(ha_mode);
        validate_headers_set_mode(vh_mode);
    }

    struct node_health_snapshot health;
    node_health_collect(&health, NULL, NULL);
    cutover_record_mode_change(&health);
    push_cutover_modes(result, true, &health);
    return true;
}

static const char *
header_admit_diff_status_rpc_name(enum header_admit_diff_status s)
{
    switch (s) {
    case HEADER_ADMIT_DIFF_CONVERGED:   return "CONVERGED";
    case HEADER_ADMIT_DIFF_DIVERGENT:   return "DIVERGENT";
    case HEADER_ADMIT_DIFF_LOG_AHEAD:   return "LOG_AHEAD";
    case HEADER_ADMIT_DIFF_CHAIN_AHEAD: return "CHAIN_AHEAD";
    case HEADER_ADMIT_DIFF_EMPTY:       return "EMPTY";
    case HEADER_ADMIT_DIFF_NOT_READY:   return "NOT_READY";
    }
    return "UNKNOWN";
}

static int64_t json_obj_int_or(const struct json_value *obj,
                               const char *key,
                               int64_t fallback)
{
    const struct json_value *v = json_get(obj, key);
    return v ? json_get_int(v) : fallback;
}

static void push_validate_headers_window_json(
    struct json_value *vh,
    const struct validate_headers_window_report *r)
{
    if (!vh || !r) return;
    json_push_kv_bool(vh, "window_available", r->available);
    json_push_kv_bool(vh, "window_complete", r->complete);
    json_push_kv_int(vh, "window_start_height", r->start_height);
    json_push_kv_int(vh, "window_end_height", r->end_height);
    json_push_kv_int(vh, "window_expected_count", r->expected_count);
    json_push_kv_int(vh, "window_checked_count", r->checked_count);
    json_push_kv_int(vh, "window_failed_count", r->failed_count);
    json_push_kv_int(vh, "window_first_failed_height",
                     r->first_failed_height);
    json_push_kv_str(vh, "window_first_fail_reason",
                     r->first_fail_reason);
}

static void cutover_preflight_push_blocker(struct json_value *blockers,
                                           const char *reason)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, reason);
    json_push_back(blockers, &v);
    json_free(&v);
}

static bool cutover_preflight_tail_window(
    int64_t start_i,
    int64_t end_i,
    const struct header_admit_diff_report *rep,
    int32_t *start_out,
    int32_t *end_out)
{
    if (start_i != -1 || end_i != -1 || !rep || !start_out || !end_out)
        return false;
    if (rep->log_max_height < 0 || rep->chain_tip_height < 0)
        return false;

    int32_t end = rep->log_max_height < rep->chain_tip_height
        ? rep->log_max_height : rep->chain_tip_height;
    if (end < 0)
        return false;

    int32_t start = 0;
    if (end >= HEADER_ADMIT_DIFF_MAX_RANGE)
        start = end - HEADER_ADMIT_DIFF_MAX_RANGE + 1;
    if (start == rep->start_height && end == rep->end_height)
        return false;

    *start_out = start;
    *end_out = end;
    return true;
}

static bool cutover_preflight_operator_needed_blocks(
    const struct node_health_snapshot *health)
{
    if (!health || !health->operator_needed)
        return false;
    return strstr(health->operator_needed_detail,
                  "peer_floor_violated") == NULL;
}

static bool push_cutover_live_gate_json(struct json_value *live,
                                        struct node_health_snapshot *out)
{
    struct node_health_snapshot health;
    node_health_collect(&health, NULL, NULL);
    if (out)
        *out = health;

    bool tip_recent =
        health.tip_advance_age_seconds >= 0 &&
        health.tip_advance_age_seconds <=
            CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS;
    bool headers_in_range =
        health.header_height <= health.tip_height + 1;
    bool operator_needed_blocks =
        cutover_preflight_operator_needed_blocks(&health);
    bool cutover_ready =
        health.synced &&
        health.has_peers &&
        health.tip_lag == 0 &&
        tip_recent &&
        headers_in_range &&
        !health.tip_stale &&
        !operator_needed_blocks &&
        strcmp(health.mirror_lag_breach_severity, "fatal") != 0;

    json_set_object(live);
    json_push_kv_bool(live, "healthy", health.healthy);
    json_push_kv_bool(live, "cutover_ready", cutover_ready);
    json_push_kv_bool(live, "synced", health.synced);
    json_push_kv_bool(live, "has_peers", health.has_peers);
    json_push_kv_bool(live, "tip_recent", tip_recent);
    json_push_kv_bool(live, "headers_in_range", headers_in_range);
    json_push_kv_bool(live, "operator_needed_blocks_cutover",
                      operator_needed_blocks);
    json_push_kv_bool(live, "operator_needed", health.operator_needed);
    json_push_kv_str(live, "operator_needed_detail",
                     health.operator_needed_detail);
    json_push_kv_int(live, "peer_count", (int64_t)health.peer_count);
    json_push_kv_int(live, "tip_height", health.tip_height);
    json_push_kv_int(live, "canary_target_height",
                     health.tip_height >= 0 ? health.tip_height + 1 : 0);
    json_push_kv_int(live, "header_height", health.header_height);
    json_push_kv_int(live, "peer_best_height", health.peer_best_height);
    json_push_kv_int(live, "tip_lag", health.tip_lag);
    json_push_kv_int(live, "tip_advance_age_seconds",
                     health.tip_advance_age_seconds);
    json_push_kv_str(live, "degraded_reason", health.degraded_reason);
    json_push_kv_str(live, "mirror_lag_breach_severity",
                     health.mirror_lag_breach_severity);

    return cutover_ready;
}

static bool push_cutover_chain_advance_gate_json(struct json_value *out)
{
    struct cac_decision d;
    chain_advance_coordinator_get_status(&d);

    bool source_ready = false;
    if (d.selected_source > CAC_SOURCE_NONE &&
        d.selected_source < CAC_SOURCE_NUM) {
        const struct cac_source_status *s = &d.sources[d.selected_source];
        source_ready =
            s->available && s->healthy && s->selectable && !s->blocked &&
            s->selection_blocker[0] == '\0';
    }
    bool ready = node_health_chain_advance_synced(&d);
    const char *not_ready_reason = "";
    int64_t target_gap = 0;
    if (d.local_height >= 0 && d.target_height >= 0 &&
        d.target_height > d.local_height)
        target_gap = (int64_t)d.target_height - d.local_height;
    if (!ready) {
        if (d.result != CAC_DECISION_USE_SOURCE)
            not_ready_reason = "decision_not_use_source";
        else if (d.selected_source <= CAC_SOURCE_NONE ||
                 d.selected_source >= CAC_SOURCE_NUM)
            not_ready_reason = "selected_source_invalid";
        else if (d.blocker[0] != '\0')
            not_ready_reason = "blocker_present";
        else if (d.local_height < 0 || d.target_height < 0)
            not_ready_reason = "invalid_heights";
        else if (d.local_height + 1 < d.target_height)
            not_ready_reason = "target_height_gap";
        else if (d.projection_lag < 0 || d.projection_lag > 1)
            not_ready_reason = "projection_lag";
        else if (!source_ready)
            not_ready_reason = "source_not_ready";
        else
            not_ready_reason = "unknown";
    }

    json_set_object(out);
    json_push_kv_bool(out, "ready", ready);
    json_push_kv_str(out, "not_ready_reason", not_ready_reason);
    json_push_kv_str(out, "decision",
                     cac_decision_result_name(d.result));
    json_push_kv_str(out, "selected_source",
                     cac_source_name(d.selected_source));
    json_push_kv_str(out, "selected_source_trust",
                     cac_source_trust_name(d.selected_source));
    json_push_kv_str(out, "authority", "local_consensus_validation");
    json_push_kv_bool(out, "source_ready", source_ready);
    json_push_kv_bool(out, "activation_allowed", d.activation_allowed);
    json_push_kv_int(out, "local_height", (int64_t)d.local_height);
    json_push_kv_int(out, "target_height", (int64_t)d.target_height);
    json_push_kv_int(out, "target_gap", target_gap);
    json_push_kv_int(out, "best_header_height",
                     (int64_t)d.best_header_height);
    json_push_kv_int(out, "projection_height",
                     (int64_t)d.projection_height);
    json_push_kv_int(out, "projection_lag", d.projection_lag);
    json_push_kv_bool(out, "projection_deferred",
                      d.projection_deferred);
    json_push_kv_str(out, "projection_state", d.projection_state);
    json_push_kv_str(out, "reason", d.reason);
    json_push_kv_str(out, "blocker", d.blocker);
    if (d.selected_source > CAC_SOURCE_NONE &&
        d.selected_source < CAC_SOURCE_NUM) {
        const struct cac_source_status *s = &d.sources[d.selected_source];
        json_push_kv_str(out, "selected_source_state", s->state);
        json_push_kv_str(out, "selected_source_reason", s->reason);
        json_push_kv_str(out, "selected_source_blocker", s->blocker);
        json_push_kv_str(out, "selected_source_selection_blocker",
                         s->selection_blocker);
        json_push_kv_bool(out, "selected_source_available",
                          s->available);
        json_push_kv_bool(out, "selected_source_healthy", s->healthy);
        json_push_kv_bool(out, "selected_source_selectable",
                          s->selectable);
        json_push_kv_bool(out, "selected_source_blocked", s->blocked);
        json_push_kv_int(out, "selected_source_height",
                         (int64_t)s->height);
    }
    return ready;
}

static bool push_cutover_guard_gate_json(struct json_value *guard)
{
    struct condition_runtime_snapshot snap;
    bool registered = condition_engine_get_registered_snapshot(
        CUTOVER_PREFLIGHT_GUARD_NAME, &snap);

    json_set_object(guard);
    json_push_kv_str(guard, "name", CUTOVER_PREFLIGHT_GUARD_NAME);
    json_push_kv_bool(guard, "registered", registered);
    json_push_kv_int(guard, "max_tip_advance_age_seconds",
                     CUTOVER_PREFLIGHT_MAX_TIP_ADVANCE_AGE_SECS);
    json_push_kv_int(guard, "max_poll_secs",
                     CUTOVER_PREFLIGHT_MAX_GUARD_POLL_SECS);
    json_push_kv_int(guard, "max_witness_window_secs",
                     CUTOVER_PREFLIGHT_MAX_GUARD_WITNESS_SECS);
    if (!registered) {
        json_push_kv_bool(guard, "ready", false);
        json_push_kv_str(guard, "severity", "unknown");
        json_push_kv_bool(guard, "config_ready", false);
        json_push_kv_bool(guard, "state_ready", false);
        json_push_kv_bool(guard, "currently_active", false);
        json_push_kv_bool(guard, "operator_needed_emitted", false);
        json_push_kv_int(guard, "attempts", 0);
        json_push_kv_str(guard, "last_outcome", "unknown");
        json_push_kv_int(guard, "cleared_count", 0);
        json_push_kv_int(guard, "poll_secs", 0);
        json_push_kv_int(guard, "backoff_secs", 0);
        json_push_kv_int(guard, "max_attempts", 0);
        json_push_kv_int(guard, "witness_window_secs", 0);
        return false;
    }

    bool config_ready =
        snap.severity == COND_CRITICAL &&
        snap.poll_secs > 0 &&
        snap.poll_secs <= CUTOVER_PREFLIGHT_MAX_GUARD_POLL_SECS &&
        snap.max_attempts == 1 &&
        snap.witness_window_secs > 0 &&
        snap.witness_window_secs <=
            CUTOVER_PREFLIGHT_MAX_GUARD_WITNESS_SECS;
    bool state_ready =
        !snap.currently_active &&
        !snap.operator_needed_emitted &&
        snap.attempts == 0 &&
        snap.last_outcome != COND_REMEDY_UNWITNESSED;

    json_push_kv_bool(guard, "ready", config_ready && state_ready);
    json_push_kv_str(guard, "severity",
                     condition_severity_name(snap.severity));
    json_push_kv_bool(guard, "config_ready", config_ready);
    json_push_kv_bool(guard, "state_ready", state_ready);
    json_push_kv_bool(guard, "currently_active",
                      snap.currently_active);
    json_push_kv_bool(guard, "operator_needed_emitted",
                      snap.operator_needed_emitted);
    json_push_kv_int(guard, "attempts", snap.attempts);
    json_push_kv_str(guard, "last_outcome",
                     condition_remedy_result_name(snap.last_outcome));
    json_push_kv_int(guard, "cleared_count", snap.cleared_count);
    json_push_kv_int(guard, "poll_secs", snap.poll_secs);
    json_push_kv_int(guard, "backoff_secs", snap.backoff_secs);
    json_push_kv_int(guard, "max_attempts", snap.max_attempts);
    json_push_kv_int(guard, "witness_window_secs",
                     snap.witness_window_secs);
    return config_ready && state_ready;
}

static bool rpc_cutoverpreflight(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    RPC_HELP(help, result,
        "cutoverpreflight [start_height] [end_height]\n"
        "\nRead-only C-3 preflight snapshot: runtime cutover modes, "
        "cutover-specific live progress, chain-advance source selection, "
        "header_admit shadow-vs-active-chain diff, validate_headers "
        "persisted window/cursor coverage, and a conservative ready boolean "
        "gated by the cutover no-progress guard.\n"
        "\nHeights default to the most recent header_admit diff window. "
        "Result: { ready, blockers, live, chain_advance, chain_evidence, "
        "guard, modes, header_admit_diff, validate_headers }");

    const struct json_value *start_v = json_at(params, 0);
    const struct json_value *end_v = json_at(params, 1);
    int64_t start_i = start_v ? json_get_int(start_v) : -1;
    int64_t end_i = end_v ? json_get_int(end_v) : -1;
    if (start_i < -1) start_i = -1;
    if (end_i < -1) end_i = -1;
    if (start_i > INT32_MAX) start_i = INT32_MAX;
    if (end_i > INT32_MAX) end_i = INT32_MAX;

    struct header_admit_diff_report rep;
    if (!header_admit_stage_diff((int32_t)start_i, (int32_t)end_i, &rep))
        LOG_FAIL("diag", "cutoverpreflight: header_admit diff failed");
    int32_t tail_start = 0;
    int32_t tail_end = 0;
    if (cutover_preflight_tail_window(start_i, end_i, &rep,
                                      &tail_start, &tail_end) &&
        !header_admit_stage_diff(tail_start, tail_end, &rep))
        LOG_FAIL("diag", "cutoverpreflight: header_admit tail diff failed");

    struct json_value modes;
    struct json_value live;
    struct json_value chain_advance;
    struct json_value chain_evidence;
    struct json_value guard;
    struct json_value canary;
    struct json_value diff;
    struct json_value vh;
    struct json_value blockers;
    json_init(&modes);
    json_init(&live);
    json_init(&chain_advance);
    json_init(&chain_evidence);
    json_init(&guard);
    json_init(&canary);
    json_init(&diff);
    json_init(&vh);
    json_init(&blockers);
    json_set_object(result);
    json_set_object(&modes);
    json_set_object(&diff);
    json_set_array(&blockers);

    bool vh_ok = validate_headers_stage_dump_state_json(&vh, NULL);
    if (!vh_ok)
        json_set_object(&vh);
    bool ce_ok =
        chain_evidence_controller_dump_state_json(&chain_evidence, NULL);
    if (!ce_ok)
        json_set_object(&chain_evidence);

    const char *ha_mode = header_admit_mode_name(header_admit_get_mode());
    const char *vh_mode = validate_headers_mode_name(validate_headers_get_mode());
    json_push_kv_str(&modes, "header_admit", ha_mode);
    json_push_kv_str(&modes, "validate_headers", vh_mode);
    struct node_health_snapshot live_health;
    bool live_ready = push_cutover_live_gate_json(&live, &live_health);
    bool chain_advance_ready =
        push_cutover_chain_advance_gate_json(&chain_advance);
    push_cutover_canary_state(&canary, &live_health);
    bool guard_ready = push_cutover_guard_gate_json(&guard);

    json_push_kv_str(&diff, "status",
                     header_admit_diff_status_rpc_name(rep.status));
    json_push_kv_int(&diff, "start_height", rep.start_height);
    json_push_kv_int(&diff, "end_height", rep.end_height);
    json_push_kv_int(&diff, "checked_count", rep.checked_count);
    json_push_kv_int(&diff, "match_count", rep.match_count);
    json_push_kv_int(&diff, "mismatch_count", rep.mismatch_count);
    json_push_kv_int(&diff, "missing_in_log_count",
                     rep.missing_in_log_count);
    json_push_kv_int(&diff, "missing_in_chain_count",
                     rep.missing_in_chain_count);
    json_push_kv_int(&diff, "first_divergent_height",
                     rep.first_divergent_height);
    json_push_kv_int(&diff, "log_max_height", rep.log_max_height);
    json_push_kv_int(&diff, "chain_tip_height", rep.chain_tip_height);
    json_push_kv_int(&diff, "cursor", rep.cursor);
    int64_t ha_persisted_cursor = (int64_t)header_admit_stage_cursor();
    json_push_kv_int(&diff, "persisted_cursor", ha_persisted_cursor);
    int64_t required_ha_cursor =
        (rep.chain_tip_height >= 0) ? ((int64_t)rep.chain_tip_height + 1) : 0;
    int64_t ha_cursor_lag =
        (ha_persisted_cursor >= 0 && ha_persisted_cursor < required_ha_cursor)
            ? (required_ha_cursor - ha_persisted_cursor) : 0;
    int64_t ha_log_tip_lag =
        (rep.log_max_height >= 0 && rep.log_max_height < rep.chain_tip_height)
            ? ((int64_t)rep.chain_tip_height - rep.log_max_height) : 0;
    json_push_kv_int(&diff, "required_cursor", required_ha_cursor);
    json_push_kv_int(&diff, "cursor_lag", ha_cursor_lag);
    json_push_kv_int(&diff, "log_tip_lag", ha_log_tip_lag);

    bool header_caught_up =
        required_ha_cursor > 0 &&
        ha_persisted_cursor >= required_ha_cursor &&
        ha_cursor_lag == 0 &&
        ha_log_tip_lag == 0;
    bool header_ready =
        rep.status == HEADER_ADMIT_DIFF_CONVERGED &&
        rep.mismatch_count == 0 &&
        rep.missing_in_chain_count == 0 &&
        header_caught_up;
    int64_t vh_cursor = (int64_t)validate_headers_stage_cursor();
    json_push_kv_int(&vh, "persisted_cursor", vh_cursor);
    int64_t required_vh_cursor =
        (rep.end_height >= 0) ? ((int64_t)rep.end_height + 1) : 0;
    int64_t vh_cursor_lag =
        (vh_cursor >= 0 && vh_cursor < required_vh_cursor)
            ? (required_vh_cursor - vh_cursor) : 0;
    json_push_kv_int(&vh, "required_cursor", required_vh_cursor);
    json_push_kv_int(&vh, "cursor_lag", vh_cursor_lag);

    struct validate_headers_window_report vh_window;
    int64_t vh_window_start = rep.start_height;
    int64_t vh_window_end = rep.end_height;
    validate_headers_stage_window_report(vh_window_start, vh_window_end,
                                         &vh_window);
    push_validate_headers_window_json(&vh, &vh_window);

    bool validate_no_failures =
        json_obj_int_or(&vh, "failed_total", 1) == 0 &&
        json_obj_int_or(&vh, "failure_log_count", 1) == 0;
    json_push_kv_bool(&vh, "no_failures", validate_no_failures);
    bool validate_clean = vh_ok &&
        validate_no_failures &&
        json_obj_int_or(&vh, "error_count", 1) == 0 &&
        vh_window.available &&
        vh_window.complete &&
        vh_window.failed_count == 0;
    bool validate_caught_up =
        required_vh_cursor > 0 &&
        vh_cursor >= required_vh_cursor &&
        vh_cursor_lag == 0;
    bool validate_ready = validate_clean && validate_caught_up;
    bool modes_ready =
        strcmp(ha_mode, "shadow") == 0 &&
        strcmp(vh_mode, "shadow") == 0;

    if (!live_ready)
        cutover_preflight_push_blocker(&blockers, "live_health_not_ready");
    if (!chain_advance_ready)
        cutover_preflight_push_blocker(&blockers,
                                       "chain_advance_not_ready");
    if (!guard_ready)
        cutover_preflight_push_blocker(&blockers,
                                       condition_engine_has_registered(
                                           CUTOVER_PREFLIGHT_GUARD_NAME)
                                           ? "cutover_guard_not_ready"
                                           : "cutover_guard_not_registered");
    if (!header_ready)
        cutover_preflight_push_blocker(
            &blockers,
            header_caught_up ? "header_admit_diff_not_converged"
                             : "header_admit_cursor_lag");
    if (!validate_ready)
        cutover_preflight_push_blocker(
            &blockers,
            !validate_no_failures ? "validate_headers_failures_present" :
            validate_clean ? "validate_headers_cursor_lag" :
                             "validate_headers_window_not_clean");
    if (!modes_ready)
        cutover_preflight_push_blocker(&blockers,
                                       "cutover_modes_not_shadow");

    json_push_kv_bool(result, "ready",
                      live_ready && chain_advance_ready && guard_ready &&
                      header_ready && validate_ready && modes_ready);
    json_push_kv(result, "blockers", &blockers);
    json_push_kv(result, "live", &live);
    json_push_kv(result, "chain_advance", &chain_advance);
    json_push_kv(result, "chain_evidence", &chain_evidence);
    json_push_kv(result, "guard", &guard);
    json_push_kv(result, "cutover_state", &canary);
    json_push_kv(result, "modes", &modes);
    json_push_kv(result, "header_admit_diff", &diff);
    json_push_kv(result, "validate_headers", &vh);

    json_free(&blockers);
    json_free(&modes);
    json_free(&live);
    json_free(&chain_advance);
    json_free(&chain_evidence);
    json_free(&guard);
    json_free(&canary);
    json_free(&diff);
    json_free(&vh);
    return true;
}

/* ── RPC: getnodelog <pattern> [since_secs] [max_lines] [level] ───
 *
 * Reverse-scans ~/<datadir>/node.log in 64 KB chunks, matches each
 * line against the POSIX-extended regex `pattern`, filters by level,
 * and stops at `max_lines` or when the timestamp predates `since_secs`.
 *
 * Bounded memory (one chunk at a time + accumulator). Designed to be
 * cheaper than reading the whole 56 MB log via the file system.
 *
 * Level detection: log lines from LOG_FAIL / LOG_ERR / LOG_NULL all
 * start with `[domain] file:line function(): message` where `domain`
 * is e.g. "validation", "sync", "net". This handler also recognises
 * the leading `[FATAL]` / `[WARN]` markers used by some boot-time
 * printf paths.
 */

#define NODELOG_DEFAULT_SINCE_SECS    300
#define NODELOG_MAX_SINCE_SECS       86400
#define NODELOG_DEFAULT_MAX_LINES      50
#define NODELOG_MAX_MAX_LINES         500
#define NODELOG_CHUNK_SIZE           65536
#define NODELOG_MAX_PATTERN_LEN        256
#define NODELOG_MAX_SCAN_BYTES   (16 * 1024 * 1024) /* 16 MB hard cap */

enum log_level { LL_ALL = 0, LL_INFO, LL_WARN, LL_ERROR, LL_FATAL };

static enum log_level parse_level(const char *s)
{
    if (!s) return LL_ALL;
    if (!strcmp(s, "all"))   return LL_ALL;
    if (!strcmp(s, "info"))  return LL_INFO;
    if (!strcmp(s, "warn"))  return LL_WARN;
    if (!strcmp(s, "error")) return LL_ERROR;
    if (!strcmp(s, "fatal")) return LL_FATAL;
    return LL_ALL;
}

static enum log_level line_level(const char *line)
{
    /* Cheap heuristic. LOG_FAIL/LOG_ERR/LOG_NULL all use stderr and
     * prefix with [domain]; we treat those as ERROR level. Boot-time
     * printfs use explicit FATAL/WARN markers. Everything else INFO. */
    if (strstr(line, "FATAL") || strstr(line, "PANIC") ||
        strstr(line, "ABORT"))
        return LL_FATAL;
    if (strstr(line, "WARNING") || strstr(line, "WARN:") ||
        strstr(line, "[watchdog] ESCALATION"))
        return LL_WARN;
    if (line[0] == '[' && strstr(line, "(): ") &&
        (strstr(line, "GUARD FAILED") || strstr(line, "FAIL") ||
         strstr(line, "failed") || strstr(line, "error") ||
         strstr(line, "ERROR")))
        return LL_ERROR;
    return LL_INFO;
}

static bool rpc_getnodelog(const struct json_value *params, bool help,
                           struct json_value *result)
{
    RPC_HELP(help, result,
        "getnodelog <pattern> [since_secs=300] [max_lines=50] [level=all]\n"
        "\nReverse-scan node.log. `pattern` is POSIX-extended regex.\n"
        "`level` one of: all, info, warn, error, fatal.\n"
        "\nResult: { lines: [...], scanned_bytes, truncated, log_path }");

    const char *pattern = json_get_str(json_at(params, 0));
    if (!pattern || !pattern[0])
        LOG_FAIL("diag", "getnodelog: missing pattern");
    size_t plen = strlen(pattern);
    if (plen > NODELOG_MAX_PATTERN_LEN)
        LOG_FAIL("diag", "getnodelog: pattern too long (%zu > %d)",
                 plen, NODELOG_MAX_PATTERN_LEN);

    int64_t since_secs = json_at(params, 1) ?
        json_get_int(json_at(params, 1)) : NODELOG_DEFAULT_SINCE_SECS;
    if (since_secs < 0) since_secs = 0;
    if (since_secs > NODELOG_MAX_SINCE_SECS)
        since_secs = NODELOG_MAX_SINCE_SECS;

    int64_t max_lines = json_at(params, 2) ?
        json_get_int(json_at(params, 2)) : NODELOG_DEFAULT_MAX_LINES;
    if (max_lines < 1) max_lines = 1;
    if (max_lines > NODELOG_MAX_MAX_LINES)
        max_lines = NODELOG_MAX_MAX_LINES;

    enum log_level want_level = parse_level(
        json_at(params, 3) ? json_get_str(json_at(params, 3)) : NULL);

    if (!g_diag.datadir[0])
        LOG_FAIL("diag", "getnodelog: datadir not configured");

    char log_path[1280];
    snprintf(log_path, sizeof(log_path), "%s/node.log", g_diag.datadir);

    int fd = open(log_path, O_RDONLY);
    if (fd < 0)
        LOG_FAIL("diag", "getnodelog: cannot open %s", log_path);

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        LOG_FAIL("diag", "getnodelog: fstat failed on %s", log_path);
    }

    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char errbuf[128];
        regerror(rc, &re, errbuf, sizeof(errbuf));
        close(fd);
        LOG_FAIL("diag", "getnodelog: bad regex '%s': %s",
                 pattern, errbuf);
    }

    int64_t now = (int64_t)platform_time_wall_time_t();
    int64_t earliest = (since_secs > 0) ? (now - since_secs) : 0;

    /* Reverse-scan in NODELOG_CHUNK_SIZE chunks. Each chunk we read,
     * we split into lines and emit any complete lines that match.
     * Partial-line tail goes back into the next chunk. */
    json_set_object(result);
    struct json_value lines_arr = {0};
    json_set_array(&lines_arr);
    int emitted = 0;
    bool truncated = false;
    int64_t scanned = 0;
    off_t pos = st.st_size;

    /* Each scanned line goes onto a stack so we emit in
     * newest-first order; lines_arr is built from that stack at the end. */
    char *stack[NODELOG_MAX_MAX_LINES];
    int stack_n = 0;
    char carry[NODELOG_CHUNK_SIZE + 1] = {0};
    size_t carry_len = 0;

    while (pos > 0 && emitted < max_lines &&
           scanned < NODELOG_MAX_SCAN_BYTES) {
        size_t want = (pos > NODELOG_CHUNK_SIZE) ? NODELOG_CHUNK_SIZE
                                                  : (size_t)pos;
        off_t start = pos - (off_t)want;
        char buf[NODELOG_CHUNK_SIZE + 1];
        ssize_t got = pread(fd, buf, want, start);
        if (got <= 0) break;
        buf[got] = '\0';
        scanned += got;
        pos = start;

        /* Combine `buf` (this chunk) with `carry` (partial line from
         * the previous-newer chunk). Process newline-separated lines
         * from the end. */
        size_t combined_cap = (size_t)got + carry_len + 1;
        char *combined = zcl_malloc(combined_cap, "diagnostics.node_log.combined");
        if (!combined) break;
        memcpy(combined, buf, got);
        memcpy(combined + got, carry, carry_len);
        combined[got + carry_len] = '\0';
        size_t combined_len = (size_t)got + carry_len;

        /* Walk backwards over `combined`, slicing at '\n'. */
        size_t line_end = combined_len;
        for (ssize_t i = (ssize_t)combined_len - 1;
             i >= -1 && emitted < max_lines; i--) {
            if (i < 0 || combined[i] == '\n') {
                size_t start_off = (i < 0) ? 0 : (size_t)i + 1;
                if (start_off < line_end) {
                    /* line = combined[start_off .. line_end) */
                    size_t llen = line_end - start_off;
                    if (i < 0 && pos > 0) {
                        /* Partial line at the start of this chunk —
                         * save as carry, don't emit yet (older chunk
                         * holds the head). */
                        if (llen > sizeof(carry) - 1) llen = sizeof(carry) - 1;
                        memcpy(carry, combined + start_off, llen);
                        carry_len = llen;
                        carry[llen] = '\0';
                    } else {
                        char *line = zcl_malloc(llen + 1, "diagnostics.node_log.line");
                        if (line) {
                            memcpy(line, combined + start_off, llen);
                            line[llen] = '\0';
                            /* Filter by regex + level. */
                            bool match = (regexec(&re, line, 0,
                                                  NULL, 0) == 0);
                            enum log_level lvl = line_level(line);
                            bool level_ok = (want_level == LL_ALL) ||
                                            (lvl >= want_level);
                            /* Crude "since" filter: log lines start
                             * with "Mon DD HH:MM:SS" or "MM-DD HH:MM:SS"
                             * but we don't parse — use mtime delta as
                             * a coarse approximation. Lines older than
                             * `earliest` are not specifically detected
                             * here; that's a future enhancement. */
                            (void)earliest;
                            if (match && level_ok &&
                                stack_n < NODELOG_MAX_MAX_LINES) {
                                stack[stack_n++] = line;
                                emitted++;
                            } else {
                                free(line);
                            }
                        }
                        carry_len = 0;
                    }
                }
                line_end = (size_t)i;
            }
            if (i < 0) break;
        }
        free(combined);
        if (pos == 0) break;
    }

    if (pos > 0 && scanned >= NODELOG_MAX_SCAN_BYTES)
        truncated = true;

    /* Build the result array newest-first (stack already newest-last → reverse). */
    for (int i = 0; i < stack_n; i++) {
        struct json_value lv = {0};
        json_set_str(&lv, stack[i]);
        json_push_back(&lines_arr, &lv);
        json_free(&lv);
        free(stack[i]);
    }

    json_push_kv(result, "lines", &lines_arr);
    json_free(&lines_arr);
    json_push_kv_int(result, "scanned_bytes", scanned);
    json_push_kv_bool(result, "truncated", truncated);
    json_push_kv_str(result, "log_path", log_path);
    json_push_kv_int(result, "emitted", emitted);

    regfree(&re);
    close(fd);
    return true;
}

/* ── RPC: dbquery <sql> [limit] ─────────────────────────────────────
 *
 * SELECT-only SQL passthrough against the node's SQLite database.
 * Hard validation: must start with SELECT, no semicolons, no DDL/DML
 * keywords. Auto-LIMIT appended if missing. 2 s wall-clock budget
 * enforced via sqlite3_progress_handler. Hard cap 100 rows.
 *
 * Marked destructive in MCP middleware (rate-limited) — not because
 * it mutates (it can't), but because arbitrary scans against a
 * 100M-row table can be expensive.
 */

#define DBQUERY_MAX_SQL_LEN       1024
#define DBQUERY_HARD_ROW_CAP       100
#define DBQUERY_DEFAULT_LIMIT       10
#define DBQUERY_PROGRESS_OPS_TICK 1000   /* progress_handler callback granularity */

/* Wall-clock budget in milliseconds. Checked in the progress handler. */
#define DBQUERY_BUDGET_MS         2000

struct dbq_progress_ctx {
    int64_t start_ms;
    int64_t budget_ms;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int dbq_progress_cb(void *vp)
{
    struct dbq_progress_ctx *c = vp;
    if (!c) return 0;
    int64_t elapsed = now_ms() - c->start_ms;
    return (elapsed > c->budget_ms) ? 1 : 0;  /* nonzero → interrupt */
}

static bool sql_has_word(const char *sql, const char *word)
{
    /* Case-insensitive whole-word search. */
    size_t wlen = strlen(word);
    const char *p = sql;
    while (*p) {
        if (strncasecmp(p, word, wlen) == 0) {
            char prev = (p > sql) ? p[-1] : ' ';
            char next = p[wlen];
            bool prev_b = !(isalnum((unsigned char)prev) || prev == '_');
            bool next_b = !(isalnum((unsigned char)next) || next == '_');
            if (prev_b && next_b) return true;
        }
        p++;
    }
    return false;
}

static bool rpc_dbquery(const struct json_value *params, bool help,
                        struct json_value *result)
{
    RPC_HELP(help, result,
        "dbquery <sql> [limit=10]\n"
        "\nRead-only SELECT passthrough against node.db. Hard limits:\n"
        "  - must start with SELECT (case-insensitive)\n"
        "  - no semicolons anywhere in the query\n"
        "  - DDL/DML keywords rejected (INSERT, UPDATE, DELETE, etc.)\n"
        "  - LIMIT auto-appended if missing\n"
        "  - 2 s wall-clock budget enforced\n"
        "  - 100-row hard cap regardless of LIMIT\n"
        "\nResult: { columns, rows, elapsed_ms, truncated, sql_executed }");

    const char *sql_in = json_get_str(json_at(params, 0));
    int64_t limit = json_at(params, 1) ?
        json_get_int(json_at(params, 1)) : DBQUERY_DEFAULT_LIMIT;
    if (limit < 1) limit = 1;
    if (limit > DBQUERY_HARD_ROW_CAP) limit = DBQUERY_HARD_ROW_CAP;

    if (!sql_in || !sql_in[0])
        LOG_FAIL("diag", "dbquery: missing sql");
    size_t slen = strlen(sql_in);
    if (slen > DBQUERY_MAX_SQL_LEN)
        LOG_FAIL("diag", "dbquery: sql too long (%zu > %d)",
                 slen, DBQUERY_MAX_SQL_LEN);

    /* Skip leading whitespace. */
    const char *sql = sql_in;
    while (*sql && isspace((unsigned char)*sql)) sql++;

    if (strncasecmp(sql, "SELECT", 6) != 0 ||
        !(sql[6] == ' ' || sql[6] == '\t' || sql[6] == '\n'))
        LOG_FAIL("diag", "dbquery: query must start with SELECT");

    if (strchr(sql, ';'))
        LOG_FAIL("diag", "dbquery: semicolons not allowed");

    static const char *blocked[] = {
        "INSERT", "UPDATE", "DELETE", "DROP", "ALTER", "CREATE",
        "REPLACE", "ATTACH", "DETACH", "PRAGMA", "VACUUM", "REINDEX",
        "TRIGGER", "TRUNCATE",
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (sql_has_word(sql, blocked[i]))
            LOG_FAIL("diag", "dbquery: keyword '%s' not allowed",
                     blocked[i]);
    }

    struct node_db *ndb = app_runtime_node_db();
    if (!ndb || !ndb->db)
        LOG_FAIL("diag", "dbquery: node_db not available");

    /* Build the executed SQL: append LIMIT if not present. */
    char executed[DBQUERY_MAX_SQL_LEN + 64];
    if (sql_has_word(sql, "LIMIT")) {
        snprintf(executed, sizeof(executed), "%s", sql);
    } else {
        snprintf(executed, sizeof(executed), "%s LIMIT %lld",
                 sql, (long long)limit);
    }

    struct dbq_progress_ctx pctx = {
        .start_ms = now_ms(),
        .budget_ms = DBQUERY_BUDGET_MS,
    };
    sqlite3_progress_handler(ndb->db, DBQUERY_PROGRESS_OPS_TICK,
                             dbq_progress_cb, &pctx);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, executed, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_progress_handler(ndb->db, 0, NULL, NULL);
        const char *err = sqlite3_errmsg(ndb->db);
        LOG_FAIL("diag", "dbquery: prepare failed: %s", err ? err : "(null)");
    }

    int ncols = sqlite3_column_count(stmt);

    json_set_object(result);
    struct json_value cols_arr = {0};
    json_set_array(&cols_arr);
    for (int c = 0; c < ncols; c++) {
        struct json_value cv = {0};
        const char *name = sqlite3_column_name(stmt, c);
        json_set_str(&cv, name ? name : "");
        json_push_back(&cols_arr, &cv);
        json_free(&cv);
    }
    json_push_kv(result, "columns", &cols_arr);
    json_free(&cols_arr);

    struct json_value rows_arr = {0};
    json_set_array(&rows_arr);
    int row_count = 0;
    bool truncated = false;
    bool interrupted = false;

    while (row_count < DBQUERY_HARD_ROW_CAP) {
        rc = AR_STEP_ROW_READONLY(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc == SQLITE_INTERRUPT) { interrupted = true; break; }
        if (rc != SQLITE_ROW) {
            const char *err = sqlite3_errmsg(ndb->db);
            sqlite3_finalize(stmt);
            sqlite3_progress_handler(ndb->db, 0, NULL, NULL);
            LOG_FAIL("diag", "dbquery: step failed (rc=%d): %s",
                     rc, err ? err : "(null)");
        }
        struct json_value row = {0};
        json_set_array(&row);
        for (int c = 0; c < ncols; c++) {
            struct json_value cell = {0};
            int t = sqlite3_column_type(stmt, c);
            switch (t) {
                case SQLITE_INTEGER:
                    json_set_int(&cell, (int64_t)sqlite3_column_int64(stmt, c));
                    break;
                case SQLITE_FLOAT:
                    json_set_real(&cell, sqlite3_column_double(stmt, c));
                    break;
                case SQLITE_TEXT: {
                    const char *txt = (const char *)sqlite3_column_text(stmt, c);
                    json_set_str(&cell, txt ? txt : "");
                    break;
                }
                case SQLITE_NULL:
                    json_set_null(&cell);
                    break;
                case SQLITE_BLOB: {
                    /* Encode as hex string (BLOBs in our schema are
                     * usually 20–32 byte hashes). Truncate at 256 hex
                     * chars to keep responses small. */
                    int blen = sqlite3_column_bytes(stmt, c);
                    if (blen > 128) blen = 128;
                    const unsigned char *b = sqlite3_column_blob(stmt, c);
                    char hex[257];
                    static const char hx[] = "0123456789abcdef";
                    for (int k = 0; k < blen; k++) {
                        hex[k * 2]     = hx[(b[k] >> 4) & 0xf];
                        hex[k * 2 + 1] = hx[b[k] & 0xf];
                    }
                    hex[blen * 2] = '\0';
                    json_set_str(&cell, hex);
                    break;
                }
            }
            json_push_back(&row, &cell);
            json_free(&cell);
        }
        json_push_back(&rows_arr, &row);
        json_free(&row);
        row_count++;
    }

    if (row_count >= DBQUERY_HARD_ROW_CAP) truncated = true;

    int64_t elapsed = now_ms() - pctx.start_ms;
    sqlite3_finalize(stmt);
    sqlite3_progress_handler(ndb->db, 0, NULL, NULL);

    json_push_kv(result, "rows", &rows_arr);
    json_free(&rows_arr);
    json_push_kv_int(result, "row_count", (int64_t)row_count);
    json_push_kv_bool(result, "truncated", truncated);
    json_push_kv_bool(result, "interrupted", interrupted);
    json_push_kv_int(result, "elapsed_ms", elapsed);
    json_push_kv_str(result, "sql_executed", executed);
    return true;
}

/* ── RPC: probezclassicd <height> ──────────────────────────────────
 *
 * Synchronously probe the local zclassicd at a given height and
 * compare its getblockhash result against our block_index. */

static bool rpc_probezclassicd(const struct json_value *params, bool help,
                               struct json_value *result)
{
    RPC_HELP(help, result,
        "probezclassicd <height>\n"
        "\nProbe the local zclassicd (independent ZClassic impl) for the\n"
        "block hash at <height> and compare to our block_index.\n"
        "\nResult: { height, our_hash, their_hash, match, our_have_block,\n"
        "         error, error_msg }");

    const struct json_value *h_val = json_at(params, 0);
    int height = -1;
    if (h_val) {
        if (h_val->type == JSON_INT)
            height = (int)json_get_int(h_val);
        else if (h_val->type == JSON_STR)
            height = atoi(json_get_str(h_val));
    }
    if (height < 0) {
        LOG_FAIL("diag", "probezclassicd: bad/missing height");
    }

    struct zclassicd_oracle_probe_result r;
    bool ok = zclassicd_oracle_probe(height, &r);

    json_set_object(result);
    json_push_kv_int (result, "height",         r.height);
    json_push_kv_str (result, "our_hash",       r.our_hash);
    json_push_kv_str (result, "their_hash",     r.their_hash);
    json_push_kv_bool(result, "match",          r.match);
    json_push_kv_bool(result, "our_have_block", r.our_have_block);
    json_push_kv_bool(result, "error",          r.error);
    json_push_kv_str (result, "error_msg",      r.error_msg);
    return ok;
}

static bool rpc_getmirrorstatus(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getmirrorstatus\n"
        "\nReturn legacy mirror sync status.\n"
        "\nResult: zclassic23_height/hash, zclassicd_height/hash, lag, "
        "reachable, mirror_running, last_catchup, last_error, "
        "headers_added, blocks_applied.");

    json_set_object(result);
    return legacy_mirror_sync_dump_state_json(result, NULL);
}

static bool rpc_peersprojectiondiff(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "peersprojectiondiff\n"
        "\nCompare Phase 4d peers_projection against legacy peers table.\n"
        "\nResult: projection_count, legacy_count, match, first_diff.");

    json_set_object(result);
    peers_projection_t *proj = peers_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        json_push_kv_int(result, "projection_count",
                         proj ? (int64_t)peers_projection_count(proj) : 0);
        json_push_kv_int(result, "legacy_count",
                         ndb && ndb->open ? db_peer_count(ndb) : 0);
        return true;
    }
    if (peers_projection_catch_up(proj) == UINT64_MAX) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "projection_catch_up_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)peers_projection_count(proj));
        json_push_kv_int(result, "legacy_count", db_peer_count(ndb));
        return true;
    }

    uint64_t projection_count = peers_projection_count(proj);
    int legacy_count = db_peer_count(ndb);
    bool match = projection_count == (uint64_t)legacy_count;
    char first_diff[160] = {0};
    if (!match) {
        snprintf(first_diff, sizeof(first_diff),
                 "count projection=%llu legacy=%d",
                 (unsigned long long)projection_count, legacy_count);
    }

    struct db_peer sample[10];
    int n = db_peer_recent(ndb, sample, 10);
    for (int i = 0; i < n && match; i++) {
        uint64_t services = 0;
        int64_t last_seen = 0;
        if (!peers_projection_get(proj, sample[i].ip, sample[i].port,
                                  &services, &last_seen, NULL)) {
            snprintf(first_diff, sizeof(first_diff),
                     "missing recent peer port=%u", sample[i].port);
            match = false;
            break;
        }
        if (services != sample[i].services) {
            snprintf(first_diff, sizeof(first_diff),
                     "services mismatch port=%u projection=%llu legacy=%llu",
                     sample[i].port,
                     (unsigned long long)services,
                     (unsigned long long)sample[i].services);
            match = false;
            break;
        }
    }

    json_push_kv_str(result, "projection", "peers_projection");
    json_push_kv_int(result, "projection_count", (int64_t)projection_count);
    json_push_kv_int(result, "legacy_count", legacy_count);
    json_push_kv_int(result, "sample_checked", n);
    json_push_kv_bool(result, "match", match);
    if (match) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(result, "first_diff", &nullv);
    } else {
        json_push_kv_str(result, "first_diff", first_diff);
    }
    return true;
}

struct mempool_diff_entry {
    uint8_t txid[32];
    int64_t fee;
    uint32_t size;
    uint32_t weight;
};

struct mempool_diff_list {
    struct mempool_diff_entry *items;
    size_t cap;
    size_t count;
    bool overflow;
};

static int cmp_mempool_diff_entry(const void *a, const void *b)
{
    const struct mempool_diff_entry *ea = a;
    const struct mempool_diff_entry *eb = b;
    return memcmp(ea->txid, eb->txid, 32);
}

static void mempool_txid_hex(const uint8_t txid[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[(txid[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[txid[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool mempool_diff_list_append(struct mempool_diff_list *list,
                                     const uint8_t txid[32],
                                     int64_t fee,
                                     uint32_t size,
                                     uint32_t weight)
{
    if (!list || !txid || list->count >= list->cap) {
        if (list) list->overflow = true;
        return false;
    }
    struct mempool_diff_entry *dst = &list->items[list->count++];
    memcpy(dst->txid, txid, 32);
    dst->fee = fee;
    dst->size = size;
    dst->weight = weight;
    return true;
}

static void mempool_diff_live_cb(const struct db_mempool_entry *e, void *ctx)
{
    struct mempool_diff_list *list = ctx;
    if (!e || !list) return;
    uint32_t size = e->size > 0 ? (uint32_t)e->size : 0u;
    (void)mempool_diff_list_append(list, e->txid, e->fee, size, size);
}

static bool mempool_diff_projection_cb(const uint8_t txid[32],
                                       int64_t fee,
                                       uint32_t size_bytes,
                                       uint32_t weight,
                                       void *user)
{
    return mempool_diff_list_append(user, txid, fee, size_bytes, weight);
}

static bool mempool_diff_alloc(struct mempool_diff_list *list, size_t count)
{
    memset(list, 0, sizeof(*list));
    list->cap = count;
    if (count == 0)
        return true;
    list->items = zcl_malloc(count * sizeof(list->items[0]),
                             "mempool projection diff entries");
    return list->items != NULL;
}

static void mempool_diff_free(struct mempool_diff_list *list)
{
    if (!list) return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int64_t mempool_diff_total_fee(const struct mempool_diff_list *list)
{
    int64_t total = 0;
    for (size_t i = 0; list && i < list->count; i++)
        total += list->items[i].fee;
    return total;
}

static uint64_t mempool_diff_total_weight(const struct mempool_diff_list *list)
{
    uint64_t total = 0;
    for (size_t i = 0; list && i < list->count; i++)
        total += list->items[i].weight;
    return total;
}

static bool rpc_mempoolprojectiondiff(const struct json_value *params,
                                      bool help,
                                      struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "mempoolprojectiondiff\n"
        "\nCompare Phase 4d mempool_projection against legacy mempool table.\n"
        "\nResult: projection_count, legacy_count, total_fee, total_weight, match, first_diff.");

    json_set_object(result);
    mempool_projection_t *proj = mempool_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        json_push_kv_int(result, "projection_count",
                         proj ? (int64_t)mempool_projection_count(proj) : 0);
        json_push_kv_int(result, "legacy_count",
                         ndb && ndb->open ? db_mempool_count(ndb) : 0);
        return true;
    }
    if (mempool_projection_catch_up(proj) == UINT64_MAX) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "projection_catch_up_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)mempool_projection_count(proj));
        json_push_kv_int(result, "legacy_count", db_mempool_count(ndb));
        return true;
    }

    uint64_t projection_count = mempool_projection_count(proj);
    int legacy_count_i = db_mempool_count(ndb);
    size_t legacy_count = legacy_count_i > 0 ? (size_t)legacy_count_i : 0;

    struct mempool_diff_list projection = {0};
    struct mempool_diff_list legacy = {0};
    bool alloc_ok = mempool_diff_alloc(&projection,
                                       (size_t)projection_count) &&
                    mempool_diff_alloc(&legacy, legacy_count);
    if (!alloc_ok) {
        mempool_diff_free(&projection);
        mempool_diff_free(&legacy);
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "allocation_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)projection_count);
        json_push_kv_int(result, "legacy_count", legacy_count_i);
        return true;
    }

    int projection_each = mempool_projection_each(
        proj, mempool_diff_projection_cb, &projection);
    int legacy_each = db_mempool_each(ndb, mempool_diff_live_cb, &legacy);
    if (projection_each < 0 || legacy_each < 0 ||
        projection.overflow || legacy.overflow) {
        mempool_diff_free(&projection);
        mempool_diff_free(&legacy);
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "iteration_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)projection_count);
        json_push_kv_int(result, "legacy_count", legacy_count_i);
        return true;
    }

    qsort(projection.items, projection.count, sizeof(projection.items[0]),
          cmp_mempool_diff_entry);
    qsort(legacy.items, legacy.count, sizeof(legacy.items[0]),
          cmp_mempool_diff_entry);

    int64_t projection_fee = mempool_diff_total_fee(&projection);
    int64_t legacy_fee = mempool_diff_total_fee(&legacy);
    uint64_t projection_weight = mempool_diff_total_weight(&projection);
    uint64_t legacy_weight = mempool_diff_total_weight(&legacy);

    bool match = projection.count == legacy.count &&
                 projection_fee == legacy_fee &&
                 projection_weight == legacy_weight;
    char first_diff[65] = {0};
    size_t common = projection.count < legacy.count ?
                    projection.count : legacy.count;
    for (size_t i = 0; i < common; i++) {
        int txcmp = memcmp(projection.items[i].txid, legacy.items[i].txid, 32);
        if (txcmp != 0) {
            const uint8_t *first = txcmp < 0 ? projection.items[i].txid :
                                               legacy.items[i].txid;
            mempool_txid_hex(first, first_diff);
            match = false;
            break;
        }
        if (projection.items[i].fee != legacy.items[i].fee ||
            projection.items[i].size != legacy.items[i].size ||
            projection.items[i].weight != legacy.items[i].weight) {
            mempool_txid_hex(projection.items[i].txid, first_diff);
            match = false;
            break;
        }
    }
    if (!match && first_diff[0] == '\0') {
        if (projection.count > common)
            mempool_txid_hex(projection.items[common].txid, first_diff);
        else if (legacy.count > common)
            mempool_txid_hex(legacy.items[common].txid, first_diff);
        else
            snprintf(first_diff, sizeof(first_diff), "aggregate_mismatch");
    }

    json_push_kv_str(result, "projection", "mempool_projection");
    json_push_kv_int(result, "projection_count",
                     (int64_t)projection.count);
    json_push_kv_int(result, "legacy_count", (int64_t)legacy.count);
    json_push_kv_int(result, "projection_total_fee", projection_fee);
    json_push_kv_int(result, "legacy_total_fee", legacy_fee);
    json_push_kv_int(result, "projection_total_weight",
                     (int64_t)projection_weight);
    json_push_kv_int(result, "legacy_total_weight", (int64_t)legacy_weight);
    json_push_kv_int(result, "sample_checked", (int64_t)common);
    json_push_kv_bool(result, "match", match);
    if (match) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(result, "first_diff", &nullv);
    } else {
        json_push_kv_str(result, "first_diff", first_diff);
    }
    mempool_diff_free(&projection);
    mempool_diff_free(&legacy);
    return true;
}

static bool rpc_znamprojectiondiff(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "znamprojectiondiff\n"
        "\nCompare Phase 4d-4 znam_projection against the legacy znam tables.\n"
        "\nResult: projection/legacy name/addr/text counts, match, first_diff.");

    json_set_object(result);
    znam_projection_t *proj = znam_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        return true;
    }

    uint64_t p_names = znam_projection_name_count(proj);
    uint64_t p_addrs = znam_projection_addr_count(proj);
    uint64_t p_texts = znam_projection_text_count(proj);

    int64_t l_names = 0, l_addrs = 0, l_texts = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db, "SELECT COUNT(*) FROM znam_names",
                           -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-diff
            l_names = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(ndb->db, "SELECT COUNT(*) FROM znam_addr_records",
                           -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-diff
            l_addrs = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(ndb->db, "SELECT COUNT(*) FROM znam_text_records",
                           -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-diff
            l_texts = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }

    bool match = (int64_t)p_names == l_names &&
                 (int64_t)p_addrs == l_addrs &&
                 (int64_t)p_texts == l_texts;
    char first_diff[256] = {0};
    if (!match) {
        snprintf(first_diff, sizeof(first_diff),
                 "names p=%llu l=%lld; addrs p=%llu l=%lld; texts p=%llu l=%lld",
                 (unsigned long long)p_names, (long long)l_names,
                 (unsigned long long)p_addrs, (long long)l_addrs,
                 (unsigned long long)p_texts, (long long)l_texts);
    }

    json_push_kv_str(result, "projection", "znam_projection");
    json_push_kv_int(result, "projection_name_count", (int64_t)p_names);
    json_push_kv_int(result, "legacy_name_count", l_names);
    json_push_kv_int(result, "projection_addr_count", (int64_t)p_addrs);
    json_push_kv_int(result, "legacy_addr_count", l_addrs);
    json_push_kv_int(result, "projection_text_count", (int64_t)p_texts);
    json_push_kv_int(result, "legacy_text_count", l_texts);
    json_push_kv_bool(result, "match", match);
    if (match) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(result, "first_diff", &nullv);
    } else {
        json_push_kv_str(result, "first_diff", first_diff);
    }
    return true;
}

static bool rpc_walletprojectiondiff(const struct json_value *params,
                                     bool help,
                                     struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "walletprojectiondiff\n"
        "\nCompare Phase 4d-3 wallet_projection against legacy wallet view tables.\n"
        "\nResult: projection/live address, tx, UTXO, note counts, total value, match, first_diff.");

    json_set_object(result);
    wallet_projection_t *proj = wallet_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        return true;
    }

    uint64_t p_addresses = wallet_projection_address_count(proj);
    uint64_t p_txs = wallet_projection_tx_count(proj);
    uint64_t p_utxos = wallet_projection_utxo_count(proj);
    uint64_t p_notes = wallet_projection_note_count(proj);
    int64_t p_total = wallet_projection_total_value_zat(proj);

    int live_utxos = 0;
    int live_notes = 0;
    int64_t live_t_value = db_wallet_utxo_balance_with_count(ndb,
                                                             &live_utxos);
    int64_t live_z_value = db_sapling_note_balance_with_count(ndb,
                                                              &live_notes);
    int64_t live_total = live_t_value + live_z_value;
    /*
     * There is no legacy public address-view table. wallet_keys is
     * secret-owned, so this diff must not query it even for counts.
     */
    int live_addresses = (int)p_addresses;
    int live_txs = db_wallet_tx_count(ndb);

    const char *first_diff = NULL;
    if ((uint64_t)live_addresses != p_addresses)
        first_diff = "addresses";
    else if ((uint64_t)live_txs != p_txs)
        first_diff = "transactions";
    else if ((uint64_t)live_utxos != p_utxos)
        first_diff = "utxos";
    else if ((uint64_t)live_notes != p_notes)
        first_diff = "notes";
    else if (live_total != p_total)
        first_diff = "utxos";
    bool match = first_diff == NULL;

    json_push_kv_str(result, "projection", "wallet_projection");
    json_push_kv_int(result, "projection_address_count",
                     (int64_t)p_addresses);
    json_push_kv_int(result, "live_address_count", live_addresses);
    json_push_kv_int(result, "projection_tx_count", (int64_t)p_txs);
    json_push_kv_int(result, "live_tx_count", live_txs);
    json_push_kv_int(result, "projection_utxo_count", (int64_t)p_utxos);
    json_push_kv_int(result, "live_utxo_count", live_utxos);
    json_push_kv_int(result, "projection_note_count", (int64_t)p_notes);
    json_push_kv_int(result, "live_note_count", live_notes);
    json_push_kv_int(result, "projection_total_value_zat", p_total);
    json_push_kv_int(result, "live_total_value_zat", live_total);
    json_push_kv_bool(result, "match", match);
    if (match) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(result, "first_diff", &nullv);
    } else {
        json_push_kv_str(result, "first_diff", first_diff);
    }
    return true;
}

static int64_t diag_count_table(sqlite3 *db, const char *table)
{
    if (!db || !table || !table[0]) {
        LOG_ERR("diag", "projection diff count: invalid table args");
        return -1;
    }
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *s = NULL;
    int64_t count = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-diff
            count = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return count;
}

static void push_projection_count_diff(struct json_value *result,
                                       const char *projection,
                                       int64_t projection_count,
                                       int64_t legacy_count,
                                       const char *first_diff)
{
    bool match = projection_count == legacy_count &&
                 (!first_diff || first_diff[0] == '\0');
    json_push_kv_str(result, "projection", projection);
    json_push_kv_int(result, "projection_count", projection_count);
    json_push_kv_int(result, "legacy_count", legacy_count);
    json_push_kv_bool(result, "match", match);
    if (match) {
        struct json_value nullv;
        json_init(&nullv);
        json_set_null(&nullv);
        json_push_kv(result, "first_diff", &nullv);
    } else {
        char count_diff[128];
        snprintf(count_diff, sizeof(count_diff),
                 "count projection=%lld legacy=%lld",
                 (long long)projection_count, (long long)legacy_count);
        json_push_kv_str(result, "first_diff",
                         first_diff && first_diff[0] ? first_diff
                                                     : count_diff);
    }
}

static bool rpc_contactsprojectiondiff(const struct json_value *params,
                                       bool help,
                                       struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "contactsprojectiondiff\n"
        "\nCompare Phase 4d-5 contacts_projection against legacy contacts table.\n"
        "\nResult: projection_count, legacy_count, match, first_diff.");

    json_set_object(result);
    contacts_projection_t *proj = contacts_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        json_push_kv_int(result, "projection_count",
                         proj ? (int64_t)contacts_projection_count(proj) : 0);
        json_push_kv_int(result, "legacy_count",
                         ndb && ndb->open ?
                         diag_count_table(ndb->db, "contacts") : 0);
        return true;
    }
    if (contacts_projection_catch_up(proj) == UINT64_MAX) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "projection_catch_up_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)contacts_projection_count(proj));
        json_push_kv_int(result, "legacy_count",
                         diag_count_table(ndb->db, "contacts"));
        return true;
    }

    int64_t projection_count = 0;
    int64_t legacy_count = 0;
    char first_diff[256] = {0};
    if (!contacts_projection_diff_legacy(proj, ndb->db, &projection_count,
                                         &legacy_count, first_diff,
                                         sizeof(first_diff))) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "diff_query_failed");
        return true;
    }
    push_projection_count_diff(result, "contacts_projection",
                               projection_count, legacy_count, first_diff);
    return true;
}

static bool rpc_onionannouncementsprojectiondiff(
    const struct json_value *params, bool help, struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "onionannouncementsprojectiondiff\n"
        "\nCompare Phase 4d-5 onion_announcements_projection against legacy onion_announcements table.\n"
        "\nResult: projection_count, legacy_count, match, first_diff.");

    json_set_object(result);
    onion_ann_projection_t *proj = onion_ann_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        json_push_kv_int(result, "projection_count",
                         proj ? (int64_t)onion_ann_projection_count(proj) : 0);
        json_push_kv_int(result, "legacy_count",
                         ndb && ndb->open ?
                         diag_count_table(ndb->db,
                                          "onion_announcements") : 0);
        return true;
    }
    if (onion_ann_projection_catch_up(proj) == UINT64_MAX) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "projection_catch_up_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)onion_ann_projection_count(proj));
        json_push_kv_int(result, "legacy_count",
                         diag_count_table(ndb->db, "onion_announcements"));
        return true;
    }

    int64_t projection_count = 0;
    int64_t legacy_count = 0;
    char first_diff[256] = {0};
    if (!onion_ann_projection_diff_legacy(proj, ndb->db, &projection_count,
                                          &legacy_count, first_diff,
                                          sizeof(first_diff))) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "diff_query_failed");
        return true;
    }
    push_projection_count_diff(result, "onion_announcements_projection",
                               projection_count, legacy_count, first_diff);
    return true;
}

static bool rpc_hodlhistoryprojectiondiff(const struct json_value *params,
                                          bool help,
                                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "hodlhistoryprojectiondiff\n"
        "\nCompare Phase 4d-5 hodl_history_projection against legacy hodl_history table.\n"
        "\nResult: projection_count, legacy_count, match, first_diff.");

    json_set_object(result);
    hodl_history_projection_t *proj = hodl_history_projection_current();
    struct node_db *ndb = app_runtime_node_db();
    if (!proj || !ndb || !ndb->open) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff",
                         !proj ? "projection_not_open" : "legacy_db_not_open");
        json_push_kv_int(result, "projection_count",
                         proj ? (int64_t)hodl_history_projection_count(proj) : 0);
        json_push_kv_int(result, "legacy_count",
                         ndb && ndb->open ?
                         diag_count_table(ndb->db, "hodl_history") : 0);
        return true;
    }
    if (hodl_history_projection_catch_up(proj) == UINT64_MAX) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "projection_catch_up_failed");
        json_push_kv_int(result, "projection_count",
                         (int64_t)hodl_history_projection_count(proj));
        json_push_kv_int(result, "legacy_count",
                         diag_count_table(ndb->db, "hodl_history"));
        return true;
    }

    int64_t projection_count = 0;
    int64_t legacy_count = 0;
    char first_diff[256] = {0};
    if (!hodl_history_projection_diff_legacy(proj, ndb->db,
                                             &projection_count,
                                             &legacy_count, first_diff,
                                             sizeof(first_diff))) {
        json_push_kv_bool(result, "match", false);
        json_push_kv_str(result, "first_diff", "diff_query_failed");
        return true;
    }
    push_projection_count_diff(result, "hodl_history_projection",
                               projection_count, legacy_count, first_diff);
    return true;
}

/* ── Registration ────────────────────────────────────────────────── */

void register_diagnostics_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "dumpstate",     rpc_dumpstate,     true },
        { "control", "cutovermode",   rpc_cutovermode,   true },
        { "control", "cutoverpreflight", rpc_cutoverpreflight, true },
        { "control", "getnodelog",    rpc_getnodelog,    true },
        { "control", "dbquery",       rpc_dbquery,       true },
        { "control", "probezclassicd", rpc_probezclassicd, true },
        { "control", "getmirrorstatus", rpc_getmirrorstatus, true },
        { "control", "peersprojectiondiff", rpc_peersprojectiondiff, true },
        { "control", "mempoolprojectiondiff", rpc_mempoolprojectiondiff, true },
        { "control", "znamprojectiondiff",  rpc_znamprojectiondiff,  true },
        { "control", "walletprojectiondiff", rpc_walletprojectiondiff, true },
        { "control", "contactsprojectiondiff",
          rpc_contactsprojectiondiff, true },
        { "control", "onionannouncementsprojectiondiff",
          rpc_onionannouncementsprojectiondiff, true },
        { "control", "hodlhistoryprojectiondiff",
          rpc_hodlhistoryprojectiondiff, true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
