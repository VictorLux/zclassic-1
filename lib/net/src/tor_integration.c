/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor is compiled INTO zclassic23. No external binary. No ports.
 * No SOCKS proxy. Tor runs as a thread inside our process.
 * Dynhost handles .onion connections via direct C function calls. */

#include "net/tor_integration.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_t g_tor_thread;
static _Atomic bool g_tor_running = false;
static _Atomic bool g_tor_ready = false;
static char g_onion_address[128];
static char g_tor_datadir[512];

static tor_request_handler_fn g_request_handler = NULL;
static void *g_request_handler_ctx = NULL;

void tor_integration_set_handler(tor_request_handler_fn handler, void *ctx)
{
    g_request_handler = handler;
    g_request_handler_ctx = ctx;
}

/* Write torrc — SocksPort 0 (no ports). Dynhost handles everything. */
static bool write_torrc(const char *datadir)
{
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", datadir);

    FILE *f = fopen(torrc_path, "w");
    if (!f) return false;

    fprintf(f,
        "SocksPort 0\n"
        "DataDirectory %s/tor_data\n"
        "Log notice file %s/tor.log\n",
        datadir, datadir);

    fclose(f);
    return true;
}

/* Parse .onion address from Tor's dynhost log output */
static bool read_onion_from_log(const char *datadir)
{
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/tor.log", datadir);

    for (int attempt = 0; attempt < 120; attempt++) {
        FILE *f = fopen(log_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                char *p = strstr(line,
                    "ephemeral service created with address: ");
                if (p) {
                    p += strlen("ephemeral service created with address: ");
                    char *end = p;
                    while (*end && *end != '\n' && *end != '\r' && *end != ' ')
                        end++;
                    size_t len = (size_t)(end - p);
                    if (len > 0 && len < sizeof(g_onion_address)) {
                        memcpy(g_onion_address, p, len);
                        g_onion_address[len] = '\0';
                        if (!strstr(g_onion_address, ".onion")) {
                            size_t alen = strlen(g_onion_address);
                            if (alen + 7 <= sizeof(g_onion_address) - 1)
                                memcpy(g_onion_address + alen, ".onion", 7);
                        }
                        fclose(f);
                        return true;
                    }
                }
            }
            fclose(f);
        }
        sleep(1);
    }
    return false;
}

/* Tor embedding API */
typedef struct tor_main_configuration_t tor_main_configuration_t;
extern tor_main_configuration_t *tor_main_configuration_new(void);
extern int tor_main_configuration_set_command_line(
    tor_main_configuration_t *cfg, int argc, char *argv[]);
extern void tor_main_configuration_free(tor_main_configuration_t *cfg);
extern int tor_run_main(const tor_main_configuration_t *);

/* Dynhost external handler — routes .onion requests to our code */
typedef size_t (*dynhost_external_handler_fn)(const char *, const char *,
    const uint8_t *, size_t, uint8_t *, size_t, void *);
extern void dynhost_webserver_set_external_handler(
    dynhost_external_handler_fn handler, void *ctx);

/* Bridge: dynhost calls this → we call the registered handler */
static size_t dynhost_bridge(const char *method, const char *path,
                              const uint8_t *body, size_t body_len,
                              uint8_t *response, size_t response_max,
                              void *ctx)
{
    (void)ctx;
    if (!g_request_handler) return 0;
    return g_request_handler(method, path, body, body_len,
                              response, response_max,
                              g_request_handler_ctx);
}

static void *tor_onion_monitor(void *arg);

static void *tor_thread_fn(void *arg)
{
    (void)arg;
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", g_tor_datadir);

    printf("Tor: starting embedded (no ports, no SOCKS, dynhost only)\n");
    fflush(stdout);

    /* Monitor for .onion address in parallel */
    pthread_t mon;
    pthread_create(&mon, NULL, tor_onion_monitor, NULL);
    pthread_detach(mon);

    /* Run Tor in this thread (blocks until exit) */
    tor_main_configuration_t *cfg = tor_main_configuration_new();
    char *argv[] = {"tor", "-f", torrc_path};
    tor_main_configuration_set_command_line(cfg, 3, argv);
    int result = tor_run_main(cfg);
    tor_main_configuration_free(cfg);

    atomic_store(&g_tor_running, false);
    atomic_store(&g_tor_ready, false);
    printf("Tor: exited with code %d\n", result);
    return NULL;
}

static void *tor_onion_monitor(void *arg)
{
    (void)arg;
    if (read_onion_from_log(g_tor_datadir)) {
        atomic_store(&g_tor_ready, true);
        printf("Tor .onion: %s\n", g_onion_address);
        fflush(stdout);
    } else {
        fprintf(stderr, "Tor: timed out waiting for .onion\n");
    }
    return NULL;
}

bool tor_integration_start(const char *datadir, uint16_t p2p_port)
{
    (void)p2p_port;

    if (atomic_load(&g_tor_running))
        return true;

    snprintf(g_tor_datadir, sizeof(g_tor_datadir), "%s", datadir);
    g_onion_address[0] = '\0';

    char path[1024];
    snprintf(path, sizeof(path), "%s/tor_data", datadir);
    mkdir(path, 0700);

    if (!write_torrc(datadir)) {
        fprintf(stderr, "Tor: failed to write torrc\n");
        return false;
    }

    /* Register our handler with Tor's dynhost before starting.
     * All .onion HTTP requests will route through dynhost_bridge →
     * g_request_handler → onion_service_handle_request. */
    if (g_request_handler) {
        dynhost_webserver_set_external_handler(dynhost_bridge, NULL);
        printf("Tor: external handler registered for .onion requests\n");
    }

    atomic_store(&g_tor_running, true);

    if (pthread_create(&g_tor_thread, NULL, tor_thread_fn, NULL) != 0) {
        atomic_store(&g_tor_running, false);
        return false;
    }
    pthread_detach(g_tor_thread);
    return true;
}

void tor_integration_stop(void)
{
    atomic_store(&g_tor_running, false);
    atomic_store(&g_tor_ready, false);
}

const char *tor_integration_get_onion_address(void)
{
    if (!atomic_load(&g_tor_ready))
        return NULL;
    return g_onion_address;
}

bool tor_integration_is_ready(void)
{
    return atomic_load(&g_tor_ready);
}
