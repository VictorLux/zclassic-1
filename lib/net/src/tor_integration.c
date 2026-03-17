/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor integration for zclassic23.
 *
 * Architecture: dynhost runs INSIDE our modified Tor. When a request
 * arrives over a Tor circuit, dynhost calls our handler directly —
 * no sockets, no HTTP, no ports. Just C function calls.
 *
 * For now: Tor runs as a subprocess. The dynhost webserver inside Tor
 * handles .onion requests. Future: link Tor as a library and call
 * dynhost_register_handler() to route requests to blog_serve(). */

#include "net/tor_integration.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Tor binary — try custom dynhost build first, fall back to system tor */
#define TOR_BINARY_CUSTOM "vendor/tor/src/app/tor"
#define TOR_BINARY_SYSTEM "/usr/bin/tor"

/* SOCKS port (for outbound Tor connections to discover .onion peers) */
#define TOR_SOCKS_PORT 19050

static pthread_t g_tor_thread;
static _Atomic bool g_tor_running = false;
static _Atomic bool g_tor_ready = false;
static char g_onion_address[128];
static char g_tor_datadir[512];
static char g_tor_binary[512];

/* Request handler — dynhost calls this directly for each .onion request.
 * Set by the application before starting Tor. */
static tor_request_handler_fn g_request_handler = NULL;
static void *g_request_handler_ctx = NULL;

void tor_integration_set_handler(tor_request_handler_fn handler, void *ctx)
{
    g_request_handler = handler;
    g_request_handler_ctx = ctx;
}

static uint16_t g_p2p_port = 18033;
static bool g_has_dynhost = false;

/* Write torrc — standard Tor creates hidden service for P2P,
 * dynhost Tor handles connections directly. */
static bool write_torrc(const char *datadir)
{
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", datadir);

    char hs_dir[1024];
    snprintf(hs_dir, sizeof(hs_dir), "%s/tor_data/zcl_hidden_service", datadir);
    mkdir(hs_dir, 0700);

    FILE *f = fopen(torrc_path, "w");
    if (!f) return false;

    fprintf(f,
        "SocksPort %d\n"
        "DataDirectory %s/tor_data\n"
        "Log notice file %s/tor.log\n",
        TOR_SOCKS_PORT, datadir, datadir);

    if (!g_has_dynhost) {
        /* Standard Tor: hidden service forwards to our P2P port */
        fprintf(f,
            "HiddenServiceDir %s\n"
            "HiddenServicePort %d 127.0.0.1:%d\n",
            hs_dir, g_p2p_port, g_p2p_port);
    }

    fclose(f);
    return true;
}

/* Read .onion from hidden service hostname file */
static bool read_onion_from_hs(const char *datadir)
{
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/tor_data/zcl_hidden_service/hostname", datadir);
    for (int attempt = 0; attempt < 120; attempt++) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[128];
            if (fgets(line, sizeof(line), f)) {
                char *end = line;
                while (*end && *end != '\n' && *end != '\r') end++;
                *end = '\0';
                if (strstr(line, ".onion")) {
                    snprintf(g_onion_address, sizeof(g_onion_address), "%s", line);
                    fclose(f);
                    return true;
                }
            }
            fclose(f);
        }
        sleep(1);
    }
    return false;
}

/* Parse .onion address from Tor's log */
static bool read_onion_from_log(const char *datadir)
{
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/tor.log", datadir);

    for (int attempt = 0; attempt < 120; attempt++) {
        FILE *f = fopen(log_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                char *p = strstr(line, "ephemeral service created with address: ");
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

static void *tor_onion_monitor(void *arg);

/* Tor embedding API — declared in vendor/tor/src/feature/api/tor_api.h */
typedef struct tor_main_configuration_t tor_main_configuration_t;
extern tor_main_configuration_t *tor_main_configuration_new(void);
extern int tor_main_configuration_set_command_line(
    tor_main_configuration_t *cfg, int argc, char *argv[]);
extern void tor_main_configuration_free(tor_main_configuration_t *cfg);
extern int tor_run_main(const tor_main_configuration_t *);

static void *tor_embedded_thread(void *arg)
{
    (void)arg;
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", g_tor_datadir);

    printf("Starting embedded Tor (dynhost=%s)...\n",
           g_has_dynhost ? "yes" : "no");
    fflush(stdout);

    /* Start .onion address monitor in parallel */
    static pthread_t onion_thread;
    pthread_create(&onion_thread, NULL, tor_onion_monitor, NULL);
    pthread_detach(onion_thread);

    /* Use the Tor embedding API */
    tor_main_configuration_t *cfg = tor_main_configuration_new();
    char *argv[] = {"tor", "-f", torrc_path};
    tor_main_configuration_set_command_line(cfg, 3, argv);

    /* This blocks until Tor exits */
    int result = tor_run_main(cfg);

    tor_main_configuration_free(cfg);
    atomic_store(&g_tor_running, false);
    atomic_store(&g_tor_ready, false);
    printf("Tor exited with code %d\n", result);
    return NULL;
}

/* Monitor thread: watches for .onion address to appear */
static void *tor_onion_monitor(void *arg)
{
    (void)arg;
    bool found = false;
    if (g_has_dynhost)
        found = read_onion_from_log(g_tor_datadir);
    if (!found)
        found = read_onion_from_hs(g_tor_datadir);
    if (!found)
        found = read_onion_from_log(g_tor_datadir);

    if (found) {
        atomic_store(&g_tor_ready, true);
        printf("Tor .onion address: %s (port %d)\n",
               g_onion_address, g_p2p_port);
        fflush(stdout);
    } else {
        fprintf(stderr, "tor: timed out waiting for .onion address\n");
    }
    return NULL;
}

bool tor_integration_start(const char *datadir, uint16_t p2p_port)
{
    if (atomic_load(&g_tor_running))
        return true;

    g_p2p_port = p2p_port;
    snprintf(g_tor_datadir, sizeof(g_tor_datadir), "%s", datadir);
    g_onion_address[0] = '\0';

    /* Find best Tor binary: custom dynhost first, then system */
    if (access(TOR_BINARY_CUSTOM, X_OK) == 0) {
        snprintf(g_tor_binary, sizeof(g_tor_binary), "%s", TOR_BINARY_CUSTOM);
        g_has_dynhost = true;
        printf("Tor: using dynhost binary (%s)\n", g_tor_binary);
    } else if (access(TOR_BINARY_SYSTEM, X_OK) == 0) {
        snprintf(g_tor_binary, sizeof(g_tor_binary), "%s", TOR_BINARY_SYSTEM);
        g_has_dynhost = false;
        printf("Tor: using system binary (%s)\n", g_tor_binary);
    } else {
        fprintf(stderr, "tor: no binary found (tried %s, %s)\n",
                TOR_BINARY_CUSTOM, TOR_BINARY_SYSTEM);
        return false;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/tor_data", datadir);
    mkdir(path, 0700);

    if (!write_torrc(datadir)) {
        fprintf(stderr, "tor: failed to write torrc\n");
        return false;
    }

    atomic_store(&g_tor_running, true);

    if (pthread_create(&g_tor_thread, NULL, tor_embedded_thread, NULL) != 0) {
        atomic_store(&g_tor_running, false);
        return false;
    }
    pthread_detach(g_tor_thread);
    return true;
}

void tor_integration_stop(void)
{
    /* Tor runs embedded in a thread — it will exit when the process exits.
     * For clean shutdown, Tor's event loop needs a signal. */
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
