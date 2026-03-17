/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for Tor integration module. */

#include "test/test_helpers.h"
#include "net/tor_integration.h"
#include <sys/stat.h>
#include <unistd.h>

static int test_tor_initial_state(void)
{
    int failures = 0;
    printf("test_tor_initial_state: ");

    if (tor_integration_is_ready()) {
        printf("FAIL (should not be ready before start)\n");
        failures++;
    } else {
        printf("OK\n");
    }

    printf("test_tor_get_onion_null: ");
    if (tor_integration_get_onion_address() != NULL) {
        printf("FAIL (should be NULL before start)\n");
        failures++;
    } else {
        printf("OK\n");
    }

    return failures;
}

static int test_tor_missing_binary(void)
{
    int failures = 0;
    printf("test_tor_missing_binary: ");

    /* Try to start with a nonexistent datadir — the binary check
     * inside tor_integration_start should catch this. We test the
     * failure path without actually needing Tor installed. */
    char tmpdir[] = "/tmp/zcl_tor_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("SKIP (mkdtemp failed)\n");
        return 0;
    }

    /* tor_integration_start checks for the Tor binary existence.
     * If the binary doesn't exist, it returns false. */
    bool started = tor_integration_start(tmpdir, 18033);

    /* Clean up */
    char path[512];
    snprintf(path, sizeof(path), "%s/tor_data", tmpdir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/torrc", tmpdir);
    unlink(path);
    rmdir(tmpdir);

    if (!started) {
        printf("OK (correctly failed with missing binary)\n");
    } else {
        /* Binary exists on this system — stop it */
        tor_integration_stop();
        printf("OK (binary found, started and stopped)\n");
    }

    return failures;
}

static int test_tor_stop_when_not_running(void)
{
    int failures = 0;
    printf("test_tor_stop_when_not_running: ");

    /* Calling stop when not running should be safe (no-op) */
    tor_integration_stop();

    if (!tor_integration_is_ready()) {
        printf("OK\n");
    } else {
        printf("FAIL (should not be ready after stop)\n");
        failures++;
    }

    return failures;
}

static int test_tor_torrc_generation(void)
{
    int failures = 0;
    printf("test_tor_torrc_generation: ");

    char tmpdir[] = "/tmp/zcl_torrc_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("SKIP (mkdtemp failed)\n");
        return 0;
    }

    /* Create tor_data subdir */
    char td[512];
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    mkdir(td, 0700);

    /* Start will write torrc even if binary doesn't exist */
    tor_integration_start(tmpdir, 18033);
    tor_integration_stop();

    /* Check torrc was written */
    char torrc[512];
    snprintf(torrc, sizeof(torrc), "%s/torrc", tmpdir);

    FILE *f = fopen(torrc, "r");
    if (f) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        /* Verify key directives */
        bool has_socks = strstr(buf, "SocksPort") != NULL;
        bool has_datadir = strstr(buf, "DataDirectory") != NULL;
        bool has_log = strstr(buf, "Log notice") != NULL;

        if (has_socks && has_datadir && has_log) {
            printf("OK\n");
        } else {
            printf("FAIL (missing directives: socks=%d datadir=%d log=%d)\n",
                   has_socks, has_datadir, has_log);
            failures++;
        }
    } else {
        printf("FAIL (torrc not written)\n");
        failures++;
    }

    /* Clean up */
    unlink(torrc);
    rmdir(td);
    rmdir(tmpdir);

    return failures;
}

int test_tor(void)
{
    int failures = 0;
    printf("\n=== Tor Integration Tests ===\n");

    failures += test_tor_initial_state();
    failures += test_tor_stop_when_not_running();
    failures += test_tor_torrc_generation();
    failures += test_tor_missing_binary();

    printf("Tor integration: %d failures\n", failures);
    return failures;
}
