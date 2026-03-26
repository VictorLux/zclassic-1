/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Spec tests for ActiveRecord lifecycle hooks:
 * before_validate, after_validate, before_save, after_save,
 * before_destroy, after_destroy, async callbacks. */

#include "test/test_helpers.h"
#include "models/activerecord.h"
#include "models/block.h"
#include <string.h>
#include <stdio.h>

/* ── Test state ─────────────────────────────────────────── */

static int g_bv_count, g_av_count, g_bs_count, g_as_count;
static int g_bd_count, g_ad_count, g_async_s, g_async_d;

static bool hook_bv(void *r, void *c) { (void)r;(void)c; g_bv_count++; return true; }
static void hook_av(void *r, void *c) { (void)r;(void)c; g_av_count++; }
static bool hook_bs(void *r, void *c) { (void)r;(void)c; g_bs_count++; return true; }
static void hook_as(void *r, void *c) { (void)r;(void)c; g_as_count++; }
static bool hook_bd(void *r, void *c) { (void)r;(void)c; g_bd_count++; return true; }
static void hook_ad(void *r, void *c) { (void)r;(void)c; g_ad_count++; }
static void hook_as_async(void *r, size_t sz, void *c) { (void)r;(void)sz;(void)c; g_async_s++; }
static void hook_ad_async(void *r, size_t sz, void *c) { (void)r;(void)sz;(void)c; g_async_d++; }
static bool hook_reject(void *r, void *c) { (void)r;(void)c; return false; }

static void reset(void) {
    g_bv_count=0; g_av_count=0; g_bs_count=0; g_as_count=0;
    g_bd_count=0; g_ad_count=0; g_async_s=0; g_async_d=0;
}

int spec_data_hooks(void)
{
    int failures = 0;
    printf("\n=== ActiveRecord Lifecycle Hooks ===\n");

    {   printf("before_validate and after_validate fire... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_before_validate(&cb, hook_bv);
        ar_register_after_validate(&cb, hook_av);
        bool ok = ar_run_before_validate(&cb, NULL);
        ar_run_after_validate(&cb, NULL);
        ok = ok && g_bv_count == 1 && g_av_count == 1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("before_validate halt stops validation... ");
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_before_validate(&cb, hook_reject);
        bool ok = !ar_run_before_validate(&cb, NULL);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("full lifecycle: validate → save → after fires... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_before_validate(&cb, hook_bv);
        ar_register_after_validate(&cb, hook_av);
        ar_register_before_save(&cb, hook_bs);
        ar_register_after_save(&cb, hook_as);
        ar_run_before_validate(&cb, NULL);
        ar_run_after_validate(&cb, NULL);
        ar_run_before_save(&cb, NULL);
        ar_run_after_save(&cb, NULL);
        bool ok = g_bv_count==1 && g_av_count==1 && g_bs_count==1 && g_as_count==1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("before_save halt does not affect validation... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_before_validate(&cb, hook_bv);
        ar_register_before_save(&cb, hook_reject);
        bool ok = ar_run_before_validate(&cb, NULL);
        ok = ok && !ar_run_before_save(&cb, NULL);
        ok = ok && g_bv_count == 1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("before_destroy / after_destroy fire... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_before_destroy(&cb, hook_bd);
        ar_register_after_destroy(&cb, hook_ad);
        bool ok = ar_run_before_destroy(&cb, NULL);
        ar_run_after_destroy(&cb, NULL);
        ok = ok && g_bd_count==1 && g_ad_count==1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("after_save_async fires... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_after_save_async(&cb, hook_as_async);
        ar_run_after_save_async(&cb, NULL);
        bool ok = g_async_s == 1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("after_destroy_async fires... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_after_destroy_async(&cb, hook_ad_async);
        ar_run_after_destroy_async(&cb, NULL);
        bool ok = g_async_d == 1;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("multiple before_save callbacks all run... ");
        reset();
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        ar_register_before_save(&cb, hook_bs);
        ar_register_before_save(&cb, hook_bs);
        ar_register_before_save(&cb, hook_bs);
        ar_run_before_save(&cb, NULL);
        bool ok = g_bs_count == 3;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("callback overflow rejected... ");
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        bool ok = true;
        for (int i = 0; i < AR_MAX_CALLBACKS; i++)
            ok = ok && ar_register_before_save(&cb, hook_bs);
        ok = ok && !ar_register_before_save(&cb, hook_bs);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    {   printf("context pointer passed to callbacks... ");
        struct ar_callbacks cb; ar_callbacks_init(&cb);
        int ctx_val = 42;
        ar_callbacks_set_ctx(&cb, &ctx_val);
        bool ok = cb.ctx == &ctx_val;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    printf("Data hooks: %d failures\n", failures);
    return failures;
}
