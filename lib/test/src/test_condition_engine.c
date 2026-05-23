/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "event/event.h"
#include "framework/condition.h"
#include "json/json.h"

#include <stdatomic.h>
#include <string.h>

#define CE_CHECK(name, expr) do { \
    printf("condition_engine: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static _Atomic bool g_detect;
static _Atomic bool g_witness;
static _Atomic int g_remedy_calls;
static _Atomic int g_operator_events;

static bool ce_detect(void)
{
    return atomic_load(&g_detect);
}

static enum condition_remedy_result ce_remedy(void)
{
    atomic_fetch_add(&g_remedy_calls, 1);
    return COND_REMEDY_OK;
}

static bool ce_witness(int64_t target_at_detect)
{
    (void)target_at_detect;
    return atomic_load(&g_witness);
}

static void operator_observer(enum event_type type, uint32_t peer_id,
                              const void *payload, uint32_t payload_len,
                              void *ctx)
{
    (void)type;
    (void)peer_id;
    (void)payload;
    (void)payload_len;
    (void)ctx;
    atomic_fetch_add(&g_operator_events, 1);
}

static void reset_fixture(void)
{
    condition_engine_reset_for_testing();
    event_clear_all_observers();
    atomic_store(&g_detect, false);
    atomic_store(&g_witness, false);
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_operator_events, 0);
}

int test_condition_engine(void)
{
    printf("\n=== condition engine tests ===\n");
    int failures = 0;

    static struct condition c_basic = {
        .name = "ce_basic",
        .severity = COND_CRITICAL,
        .poll_secs = 1,
        .backoff_secs = 30,
        .max_attempts = 3,
        .detect = ce_detect,
        .remedy = ce_remedy,
        .witness = ce_witness,
        .witness_window_secs = 60,
    };

    {
        reset_fixture();
        bool ok = condition_register(&c_basic);
        atomic_store(&g_detect, true);
        condition_engine_tick();
        ok = ok && atomic_load(&g_remedy_calls) == 1;
        ok = ok && condition_engine_get_active_count() == 1;
        CE_CHECK("register + first remedy", ok);
    }

    {
        reset_fixture();
        bool ok = condition_register(&c_basic);
        atomic_store(&g_detect, true);
        condition_engine_tick();
        condition_engine_tick();
        ok = ok && atomic_load(&g_remedy_calls) == 1;
        CE_CHECK("backoff suppresses immediate retry", ok);
    }

    {
        reset_fixture();
        bool ok = condition_register(&c_basic);
        atomic_store(&g_detect, true);
        condition_engine_tick();
        atomic_store(&g_witness, true);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && atomic_load(&c_basic.state.cleared_count) == 1;
        CE_CHECK("witness clears active state", ok);
    }

    static struct condition c_max = {
        .name = "ce_max",
        .severity = COND_CRITICAL,
        .poll_secs = 1,
        .backoff_secs = 0,
        .max_attempts = 2,
        .detect = ce_detect,
        .remedy = ce_remedy,
        .witness = ce_witness,
        .witness_window_secs = 60,
    };

    {
        reset_fixture();
        event_observe(EV_OPERATOR_NEEDED, operator_observer, NULL);
        bool ok = condition_register(&c_max);
        atomic_store(&g_detect, true);
        condition_engine_tick();
        condition_engine_tick();
        ok = ok && atomic_load(&g_remedy_calls) == 2;
        ok = ok && condition_engine_get_unresolved_count() == 1;
        ok = ok && atomic_load(&g_operator_events) >= 1;
        CE_CHECK("max attempts emits operator event", ok);
    }

    {
        reset_fixture();
        bool ok = condition_register(&c_basic);
        ok = ok && !condition_register(&c_basic);
        CE_CHECK("duplicate registration rejected", ok);
    }

    {
        reset_fixture();
        bool ok = condition_register(&c_basic);
        struct json_value out;
        json_init(&out);
        json_set_object(&out);
        ok = ok && condition_engine_dump_state_json(&out, NULL);
        ok = ok && json_get(&out, "registered_count") != NULL;
        ok = ok && json_get(&out, "conditions") != NULL;
        json_free(&out);
        CE_CHECK("dump json includes registry", ok);
    }

    reset_fixture();
    return failures;
}
