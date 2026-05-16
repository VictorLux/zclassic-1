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

#include "controllers/diagnostics_controller.h"

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
#include "services/sync_watchdog_service.h"
#include "services/chain_restore_service.h"
#include "services/local_chain_ingest.h"
#include "services/zclassicd_oracle_service.h"
#include "services/header_probe_service.h"
#include "services/oracle_policy.h"
#include "services/quorum_oracle_service.h"
#include "services/rolling_anchor_service.h"
#include "services/block_index_integrity.h"
#include "services/chain_evidence_controller.h"
#include "health/heartbeat.h"
#include "models/database.h"
#include "config/runtime.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <ctype.h>
#include <regex.h>
#include <string.h>
#include <strings.h>
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

void diagnostics_controller_set_state(struct main_state *ms,
                                      const char *datadir)
{
    g_diag.main_state = ms;
    if (datadir) {
        snprintf(g_diag.datadir, sizeof(g_diag.datadir), "%s", datadir);
    }
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
    if (strlen(key) != 64) return NULL;
    for (const char *c = key; *c; c++) {
        if (!((*c >= '0' && *c <= '9') ||
              (*c >= 'a' && *c <= 'f') ||
              (*c >= 'A' && *c <= 'F'))) return NULL;
    }
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
    struct chain_evidence_controller authority;
    struct chain_evidence_controller_view view;

    chain_evidence_controller_init(&authority, app_runtime_node_db(), csr_instance());
    chain_evidence_controller_snapshot(&authority, &view);

    json_push_kv_str(out, "sync_state",
                     chain_evidence_controller_state_name(view.state));
    json_push_kv_int(out, "active_tip",
                     (int64_t)view.active_tip_height);
    json_push_kv_int(out, "header_tip",
                     (int64_t)view.header_tip_height);
    json_push_kv_int(out, "snapshot_anchor",
                     (int64_t)view.snapshot_anchor_height);
    json_push_kv_int(out, "utxo_max_height",
                     (int64_t)view.utxo_max_height);
    json_push_kv_int(out, "coins_best_block_height",
                     (int64_t)view.coins_best_block_height);
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
    { "watchdog",    sync_watchdog_dump_state_json,
                     "sync watchdog status + stats" },
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
    { "local_ingest", local_chain_ingest_dump_state_json,
                     "local chain ingest: phase/result/blocks/UTXOs from co-located zclassicd" },
    { "header_probe", header_probe_dump_state_json,
                     "header probe: bulk header pull from co-located zclassicd via JSON-RPC" },
    { "oracle_policy", oracle_policy_dump_state_json,
                     "oracle policy: disagreement state machine (NORMAL / HALTED / PANIC)" },
    { "rolling_anchor", rolling_anchor_dump_state_json,
                     "rolling SHA3 anchor extension: runtime windows past compile-time prefix" },
    { "quorum_oracle", quorum_oracle_dump_state_json,
                     "multi-source quorum oracle: per-source vote stats + last verdict" },
};

static bool rpc_dumpstate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result,
        "dumpstate <subsystem> [key]\n"
        "\nDump in-process state for a subsystem. Subsystems:\n"
        "  watchdog     — sync watchdog status + stats\n"
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
    for (size_t i = 0; i < sizeof(g_dumpers) / sizeof(g_dumpers[0]); i++) {
        if (strcmp(g_dumpers[i].name, sub) == 0) {
            e = &g_dumpers[i];
            break;
        }
    }
    if (!e) {
        LOG_FAIL("diag",
                 "dumpstate: unknown subsystem '%s' (try watchdog/boot/block_index)",
                 sub);
    }

    json_set_object(result);
    json_push_kv_str(result, "subsystem", e->name);
    json_push_kv_str(result, "description", e->desc);
    json_push_kv_int(result, "captured_at", (int64_t)time(NULL));

    struct json_value state = {0};
    json_set_object(&state);
    bool ok = e->fn(&state, key);
    if (!ok) {
        json_free(&state);
        LOG_FAIL("diag", "dumpstate: %s dump function returned false", sub);
    }
    json_push_kv(result, "state", &state);
    json_free(&state);
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

    int64_t now = (int64_t)time(NULL);
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
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

/* ── Registration ────────────────────────────────────────────────── */

void register_diagnostics_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "dumpstate",     rpc_dumpstate,     true },
        { "control", "getnodelog",    rpc_getnodelog,    true },
        { "control", "dbquery",       rpc_dbquery,       true },
        { "control", "probezclassicd", rpc_probezclassicd, true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
