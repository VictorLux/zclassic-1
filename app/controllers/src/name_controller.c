/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Names RPC controller.
 *
 * Commands:
 *   name_register  — register a name on-chain via OP_RETURN
 *   name_resolve   — look up a name's target
 *   name_list      — list all registered names */

#include "znam/znam.h"
#include "json/json.h"
#include "rpc/server.h"
#include "models/database.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* ── Context ────────────────────────────────────────────────────── */

static struct node_db *g_name_ndb = NULL;

void rpc_name_set_state(struct node_db *ndb)
{
    g_name_ndb = ndb;
}

/* ── Helper ─────────────────────────────────────────────────────── */

static void hash_to_hex(const uint8_t hash[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case ZNAM_TYPE_ONION: return "onion";
    case ZNAM_TYPE_ZADDR: return "z-address";
    case ZNAM_TYPE_TADDR: return "t-address";
    default: return "unknown";
    }
}

static uint8_t parse_type(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "onion") == 0) return ZNAM_TYPE_ONION;
    if (strcmp(s, "zaddr") == 0 || strcmp(s, "z-address") == 0)
        return ZNAM_TYPE_ZADDR;
    if (strcmp(s, "taddr") == 0 || strcmp(s, "t-address") == 0)
        return ZNAM_TYPE_TADDR;
    return 0;
}

static void entry_to_json(const struct znam_entry *e, struct json_value *obj)
{
    json_set_object(obj);
    json_push_kv_str(obj, "name", e->name);
    json_push_kv_str(obj, "owner", e->owner_address);
    json_push_kv_str(obj, "type", type_name(e->target_type));
    json_push_kv_str(obj, "value", e->target_value);
    json_push_kv_int(obj, "reg_height", e->reg_height);
    char hex[65];
    hash_to_hex(e->reg_txid, hex);
    json_push_kv_str(obj, "reg_txid", hex);
}

/* ── name_resolve ───────────────────────────────────────────────── */

static bool rpc_name_resolve(const struct json_value *params, bool help,
                             struct json_value *result)
{
    if (help || !params || json_size(params) < 1) {
        json_set_str(result,
            "name_resolve \"name\"\n"
            "\nResolve a ZCL Name to its target (.onion, z-addr, t-addr).\n"
            "\nArguments:\n"
            "1. name (string, required) The name to resolve\n"
            "\nResult: the name entry or null.\n");
        return true;
    }

    const struct json_value *arg0 = json_at(params, 0);
    const char *name = arg0 ? json_get_str(arg0) : NULL;
    if (!name) {
        json_set_str(result, "name required");
        return false;
    }

    struct znam_entry entry;
    if (!g_name_ndb || !db_znam_find(g_name_ndb, name, &entry)) {
        json_set_str(result, "Name not found");
        return true;
    }

    entry_to_json(&entry, result);
    return true;
}

/* ── name_list ──────────────────────────────────────────────────── */

static bool rpc_name_list(const struct json_value *params, bool help,
                          struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "name_list [\"owner_address\"]\n"
            "\nList registered ZCL Names, optionally filtered by owner.\n");
        return true;
    }

    json_set_array(result);
    if (!g_name_ndb) return true;

    struct znam_entry entries[100];
    int count;

    const struct json_value *arg0 = params ? json_at(params, 0) : NULL;
    const char *owner = arg0 ? json_get_str(arg0) : NULL;

    if (owner && owner[0])
        count = db_znam_list_by_owner(g_name_ndb, owner, entries, 100);
    else
        count = db_znam_list(g_name_ndb, entries, 100);

    for (int i = 0; i < count; i++) {
        struct json_value e = {0};
        entry_to_json(&entries[i], &e);
        json_push_back(result, &e);
        json_free(&e);
    }

    return true;
}

/* ── name_register ──────────────────────────────────────────────── */

static bool rpc_name_register(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || !params || json_size(params) < 3) {
        json_set_str(result,
            "name_register \"name\" \"type\" \"value\"\n"
            "\nRegister a ZCL Name on-chain via OP_RETURN transaction.\n"
            "\nArguments:\n"
            "1. name  (string) Name to register (1-63 chars, lowercase+hyphens)\n"
            "2. type  (string) Target type: onion, zaddr, taddr\n"
            "3. value (string) Target value (.onion address, z-addr, t-addr)\n"
            "\nNote: Requires wallet to create and broadcast the transaction.\n"
            "For now, returns the OP_RETURN hex that needs to be included\n"
            "in a transaction's first output.\n");
        return true;
    }

    const char *name = json_get_str(json_at(params, 0));
    const char *type_str = json_get_str(json_at(params, 1));
    const char *value = json_get_str(json_at(params, 2));

    if (!name || !type_str || !value) {
        json_set_str(result, "Missing arguments");
        return false;
    }

    if (!znam_validate_name(name)) {
        json_set_str(result, "Invalid name (1-63 chars, lowercase alphanumeric + hyphens)");
        return false;
    }

    uint8_t target_type = parse_type(type_str);
    if (target_type == 0) {
        json_set_str(result, "Invalid type (use: onion, zaddr, taddr)");
        return false;
    }

    /* Check if name already exists */
    struct znam_entry existing;
    if (g_name_ndb && db_znam_find(g_name_ndb, name, &existing)) {
        json_set_str(result, "Name already registered");
        return false;
    }

    /* Build the OP_RETURN script */
    uint8_t script[512];
    size_t script_len = znam_build_register(script, sizeof(script),
                                            name, target_type, value);
    if (script_len == 0) {
        json_set_str(result, "Failed to build OP_RETURN script");
        return false;
    }

    /* Return the script hex for inclusion in a transaction */
    json_set_object(result);
    json_push_kv_str(result, "name", name);
    json_push_kv_str(result, "type", type_name(target_type));
    json_push_kv_str(result, "value", value);

    char hex[1025];
    for (size_t i = 0; i < script_len && i < 512; i++)
        sprintf(hex + i * 2, "%02x", script[i]);
    hex[script_len * 2] = '\0';
    json_push_kv_str(result, "op_return_hex", hex);
    json_push_kv_int(result, "op_return_size", (int64_t)script_len);
    json_push_kv_str(result, "status", "ready");
    json_push_kv_str(result, "note",
        "Include this OP_RETURN as vout[0] in a transaction to register the name. "
        "The sender address becomes the owner.");

    return true;
}

/* ── REST API ───────────────────────────────────────────────────── */

bool api_name_list(struct json_value *result)
{
    return rpc_name_list(NULL, false, result);
}

/* ── Registration ───────────────────────────────────────────────── */

void register_name_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "names", "name_register", rpc_name_register, true },
        { "names", "name_resolve",  rpc_name_resolve,  true },
        { "names", "name_list",     rpc_name_list,     true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
