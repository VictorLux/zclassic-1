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
#include "chain/chain.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "json/json.h"
#include "rpc/server.h"
#include "controllers/strong_params.h"
#include "services/sync_watchdog_service.h"
#include "services/chain_restore_service.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <ctype.h>
#include <regex.h>
#include <string.h>
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
    { "boot",        chain_restore_dump_state_json,
                     "last boot's integrity check + nbits-backfill counters" },
    { "block_index", block_index_dump_state_json,
                     "block_index entry by height or hash (in `key`)" },
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

/* ── Registration ────────────────────────────────────────────────── */

void register_diagnostics_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "dumpstate", rpc_dumpstate, true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
