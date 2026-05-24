/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/small_projections.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void sp_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/small_projections_%d_%s",
             (int)getpid(), tag);
    test_cleanup_tmpdir(buf);
    mkdir("test-tmp", 0755);
    mkdir(buf, 0755);
}

static int t_projection_skeletons_fresh(void)
{
    int failures = 0;
    char dir[256];
    char elog_path[320];
    char contacts_path[320];
    char onion_path[320];
    char hodl_path[320];
    sp_tmpdir(dir, sizeof(dir), "fresh");
    snprintf(elog_path, sizeof(elog_path), "%s/event_log.dat", dir);
    snprintf(contacts_path, sizeof(contacts_path), "%s/contacts.db", dir);
    snprintf(onion_path, sizeof(onion_path), "%s/onion_announcements.db",
             dir);
    snprintf(hodl_path, sizeof(hodl_path), "%s/hodl_history.db", dir);

    event_log_t *log = event_log_open(elog_path);
    SP_CHECK("event log open", log != NULL);
    if (!log) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    contacts_projection_t *contacts =
        contacts_projection_open(contacts_path, log);
    onion_ann_projection_t *onion =
        onion_ann_projection_open(onion_path, log);
    hodl_history_projection_t *hodl =
        hodl_history_projection_open(hodl_path, log);
    SP_CHECK("contacts open", contacts != NULL);
    SP_CHECK("onion announcements open", onion != NULL);
    SP_CHECK("hodl history open", hodl != NULL);
    SP_CHECK("contacts current", contacts_projection_current() == contacts);
    SP_CHECK("onion current", onion_ann_projection_current() == onion);
    SP_CHECK("hodl current", hodl_history_projection_current() == hodl);
    SP_CHECK("contacts fresh count",
             contacts && contacts_projection_count(contacts) == 0);
    SP_CHECK("onion fresh count",
             onion && onion_ann_projection_count(onion) == 0);
    SP_CHECK("hodl fresh count",
             hodl && hodl_history_projection_count(hodl) == 0);
    SP_CHECK("contacts fresh catchup",
             contacts && contacts_projection_catch_up(contacts) == 0);
    SP_CHECK("onion fresh catchup",
             onion && onion_ann_projection_catch_up(onion) == 0);
    SP_CHECK("hodl fresh catchup",
             hodl && hodl_history_projection_catch_up(hodl) == 0);

    contacts_projection_close(contacts);
    onion_ann_projection_close(onion);
    hodl_history_projection_close(hodl);
    SP_CHECK("contacts current cleared", contacts_projection_current() == NULL);
    SP_CHECK("onion current cleared", onion_ann_projection_current() == NULL);
    SP_CHECK("hodl current cleared", hodl_history_projection_current() == NULL);

    contacts = contacts_projection_open(contacts_path, log);
    onion = onion_ann_projection_open(onion_path, log);
    hodl = hodl_history_projection_open(hodl_path, log);
    SP_CHECK("contacts reopen", contacts != NULL);
    SP_CHECK("onion reopen", onion != NULL);
    SP_CHECK("hodl reopen", hodl != NULL);
    SP_CHECK("contacts reopen count",
             contacts && contacts_projection_count(contacts) == 0);
    SP_CHECK("onion reopen count",
             onion && onion_ann_projection_count(onion) == 0);
    SP_CHECK("hodl reopen count",
             hodl && hodl_history_projection_count(hodl) == 0);
    SP_CHECK("contacts offset preserved",
             contacts && contacts_projection_catch_up(contacts) == 0);
    SP_CHECK("onion offset preserved",
             onion && onion_ann_projection_catch_up(onion) == 0);
    SP_CHECK("hodl offset preserved",
             hodl && hodl_history_projection_catch_up(hodl) == 0);

    contacts_projection_close(contacts);
    onion_ann_projection_close(onion);
    hodl_history_projection_close(hodl);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
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
    failures += t_projection_skeletons_fresh();
    printf("small_projections: %d failures\n", failures);
    return failures;
}
