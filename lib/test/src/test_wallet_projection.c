/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"

#include <stdio.h>
#include <string.h>

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

int test_wallet_projection(void)
{
    int failures = 0;
    printf("\n=== Wallet Projection Tests ===\n");
    failures += t_wallet_event_ids();
    failures += t_key_add_payload_roundtrip();
    failures += t_addr_derived_payload_roundtrip();
    failures += t_tx_seen_payload_roundtrip();
    failures += t_note_decrypted_payload_roundtrip();
    printf("wallet_projection: %s\n", failures ? "FAIL" : "PASS");
    return failures;
}
