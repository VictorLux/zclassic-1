/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/misc_controller.h"
#include "controllers/strong_params.h"
#include "event/event.h"
#include "net/download.h"
#include "validation/contextual_check_tx.h"
#include "controllers/wallet_helpers.h"
#include "coins/coins_view.h"
#include "chain/chainparams.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "net/version.h"
#include "util/clientversion.h"
#include "validation/chainstate.h"
#include "wallet/keystore.h"
#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include <stdlib.h>
#include <string.h>

static struct main_state *g_ms = NULL;
static struct wallet *g_misc_wallet = NULL;

void rpc_misc_set_state(struct main_state *ms)
{
    g_ms = ms;
}

void rpc_misc_set_wallet(struct wallet *w)
{
    g_misc_wallet = w;
}

static bool rpc_getinfo(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getinfo\n"
        "Returns an object containing various state info.");

    json_set_object(result);
    json_push_kv_int(result, "version", CLIENT_VERSION);
    json_push_kv_int(result, "protocolversion", PROTOCOL_VERSION);

    struct block_index *tip = g_ms ?
        active_chain_tip(&g_ms->chain_active) : NULL;
    json_push_kv_int(result, "blocks", tip ? tip->nHeight : 0);
    json_push_kv_int(result, "timeoffset", 0);
    json_push_kv_int(result, "connections", 0);
    json_push_kv_real(result, "difficulty", 0.0);
    json_push_kv_bool(result, "testnet",
                       strcmp(chain_params_get()->strNetworkID, "test") == 0);
    json_push_kv_real(result, "relayfee", 0.00000100);
    json_push_kv_int(result, "errors", 0);

    return true;
}

static bool rpc_validateaddress(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    RPC_HELP(help, result,
        "validateaddress \"address\"\n"
        "Return information about the given ZClassic address.\n"
        "Works for transparent (t1/t3) and shielded (zs1) addresses.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *addr = rpc_require_str(&p, 0, "address");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }
    const struct chain_params *cp = chain_params_get();

    json_set_object(result);
    json_push_kv_str(result, "address", addr);

    /* Try Sapling z-address (zs1...) */
    uint8_t diversifier[11];
    uint8_t pk_d[32];
    if (sapling_decode_payment_address(addr, diversifier, pk_d)) {
        json_push_kv_bool(result, "isvalid", true);
        json_push_kv_str(result, "type", "sapling");

        if (g_misc_wallet) {
            bool is_mine = false;
            for (size_t i = 0; i < g_misc_wallet->sapling_keys.num_keys; i++) {
                struct sapling_key_entry *e =
                    &g_misc_wallet->sapling_keys.keys[i];
                if (e->used &&
                    memcmp(e->diversifier, diversifier, 11) == 0 &&
                    memcmp(e->pk_d, pk_d, 32) == 0) {
                    is_mine = true;
                    break;
                }
            }
            json_push_kv_bool(result, "ismine", is_mine);
        }
        return true;
    }

    /* Try transparent address (t1/t3) */
    size_t pk_len, sc_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_len);

    struct tx_destination dest;
    bool valid = decode_destination(addr, pk_pfx, pk_len,
                                     sc_pfx, sc_len, &dest);

    json_push_kv_bool(result, "isvalid", valid);
    if (!valid)
        return true;

    if (dest.type == DEST_KEY_ID) {
        json_push_kv_str(result, "type", "pubkeyhash");
        json_push_kv_bool(result, "isscript", false);

        if (g_misc_wallet) {
            bool is_mine = keystore_have_key(&g_misc_wallet->keystore,
                                               &dest.id.key);
            json_push_kv_bool(result, "ismine", is_mine);

            if (is_mine) {
                struct pubkey pk;
                if (keystore_get_pubkey(&g_misc_wallet->keystore,
                                         &dest.id.key, &pk)) {
                    char pk_hex[PUBLIC_KEY_SIZE * 2 + 1];
                    HexStr(pk.vch, pk.size, false, pk_hex, sizeof(pk_hex));
                    json_push_kv_str(result, "pubkey", pk_hex);
                    json_push_kv_bool(result, "iscompressed",
                                      pk.size == COMPRESSED_PUBLIC_KEY_SIZE);
                }
            }
        }
    } else if (dest.type == DEST_SCRIPT_ID) {
        json_push_kv_str(result, "type", "scripthash");
        json_push_kv_bool(result, "isscript", true);

        if (g_misc_wallet) {
            bool have = keystore_have_cscript(&g_misc_wallet->keystore,
                                                &dest.id.script.hash);
            json_push_kv_bool(result, "ismine", have);
        }
    }

    return true;
}

static bool rpc_stop(const struct json_value *params, bool help,
                       struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "stop\n"
        "Stop ZClassic server.");

    json_set_str(result, "ZClassic server stopping");
    exit(0);
    return true;
}

static bool rpc_downloadstats(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "downloadstats\n"
        "\nReturn block download manager statistics.\n"
        "\nResult:\n"
        "  { \"requested\", \"received\", \"timed_out\", "
        "\"in_flight\", \"queued\", \"sync_state\" }\n");

    struct download_manager *dm = msg_get_download_mgr();

    uint64_t req = 0, recv = 0, tout = 0, inflight = 0, queued = 0;
    dl_get_stats(dm, &req, &recv, &tout, &inflight, &queued);

    json_set_object(result);
    json_push_kv_int(result, "requested", (int64_t)req);
    json_push_kv_int(result, "received", (int64_t)recv);
    json_push_kv_int(result, "timed_out", (int64_t)tout);
    json_push_kv_int(result, "in_flight", (int64_t)inflight);
    json_push_kv_int(result, "queued", (int64_t)queued);
    json_push_kv_str(result, "sync_state", sync_state_name(sync_get_state()));
    json_push_kv_int(result, "assume_valid_height",
                      (int64_t)g_assume_valid_height);
    return true;
}

static bool rpc_coinsinfo(const struct json_value *params, bool help,
                           struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "coinsinfo\n"
        "\nReturn UTXO cache diagnostics.\n");

    if (!g_coins_tip) {
        json_set_str(result, "coins_tip not initialized");
        return true;
    }
    struct coins_view_cache *tip = g_coins_tip;
    json_set_object(result);
    json_push_kv_int(result, "cache_size",
                      (int64_t)tip->cache_coins.size);
    json_push_kv_int(result, "cache_buckets",
                      (int64_t)tip->cache_coins.num_buckets);

    struct uint256 best;
    coins_view_cache_get_best_block(tip, &best);
    char hex[65];
    uint256_get_hex(&best, hex);
    json_push_kv_str(result, "best_block", hex);

    return true;
}

void register_misc_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "getinfo",          rpc_getinfo,          true },
        { "util",    "validateaddress",  rpc_validateaddress,  true },
        { "control", "stop",             rpc_stop,             true },
        { "control", "downloadstats",    rpc_downloadstats,    true },
        { "control", "coinsinfo",        rpc_coinsinfo,        true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
