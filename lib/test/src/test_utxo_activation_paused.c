/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "conditions/utxo_activation_paused.h"
#include "event/event.h"
#include "framework/condition.h"
#include "platform/clock.h"
#include "validation/process_block.h"

#include <stdatomic.h>

#define UAP_CHECK(name, expr) do { \
    printf("utxo_activation_paused: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

void register_block_failed_mask_at_tip(void);
void block_failed_mask_at_tip_test_reset(void);
int block_failed_mask_at_tip_test_stall_type(void);

struct fake_clock {
    _Atomic int64_t wall_ms;
};

static int64_t fake_now_mono(void *self)
{
    (void)self;
    return 1;
}

static int64_t fake_now_wall(void *self)
{
    struct fake_clock *c = (struct fake_clock *)self;
    return atomic_load(&c->wall_ms);
}

static void fake_clock_install(struct fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
    static clock_iface_t iface;
    iface.now_monotonic_ns = fake_now_mono;
    iface.now_wall_ms = fake_now_wall;
    iface.self = c;
    clock_set_default(&iface);
}

static void fake_clock_set(struct fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
}

static void operator_observer(enum event_type type, uint32_t peer_id,
                              const void *payload, uint32_t payload_len,
                              void *ctx)
{
    (void)type;
    (void)peer_id;
    (void)payload;
    (void)payload_len;
    _Atomic int *count = (_Atomic int *)ctx;
    atomic_fetch_add(count, 1);
}

static void reset_conditions(void)
{
    event_log_init();
    condition_engine_reset_for_testing();
    event_clear_all_observers();
    process_block_test_set_utxo_activation_paused_height(-1);
    utxo_activation_paused_test_reset();
    block_failed_mask_at_tip_test_reset();
}

static struct block_index *insert_test_block(struct main_state *ms,
                                             struct uint256 *hashes,
                                             int height,
                                             unsigned status)
{
    memset(&hashes[height], 0, sizeof(hashes[height]));
    hashes[height].data[0] = (uint8_t)height;
    hashes[height].data[1] = 0xA5;
    struct block_index *bi = chainstate_insert_block_index(
        (struct chainstate *)ms, &hashes[height]);
    if (!bi) return NULL;
    bi->nHeight = height;
    bi->nStatus = status;
    bi->nTx = 1;
    bi->nChainTx = (uint32_t)(height + 1);
    if (height > 0)
        bi->pprev = block_map_find(&ms->map_block_index, &hashes[height - 1]);
    return bi;
}

int test_utxo_activation_paused(void)
{
    printf("\n=== utxo activation paused condition tests ===\n");
    int failures = 0;

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 1000);
        bool ok = true;

        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[2];
        struct block_index *tip = insert_test_block(
            &ms, hashes, 0, BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        struct block_index *next = insert_test_block(
            &ms, hashes, 1, BLOCK_HAVE_DATA);
        ok = ok && tip && next && active_chain_set_tip(&ms.chain_active, tip);
        condition_engine_set_main_state(&ms);
        register_block_failed_mask_at_tip();

        condition_engine_tick();
        fake_clock_set(&clock, 1301);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 1;
        ok = ok && block_failed_mask_at_tip_test_stall_type() == 2;
        ok = ok && active_chain_set_tip(&ms.chain_active, next);
        fake_clock_set(&clock, 1302);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;

        UAP_CHECK("block condition fires on stale tip with HAVE_DATA", ok);
        main_state_free(&ms);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 2000);
        bool ok = true;
        register_utxo_activation_paused();
        process_block_test_set_utxo_activation_paused_height(1500);

        condition_engine_tick();
        fake_clock_set(&clock, 2299);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && process_block_test_get_utxo_activation_paused_height() == 1500;
        UAP_CHECK("detect does not fire before 300s", ok);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 3000);
        bool ok = true;
        register_utxo_activation_paused();
        process_block_test_set_utxo_activation_paused_height(1600);

        condition_engine_tick();
        fake_clock_set(&clock, 3301);
        condition_engine_tick();
        ok = ok && utxo_activation_paused_test_resume_calls() == 1;
        ok = ok && utxo_activation_paused_test_repair_calls() == 0;
        ok = ok && process_block_test_get_utxo_activation_paused_height() == -1;
        ok = ok && condition_engine_get_active_count() == 0;
        UAP_CHECK("resume remedy clears pause", ok);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 4000);
        bool ok = true;
        register_utxo_activation_paused();
        utxo_activation_paused_test_set_reason("utxo_audit_drift");
        process_block_test_set_utxo_activation_paused_height(1700);

        condition_engine_tick();
        fake_clock_set(&clock, 4301);
        condition_engine_tick();
        ok = ok && utxo_activation_paused_test_resume_calls() == 0;
        ok = ok && utxo_activation_paused_test_repair_calls() == 1;
        ok = ok && process_block_test_get_utxo_activation_paused_height() == -1;
        UAP_CHECK("drift reason takes repair path", ok);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 5000);
        _Atomic int operator_events;
        atomic_store(&operator_events, 0);
        bool ok = true;
        event_observe(EV_OPERATOR_NEEDED, operator_observer,
                      &operator_events);
        register_utxo_activation_paused();
        utxo_activation_paused_test_set_remedy_clear_enabled(false);
        process_block_test_set_utxo_activation_paused_height(1800);

        condition_engine_tick();
        fake_clock_set(&clock, 5301);
        condition_engine_tick();
        fake_clock_set(&clock, 5332);
        condition_engine_tick();
        ok = ok && condition_engine_get_unresolved_count() == 1;
        ok = ok && atomic_load(&operator_events) >= 1;
        UAP_CHECK("max attempts emits operator event", ok);
        clock_reset_default();
    }

    reset_conditions();
    clock_reset_default();
    return failures;
}
