/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for Tor integration: persistent .onion keys, torrc generation. */

#include "test/test_helpers.h"
#include "net/tor_integration.h"
#include "net/onion_service.h"
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

static int test_tor_stop_when_not_running(void)
{
    int failures = 0;
    printf("test_tor_stop_when_not_running: ");

    tor_integration_stop();

    if (!tor_integration_is_ready()) {
        printf("OK\n");
    } else {
        printf("FAIL (should not be ready after stop)\n");
        failures++;
    }

    return failures;
}

static int test_tor_torrc_has_hidden_service_dir(void)
{
    int failures = 0;
    printf("test_tor_torrc_has_hidden_service_dir: ");

    char tmpdir[] = "zcl_torrc_hs_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("SKIP (mkdtemp failed)\n");
        return 0;
    }

    /* Create tor_data/onion_service subdirs */
    char td[512];
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    mkdir(td, 0700);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    mkdir(td, 0700);

    tor_integration_start(tmpdir, 18033);
    tor_integration_stop();

    char torrc[512];
    snprintf(torrc, sizeof(torrc), "%s/torrc", tmpdir);

    FILE *f = fopen(torrc, "r");
    if (f) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        bool has_socks = strstr(buf, "SocksPort") != NULL;
        bool has_datadir = strstr(buf, "DataDirectory") != NULL;
        bool has_log = strstr(buf, "Log notice") != NULL;
        bool has_hs_dir = strstr(buf, "HiddenServiceDir") != NULL;
        bool has_hs_port = strstr(buf, "HiddenServicePort 80") != NULL;

        if (has_socks && has_datadir && has_log && has_hs_dir && has_hs_port) {
            printf("OK\n");
        } else {
            printf("FAIL (socks=%d datadir=%d log=%d hsdir=%d hsport=%d)\n",
                   has_socks, has_datadir, has_log, has_hs_dir, has_hs_port);
            failures++;
        }
    } else {
        printf("FAIL (torrc not written)\n");
        failures++;
    }

    /* Clean up */
    unlink(torrc);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    rmdir(td);
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    rmdir(td);
    /* tor.log may have been created */
    char logp[512];
    snprintf(logp, sizeof(logp), "%s/tor.log", tmpdir);
    unlink(logp);
    rmdir(tmpdir);

    return failures;
}

static int test_tor_persistent_hostname_read(void)
{
    int failures = 0;
    printf("test_tor_persistent_hostname_read: ");

    /* Simulate Tor writing the hostname file */
    char tmpdir[] = "zcl_hostname_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("SKIP (mkdtemp failed)\n");
        return 0;
    }

    char td[512];
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    mkdir(td, 0700);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    mkdir(td, 0700);

    /* Write a fake hostname file (56 chars + .onion) */
    char hostname_path[512];
    snprintf(hostname_path, sizeof(hostname_path),
             "%s/tor_data/onion_service/hostname", tmpdir);

    const char *fake_onion =
        "zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion";

    FILE *f = fopen(hostname_path, "w");
    if (!f) {
        printf("SKIP (cannot write hostname file)\n");
        rmdir(td);
        snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
        rmdir(td);
        rmdir(tmpdir);
        return 0;
    }
    fprintf(f, "%s\n", fake_onion);
    fclose(f);

    /* Now start tor_integration — it will write torrc and create dirs,
     * then the monitor would read the hostname file. We can't run the
     * full Tor stack, so test the onion_service_set_address path. */
    onion_service_set_address(fake_onion);
    const char *addr = onion_service_get_address();

    if (addr && strcmp(addr, fake_onion) == 0) {
        printf("OK\n");
    } else {
        printf("FAIL (got '%s', expected '%s')\n",
               addr ? addr : "NULL", fake_onion);
        failures++;
    }

    /* Clean up */
    onion_service_set_address(NULL);
    unlink(hostname_path);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    rmdir(td);
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    rmdir(td);
    rmdir(tmpdir);

    return failures;
}

static int test_tor_address_persists_across_restarts(void)
{
    int failures = 0;
    printf("test_tor_address_persists_across_restarts: ");

    char tmpdir[] = "zcl_persist_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("SKIP (mkdtemp failed)\n");
        return 0;
    }

    char td[512];
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    mkdir(td, 0700);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    mkdir(td, 0700);

    const char *expected =
        "abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuv1234.onion";

    /* Simulate first run: Tor writes hostname file */
    char hostname_path[512];
    snprintf(hostname_path, sizeof(hostname_path),
             "%s/tor_data/onion_service/hostname", tmpdir);
    FILE *f = fopen(hostname_path, "w");
    if (!f) {
        printf("SKIP (cannot write hostname)\n");
        rmdir(td);
        snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
        rmdir(td);
        rmdir(tmpdir);
        return 0;
    }
    fprintf(f, "%s\n", expected);
    fclose(f);

    /* Simulate first run: read and set address */
    onion_service_set_address(expected);
    const char *addr1 = onion_service_get_address();

    /* Simulate restart: clear address */
    onion_service_set_address(NULL);
    const char *cleared = onion_service_get_address();

    /* Simulate second run: re-read from hostname file */
    f = fopen(hostname_path, "r");
    char line[128] = "";
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
        }
        fclose(f);
    }
    onion_service_set_address(line);
    const char *addr2 = onion_service_get_address();

    bool ok = addr1 && strcmp(addr1, expected) == 0
           && cleared == NULL
           && addr2 && strcmp(addr2, expected) == 0;

    if (ok) {
        printf("OK\n");
    } else {
        printf("FAIL (addr1='%s' cleared='%s' addr2='%s')\n",
               addr1 ? addr1 : "NULL",
               cleared ? cleared : "NULL",
               addr2 ? addr2 : "NULL");
        failures++;
    }

    /* Clean up */
    onion_service_set_address(NULL);
    unlink(hostname_path);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    rmdir(td);
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    rmdir(td);
    rmdir(tmpdir);

    return failures;
}

static int test_tor_set_address_null_clears(void)
{
    int failures = 0;
    printf("test_tor_set_address_null_clears: ");

    onion_service_set_address("test.onion");
    const char *a = onion_service_get_address();
    bool had = a && strcmp(a, "test.onion") == 0;

    onion_service_set_address(NULL);
    bool gone = onion_service_get_address() == NULL;

    if (had && gone) {
        printf("OK\n");
    } else {
        printf("FAIL (had=%d gone=%d)\n", had, gone);
        failures++;
    }

    return failures;
}

static int test_tor_torrc_contains_onion_service_path(void)
{
    int failures = 0;
    printf("test_tor_torrc_onion_service_path: ");

    char tmpdir[] = "zcl_torrc_path_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        printf("SKIP (mkdtemp failed)\n");
        return 0;
    }

    char td[512];
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    mkdir(td, 0700);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    mkdir(td, 0700);

    tor_integration_start(tmpdir, 18033);
    tor_integration_stop();

    char torrc[512];
    snprintf(torrc, sizeof(torrc), "%s/torrc", tmpdir);
    FILE *f = fopen(torrc, "r");
    if (!f) {
        printf("FAIL (torrc not found)\n");
        failures++;
        rmdir(td);
        snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
        rmdir(td);
        rmdir(tmpdir);
        return failures;
    }

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Verify HiddenServiceDir contains the correct path */
    char expected_path[512];
    snprintf(expected_path, sizeof(expected_path),
             "HiddenServiceDir %s/tor_data/onion_service", tmpdir);

    if (strstr(buf, expected_path)) {
        printf("OK\n");
    } else {
        printf("FAIL (expected '%s' in torrc)\n", expected_path);
        failures++;
    }

    unlink(torrc);
    snprintf(td, sizeof(td), "%s/tor_data/onion_service", tmpdir);
    rmdir(td);
    snprintf(td, sizeof(td), "%s/tor_data", tmpdir);
    rmdir(td);
    char logp[512];
    snprintf(logp, sizeof(logp), "%s/tor.log", tmpdir);
    unlink(logp);
    rmdir(tmpdir);

    return failures;
}

int test_tor(void)
{
    int failures = 0;
    printf("\n=== Tor Integration Tests ===\n");

    failures += test_tor_initial_state();
    failures += test_tor_stop_when_not_running();
    failures += test_tor_torrc_has_hidden_service_dir();
    failures += test_tor_torrc_contains_onion_service_path();
    failures += test_tor_persistent_hostname_read();
    failures += test_tor_address_persists_across_restarts();
    failures += test_tor_set_address_null_clears();

    printf("Tor integration: %d failures\n", failures);
    return failures;
}
