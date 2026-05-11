/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "validation/checkpoint.h"
#include "validation/main_constants.h"

#include <stdio.h>
#include <string.h>

int test_checkpoint(void)
{
    int failures = 0;

    printf("reorg_is_allowed: no tip allows... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(-1, 0, &r);
        if (ok && r && strcmp(r, "no_tip") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d r=%s)\n", ok, r ? r : "(null)"); failures++; }
    }

    printf("reorg_is_allowed: target above tip is no-op... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(100, 200, &r);
        if (ok && r && strcmp(r, "no_disconnect") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d r=%s)\n", ok, r ? r : "(null)"); failures++; }
    }

    printf("reorg_is_allowed: target = tip is allowed (forward)... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(100, 100, &r);
        if (ok)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("reorg_is_allowed: 1-deep reorg allowed... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(100, 99, &r);
        if (ok && r && strcmp(r, "within_max_reorg") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d r=%s)\n", ok, r ? r : "(null)"); failures++; }
    }

    printf("reorg_is_allowed: MAX_REORG_LENGTH-deep allowed... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(1000, 1000 - MAX_REORG_LENGTH, &r);
        if (ok && r && strcmp(r, "within_max_reorg") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d r=%s)\n", ok, r ? r : "(null)"); failures++; }
    }

    printf("reorg_is_allowed: MAX_REORG_LENGTH+1 deep refused... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(1000, 1000 - MAX_REORG_LENGTH - 1, &r);
        if (!ok && r && strcmp(r, "below_checkpoint") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d r=%s)\n", ok, r ? r : "(null)"); failures++; }
    }

    printf("reorg_is_allowed: very deep refused... ");
    {
        const char *r = NULL;
        bool ok = reorg_is_allowed(3000000, 1000, &r);
        if (!ok && r && strcmp(r, "below_checkpoint") == 0)
            printf("OK\n");
        else { printf("FAIL (ok=%d r=%s)\n", ok, r ? r : "(null)"); failures++; }
    }

    printf("height_is_immutable: below floor is immutable... ");
    {
        if (height_is_immutable(1000, 500))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("height_is_immutable: tip itself is mutable... ");
    {
        if (!height_is_immutable(1000, 1000))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("height_is_immutable: tip - MAX_REORG_LENGTH boundary mutable... ");
    {
        if (!height_is_immutable(1000, 1000 - MAX_REORG_LENGTH + 1))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
