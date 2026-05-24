/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/wallet_projection.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WP_CHECK(label, cond) do { \
    bool _ok = (cond); \
    printf("wallet_projection: %s... %s\n", (label), _ok ? "OK" : "FAIL"); \
    if (!_ok) failures++; \
} while (0)

static void fill_seq(uint8_t *dst, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++)
        dst[i] = (uint8_t)(seed + (uint8_t)i);
}

static void wp_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/wallet_projection_%d_%s",
             (int)getpid(), tag);
    test_cleanup_tmpdir(buf);
    mkdir("test-tmp", 0755);
    mkdir(buf, 0755);
}

static void wp_paths(const char *dir, char *elog, size_t elog_n,
                     char *proj, size_t proj_n)
{
    snprintf(elog, elog_n, "%s/event_log.dat", dir);
    snprintf(proj, proj_n, "%s/wallet_projection.db", dir);
}

static bool table_exists(const char *db_path, const char *name)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    bool found = false;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto done;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            -1, &s, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    found = sqlite3_step(s) == SQLITE_ROW;
done:
    if (s) sqlite3_finalize(s);
    if (db) sqlite3_close(db);
    return found;
}

static bool exec_projection_sql(const char *db_path, const char *sql)
{
    sqlite3 *db = NULL;
    char *err = NULL;
    bool ok = false;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) !=
        SQLITE_OK)
        goto done;
    ok = sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK;
done:
    if (err) sqlite3_free(err);
    if (db) sqlite3_close(db);
    return ok;
}

static bool append_wallet_key(event_log_t *log, int idx)
{
    char address[64];
    char label[32];
    uint8_t payload[EV_WALLET_PAYLOAD_MAX];
    size_t len = 0;
    struct ev_wallet_key_add ev;
    memset(&ev, 0, sizeof(ev));
    fill_seq(ev.pubkey_hash, sizeof(ev.pubkey_hash), (uint8_t)(0x10 + idx));
    snprintf(address, sizeof(address), "t1WalletPublic%03d", idx);
    snprintf(label, sizeof(label), "label-%03d", idx);
    ev.created_unix = 1770000000u + (uint32_t)idx;
    ev.address = address;
    ev.address_len = (uint8_t)strlen(address);
    ev.label = label;
    ev.label_len = (uint8_t)strlen(label);
    if (!ev_wallet_key_add_serialize(&ev, payload, sizeof(payload), &len))
        return false;
    return event_log_append(log, EV_WALLET_KEY_ADD, payload, len) !=
           UINT64_MAX;
}

static bool append_wallet_addr_derived(event_log_t *log, int idx)
{
    uint8_t payload[EV_WALLET_ADDR_DERIVED_LEN];
    struct ev_wallet_addr_derived ev;
    memset(&ev, 0, sizeof(ev));
    fill_seq(ev.pubkey_hash, sizeof(ev.pubkey_hash), (uint8_t)(0x20 + idx));
    fill_seq(ev.derived_pubkey_hash, sizeof(ev.derived_pubkey_hash),
             (uint8_t)(0xd0 + idx));
    ev.derivation_index = (uint32_t)idx;
    ev.derived_unix = 1770001000u + (uint32_t)idx;
    if (!ev_wallet_addr_derived_serialize(&ev, payload))
        return false;
    return event_log_append(log, EV_WALLET_ADDR_DERIVED, payload,
                            sizeof(payload)) != UINT64_MAX;
}

static bool append_wallet_tx(event_log_t *log, int idx)
{
    uint8_t payload[EV_WALLET_TX_SEEN_LEN];
    struct ev_wallet_tx_seen ev;
    memset(&ev, 0, sizeof(ev));
    fill_seq(ev.txid, sizeof(ev.txid), (uint8_t)(0x40 + idx));
    ev.block_height = 100000 + idx;
    ev.fee = 1000 + idx;
    ev.from_me = (uint8_t)(idx % 2);
    if (!ev_wallet_tx_seen_serialize(&ev, payload))
        return false;
    return event_log_append(log, EV_WALLET_TX_SEEN, payload,
                            sizeof(payload)) != UINT64_MAX;
}

static bool append_wallet_note(event_log_t *log, int idx)
{
    uint8_t payload[EV_WALLET_NOTE_DECRYPTED_LEN];
    struct ev_wallet_note_decrypted ev;
    memset(&ev, 0, sizeof(ev));
    fill_seq(ev.txid, sizeof(ev.txid), (uint8_t)(0x80 + idx));
    fill_seq(ev.cm, sizeof(ev.cm), (uint8_t)(0xa0 + idx));
    ev.output_index = (uint32_t)idx;
    ev.block_height = 200000 + idx;
    ev.value = 1 + idx;
    if (!ev_wallet_note_decrypted_serialize(&ev, payload))
        return false;
    return event_log_append(log, EV_WALLET_NOTE_DECRYPTED, payload,
                            sizeof(payload)) != UINT64_MAX;
}

static int t_wallet_event_ids(void)
{
    int failures = 0;
    WP_CHECK("key add id is stable", EV_WALLET_KEY_ADD == 9);
    WP_CHECK("tx seen id is stable", EV_WALLET_TX_SEEN == 10);
    WP_CHECK("addr derived id is allocated", EV_WALLET_ADDR_DERIVED == 17);
    WP_CHECK("note decrypted id is allocated",
             EV_WALLET_NOTE_DECRYPTED == 18);
    return failures;
}

static int t_key_add_payload_roundtrip(void)
{
    int failures = 0;
    static const char address[] = "t1WalletPublicAddressFixture";
    static const char label[] = "savings";
    uint8_t payload[EV_WALLET_PAYLOAD_MAX];
    size_t len = 0;
    struct ev_wallet_key_add in, out;
    memset(&in, 0, sizeof(in));
    fill_seq(in.pubkey_hash, sizeof(in.pubkey_hash), 0x10);
    in.created_unix = 1770000000u;
    in.address = address;
    in.address_len = (uint8_t)strlen(address);
    in.label = label;
    in.label_len = (uint8_t)strlen(label);

    WP_CHECK("key add serialize",
             ev_wallet_key_add_serialize(&in, payload, sizeof(payload),
                                         &len));
    WP_CHECK("key add public payload cap", len <= EV_WALLET_PAYLOAD_MAX);
    WP_CHECK("key add parse", ev_wallet_key_add_parse(payload, len, &out));
    WP_CHECK("key add roundtrip",
             memcmp(in.pubkey_hash, out.pubkey_hash, 20) == 0 &&
             out.created_unix == in.created_unix &&
             out.address_len == in.address_len &&
             out.label_len == in.label_len &&
             memcmp(out.address, address, in.address_len) == 0 &&
             memcmp(out.label, label, in.label_len) == 0);

    char too_long_label[EV_WALLET_LABEL_MAX + 1];
    memset(too_long_label, 'x', sizeof(too_long_label));
    in.label = too_long_label;
    in.label_len = (uint8_t)sizeof(too_long_label);
    WP_CHECK("key add rejects oversized public label",
             !ev_wallet_key_add_serialize(&in, payload, sizeof(payload),
                                          &len));
    WP_CHECK("key add rejects trailing bytes",
             !ev_wallet_key_add_parse(payload, len + 1, &out));
    return failures;
}

static int t_addr_derived_payload_roundtrip(void)
{
    int failures = 0;
    uint8_t payload[EV_WALLET_ADDR_DERIVED_LEN];
    struct ev_wallet_addr_derived in, out;
    memset(&in, 0, sizeof(in));
    fill_seq(in.pubkey_hash, sizeof(in.pubkey_hash), 0x30);
    fill_seq(in.derived_pubkey_hash, sizeof(in.derived_pubkey_hash), 0x50);
    in.derivation_index = 2147483647u;
    in.derived_unix = 1770000100u;

    WP_CHECK("addr derived serialize",
             ev_wallet_addr_derived_serialize(&in, payload));
    WP_CHECK("addr derived public payload cap",
             sizeof(payload) <= EV_WALLET_PAYLOAD_MAX);
    WP_CHECK("addr derived parse",
             ev_wallet_addr_derived_parse(payload, sizeof(payload), &out));
    WP_CHECK("addr derived roundtrip",
             memcmp(in.pubkey_hash, out.pubkey_hash, 20) == 0 &&
             memcmp(in.derived_pubkey_hash, out.derived_pubkey_hash, 20) == 0 &&
             out.derivation_index == in.derivation_index &&
             out.derived_unix == in.derived_unix);
    WP_CHECK("addr derived rejects wrong length",
             !ev_wallet_addr_derived_parse(payload, sizeof(payload) - 1,
                                           &out));
    return failures;
}

static int t_tx_seen_payload_roundtrip(void)
{
    int failures = 0;
    uint8_t payload[EV_WALLET_TX_SEEN_LEN];
    struct ev_wallet_tx_seen in, out;
    memset(&in, 0, sizeof(in));
    fill_seq(in.txid, sizeof(in.txid), 0x70);
    in.block_height = -1;
    in.fee = -12000;
    in.from_me = 1;

    WP_CHECK("tx seen serialize", ev_wallet_tx_seen_serialize(&in, payload));
    WP_CHECK("tx seen public payload cap",
             sizeof(payload) <= EV_WALLET_PAYLOAD_MAX);
    WP_CHECK("tx seen parse",
             ev_wallet_tx_seen_parse(payload, sizeof(payload), &out));
    WP_CHECK("tx seen roundtrip",
             memcmp(in.txid, out.txid, 32) == 0 &&
             out.block_height == in.block_height &&
             out.fee == in.fee &&
             out.from_me == 1);
    WP_CHECK("tx seen rejects wrong length",
             !ev_wallet_tx_seen_parse(payload, sizeof(payload) + 1, &out));
    return failures;
}

static int t_note_decrypted_payload_roundtrip(void)
{
    int failures = 0;
    uint8_t payload[EV_WALLET_NOTE_DECRYPTED_LEN];
    struct ev_wallet_note_decrypted in, out;
    memset(&in, 0, sizeof(in));
    fill_seq(in.txid, sizeof(in.txid), 0x90);
    fill_seq(in.cm, sizeof(in.cm), 0xb0);
    in.output_index = 2;
    in.block_height = 345678;
    in.value = 2500000000LL;

    WP_CHECK("note decrypted serialize",
             ev_wallet_note_decrypted_serialize(&in, payload));
    WP_CHECK("note decrypted public payload cap",
             sizeof(payload) <= EV_WALLET_PAYLOAD_MAX);
    WP_CHECK("note decrypted parse",
             ev_wallet_note_decrypted_parse(payload, sizeof(payload), &out));
    WP_CHECK("note decrypted roundtrip",
             memcmp(in.txid, out.txid, 32) == 0 &&
             out.output_index == in.output_index &&
             out.block_height == in.block_height &&
             out.value == in.value &&
             memcmp(in.cm, out.cm, 32) == 0);
    WP_CHECK("note decrypted rejects wrong length",
             !ev_wallet_note_decrypted_parse(payload, sizeof(payload) - 1,
                                             &out));
    return failures;
}

static int t_projection_skeleton_open_reopen(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    wp_tmpdir(dir, sizeof(dir), "skeleton");
    wp_paths(dir, elog_path, sizeof(elog_path), proj_path, sizeof(proj_path));

    event_log_t *log = event_log_open(elog_path);
    wallet_projection_t *p = wallet_projection_open(proj_path, log);
    WP_CHECK("skeleton open", log && p);
    WP_CHECK("current pointer set", wallet_projection_current() == p);
    wallet_projection_set_event_log(log);
    WP_CHECK("event log global set", wallet_projection_event_log() == log);
    WP_CHECK("fresh address count", wallet_projection_address_count(p) == 0);
    WP_CHECK("fresh tx count", wallet_projection_tx_count(p) == 0);
    WP_CHECK("fresh utxo count", wallet_projection_utxo_count(p) == 0);
    WP_CHECK("fresh note count", wallet_projection_note_count(p) == 0);
    WP_CHECK("fresh total value", wallet_projection_total_value_zat(p) == 0);
    WP_CHECK("skeleton catch up preserves offset",
             wallet_projection_catch_up(p) == 0);
    wallet_projection_close(p);
    WP_CHECK("current pointer cleared", wallet_projection_current() == NULL);

    p = wallet_projection_open(proj_path, log);
    WP_CHECK("skeleton reopen", p != NULL);
    WP_CHECK("reopen offset preserved", wallet_projection_catch_up(p) == 0);
    WP_CHECK("address table exists",
             table_exists(proj_path, "wallet_view_addresses"));
    WP_CHECK("tx table exists",
             table_exists(proj_path, "wallet_view_transactions"));
    WP_CHECK("utxo table exists",
             table_exists(proj_path, "wallet_view_utxos"));
    WP_CHECK("note table exists",
             table_exists(proj_path, "wallet_view_notes"));
    WP_CHECK("meta table exists", table_exists(proj_path, "projection_meta"));
    WP_CHECK("no private key projection table",
             !table_exists(proj_path, "wallet_view_keys"));
    WP_CHECK("no seed projection table",
             !table_exists(proj_path, "wallet_view_seed"));
    WP_CHECK("no sapling key projection table",
             !table_exists(proj_path, "wallet_view_sapling_keys"));
    wallet_projection_close(p);
    wallet_projection_set_event_log(NULL);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_reader_api_aggregates(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    wp_tmpdir(dir, sizeof(dir), "readers");
    wp_paths(dir, elog_path, sizeof(elog_path), proj_path, sizeof(proj_path));

    event_log_t *log = event_log_open(elog_path);
    wallet_projection_t *p = wallet_projection_open(proj_path, log);
    WP_CHECK("reader open", log && p);
    WP_CHECK("reader insert utxos",
             exec_projection_sql(proj_path,
                 "INSERT INTO wallet_view_utxos"
                 "(txid,vout,value,address_hash,height,is_coinbase) VALUES"
                 "(x'0000000000000000000000000000000000000000000000000000000000000001',0,100,x'0101010101010101010101010101010101010101',1,0),"
                 "(x'0000000000000000000000000000000000000000000000000000000000000002',1,200,x'0202020202020202020202020202020202020202',2,0),"
                 "(x'0000000000000000000000000000000000000000000000000000000000000003',2,300,x'0303030303030303030303030303030303030303',3,1)"));
    WP_CHECK("reader utxo count", wallet_projection_utxo_count(p) == 3);
    WP_CHECK("reader utxo total", wallet_projection_total_value_zat(p) == 600);
    WP_CHECK("reader insert note",
             exec_projection_sql(proj_path,
                 "INSERT INTO wallet_view_notes"
                 "(txid,output_index,value,cm,block_height) VALUES"
                 "(x'1000000000000000000000000000000000000000000000000000000000000001',0,50,x'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',4)"));
    WP_CHECK("reader note count", wallet_projection_note_count(p) == 1);
    WP_CHECK("reader combined total",
             wallet_projection_total_value_zat(p) == 650);
    wallet_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_projection_catch_up_replay(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    wp_tmpdir(dir, sizeof(dir), "replay");
    wp_paths(dir, elog_path, sizeof(elog_path), proj_path, sizeof(proj_path));

    event_log_t *log = event_log_open(elog_path);
    wallet_projection_t *p = wallet_projection_open(proj_path, log);
    WP_CHECK("replay open", log && p);
    bool appended = true;
    for (int i = 0; i < 100 && appended; i++)
        appended = append_wallet_key(log, i);
    for (int i = 0; i < 5 && appended; i++)
        appended = append_wallet_addr_derived(log, i);
    for (int i = 0; i < 200 && appended; i++)
        appended = append_wallet_tx(log, i);
    for (int i = 0; i < 50 && appended; i++)
        appended = append_wallet_note(log, i);
    WP_CHECK("synthetic wallet events appended", appended);
    WP_CHECK("catch up synthetic events",
             wallet_projection_catch_up(p) != UINT64_MAX);
    WP_CHECK("replay address count",
             wallet_projection_address_count(p) == 105);
    WP_CHECK("replay tx count", wallet_projection_tx_count(p) == 200);
    WP_CHECK("replay utxo count", wallet_projection_utxo_count(p) == 0);
    WP_CHECK("replay note count", wallet_projection_note_count(p) == 50);
    WP_CHECK("replay total note value",
             wallet_projection_total_value_zat(p) == 1275);
    WP_CHECK("replay idempotent",
             wallet_projection_catch_up(p) != UINT64_MAX &&
             wallet_projection_address_count(p) == 105 &&
             wallet_projection_tx_count(p) == 200 &&
             wallet_projection_note_count(p) == 50);
    wallet_projection_close(p);

    p = wallet_projection_open(proj_path, log);
    WP_CHECK("replay reopen", p != NULL);
    WP_CHECK("replay persists counts",
             wallet_projection_address_count(p) == 105 &&
             wallet_projection_tx_count(p) == 200 &&
             wallet_projection_note_count(p) == 50 &&
             wallet_projection_total_value_zat(p) == 1275);
    wallet_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

int test_wallet_projection(void)
{
    int failures = 0;
    printf("\n=== Wallet Projection Tests ===\n");
    failures += t_wallet_event_ids();
    failures += t_key_add_payload_roundtrip();
    failures += t_addr_derived_payload_roundtrip();
    failures += t_tx_seen_payload_roundtrip();
    failures += t_note_decrypted_payload_roundtrip();
    failures += t_projection_skeleton_open_reopen();
    failures += t_reader_api_aggregates();
    failures += t_projection_catch_up_replay();
    printf("wallet_projection: %s\n", failures ? "FAIL" : "PASS");
    return failures;
}
