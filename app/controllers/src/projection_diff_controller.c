/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Projection-diff controller — Phase-4 shadow-vs-legacy parity tools.
 *
 *   getmirrorstatus                       — legacy mirror sync status
 *   peersprojectiondiff                   — peers projection vs legacy
 *   mempoolprojectiondiff                 — mempool projection vs legacy
 *   znamprojectiondiff                    — znam projection vs legacy
 *   walletprojectiondiff                  — wallet view projection vs live
 *   contactsprojectiondiff                — contacts projection vs legacy
 *   onionannouncementsprojectiondiff      — onion ann projection vs legacy
 *   hodlhistoryprojectiondiff             — HODL history projection vs legacy
 *
 * Each diff folds a projection over the event_log and compares it to the
 * legacy SQLite table it shadows, so a projection can be proven
 * byte-identical before the cutover flips authority to the log.
 */

#include "controllers/diagnostics_internal.h"

#include "json/json.h"
#include "rpc/server.h"
#include "controllers/strong_params.h"
#include "services/legacy_mirror_sync_service.h"
#include "storage/mempool_projection.h"
#include "storage/peers_projection.h"
#include "storage/small_projections.h"
#include "storage/znam_projection.h"
#include "storage/wallet_projection.h"
#include "models/database.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include "models/wallet_tx.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool diag_rpc_getmirrorstatus(const struct json_value *params, bool help,
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

bool diag_rpc_peersprojectiondiff(const struct json_value *params, bool help,
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

bool diag_rpc_mempoolprojectiondiff(const struct json_value *params,
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

bool diag_rpc_znamprojectiondiff(const struct json_value *params, bool help,
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

bool diag_rpc_walletprojectiondiff(const struct json_value *params,
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

bool diag_rpc_contactsprojectiondiff(const struct json_value *params,
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

bool diag_rpc_onionannouncementsprojectiondiff(
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

bool diag_rpc_hodlhistoryprojectiondiff(const struct json_value *params,
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
