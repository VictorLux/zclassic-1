/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"

#include <stdio.h>
#include <string.h>

#define SP_CHECK(label, cond) do { \
    bool _ok = (cond); \
    printf("small_projections: %s... %s\n", (label), _ok ? "OK" : "FAIL"); \
    if (!_ok) failures++; \
} while (0)

static int t_payload_ids(void)
{
    int failures = 0;
    SP_CHECK("contact set id", EV_CONTACT_SET == 20);
    SP_CHECK("contact touched id", EV_CONTACT_TOUCHED == 21);
    SP_CHECK("contact delete id", EV_CONTACT_DELETE == 22);
    SP_CHECK("onion announcement id", EV_ONION_ANNOUNCEMENT == 23);
    SP_CHECK("hodl snapshot id", EV_HODL_SNAPSHOT == 24);
    return failures;
}

static int t_contact_payload_roundtrip(void)
{
    int failures = 0;

    {
        const char *address = "t1TaskOneContactAddressFixture";
        const char *name = "Alice Ops";
        struct ev_contact_set in = {
            .address_len = (uint8_t)strlen(address),
            .name_len = (uint8_t)strlen(name),
            .address = address,
            .name = name,
        };
        struct ev_contact_set out;
        uint8_t buf[128];
        size_t len = 0;
        SP_CHECK("contact set serialize",
                 ev_contact_set_serialize(&in, buf, sizeof(buf), &len));
        SP_CHECK("contact set len",
                 len == EV_CONTACT_SET_FIXED_LEN + strlen(address) +
                        strlen(name));
        SP_CHECK("contact set parse",
                 ev_contact_set_parse(buf, len, &out));
        SP_CHECK("contact set roundtrip",
                 out.address_len == in.address_len &&
                 out.name_len == in.name_len &&
                 memcmp(out.address, address, in.address_len) == 0 &&
                 memcmp(out.name, name, in.name_len) == 0);
    }

    {
        const char *address = "t1TaskOneContactTouchFixture";
        struct ev_contact_touched in = {
            .address_len = (uint8_t)strlen(address),
            .last_used_unix = 1777777777u,
            .address = address,
        };
        struct ev_contact_touched out;
        uint8_t buf[128];
        size_t len = 0;
        SP_CHECK("contact touched serialize",
                 ev_contact_touched_serialize(&in, buf, sizeof(buf), &len));
        SP_CHECK("contact touched parse",
                 ev_contact_touched_parse(buf, len, &out));
        SP_CHECK("contact touched roundtrip",
                 out.address_len == in.address_len &&
                 out.last_used_unix == in.last_used_unix &&
                 memcmp(out.address, address, in.address_len) == 0);
    }

    {
        const char *address = "t1TaskOneContactDeleteFixture";
        struct ev_contact_delete in = {
            .address_len = (uint8_t)strlen(address),
            .address = address,
        };
        struct ev_contact_delete out;
        uint8_t buf[128];
        size_t len = 0;
        SP_CHECK("contact delete serialize",
                 ev_contact_delete_serialize(&in, buf, sizeof(buf), &len));
        SP_CHECK("contact delete parse",
                 ev_contact_delete_parse(buf, len, &out));
        SP_CHECK("contact delete roundtrip",
                 out.address_len == in.address_len &&
                 memcmp(out.address, address, in.address_len) == 0);
    }

    return failures;
}

static int t_onion_payload_roundtrip(void)
{
    int failures = 0;
    const char *onion =
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdef.onion";
    const char *script = "6a045a434c23010474657374";
    struct ev_onion_announcement in = {
        .announced_at_unix = 1765432100u,
        .onion_addr_len = (uint8_t)strlen(onion),
        .script_hex_len = (uint8_t)strlen(script),
        .onion_address = onion,
        .script_hex = script,
    };
    struct ev_onion_announcement out;
    uint8_t buf[256];
    size_t len = 0;

    SP_CHECK("onion announcement serialize",
             ev_onion_announcement_serialize(&in, buf, sizeof(buf), &len));
    SP_CHECK("onion announcement parse",
             ev_onion_announcement_parse(buf, len, &out));
    SP_CHECK("onion announcement roundtrip",
             out.announced_at_unix == in.announced_at_unix &&
             out.onion_addr_len == in.onion_addr_len &&
             out.script_hex_len == in.script_hex_len &&
             memcmp(out.onion_address, onion, in.onion_addr_len) == 0 &&
             memcmp(out.script_hex, script, in.script_hex_len) == 0);
    return failures;
}

static int t_hodl_payload_roundtrip(void)
{
    int failures = 0;
    struct ev_hodl_snapshot in = {
        .height = 1234567,
        .time_unix = 1760000000u,
        .total_zat = 99887766554433LL,
        .older_1y_zat = 5544332211LL,
        .older_1y_pct = 42.625,
    };
    struct ev_hodl_snapshot out;
    uint8_t buf[EV_HODL_SNAPSHOT_LEN];

    SP_CHECK("hodl snapshot serialize", ev_hodl_snapshot_serialize(&in, buf));
    SP_CHECK("hodl snapshot parse",
             ev_hodl_snapshot_parse(buf, sizeof(buf), &out));
    SP_CHECK("hodl snapshot roundtrip",
             out.height == in.height &&
             out.time_unix == in.time_unix &&
             out.total_zat == in.total_zat &&
             out.older_1y_zat == in.older_1y_zat &&
             out.older_1y_pct == in.older_1y_pct);
    return failures;
}

int test_small_projections(void)
{
    int failures = 0;
    printf("\n=== small_projections tests ===\n");
    failures += t_payload_ids();
    failures += t_contact_payload_roundtrip();
    failures += t_onion_payload_roundtrip();
    failures += t_hodl_payload_roundtrip();
    printf("small_projections: %d failures\n", failures);
    return failures;
}
