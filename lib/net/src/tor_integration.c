/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor integration for zclassic23.
 * Starts the custom dynhost-enabled Tor binary as a subprocess.
 * The blog and web services run inside Tor itself via dynhost —
 * no HTTP ports, no forwarding, all traffic flows over onion circuits. */

#include "net/tor_integration.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Default path to the custom dynhost-enabled Tor binary. */
#define TOR_BINARY_DEFAULT "/home/bob/github_old/cheeseburger/tor/src/app/tor"

/* SOCKS port for the Tor instance (0 = disabled if only hosting). */
#define TOR_SOCKS_PORT 19050

static pid_t g_tor_pid = -1;
static pthread_t g_tor_thread;
static _Atomic bool g_tor_running = false;
static _Atomic bool g_tor_ready = false;
static char g_onion_address[128];
static char g_tor_datadir[512];
static char g_tor_binary[512];

/* Write a minimal torrc. The dynhost subsystem inside our custom Tor
 * creates the ephemeral .onion service automatically at level 52. */
static bool write_torrc(const char *datadir)
{
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", datadir);

    FILE *f = fopen(torrc_path, "w");
    if (!f) return false;

    fprintf(f,
        "SocksPort %d\n"
        "DataDirectory %s/tor_data\n"
        "Log notice file %s/tor.log\n"
        "Log notice stdout\n",
        TOR_SOCKS_PORT, datadir, datadir);

    fclose(f);
    return true;
}

/* Parse the .onion address from Tor's log output.
 * The dynhost subsystem logs:
 *   "Dynamic onion host ephemeral service created with address: <addr>" */
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
                        /* Append .onion if not present */
                        if (!strstr(g_onion_address, ".onion")) {
                            size_t alen = strlen(g_onion_address);
                            if (alen + 7 < sizeof(g_onion_address)) {
                                memcpy(g_onion_address + alen, ".onion", 7);
                            }
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

static void *tor_monitor_thread(void *arg)
{
    (void)arg;
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", g_tor_datadir);

    g_tor_pid = fork();
    if (g_tor_pid == 0) {
        execlp(g_tor_binary, "tor", "-f", torrc_path, (char *)NULL);
        fprintf(stderr, "tor: exec failed: %s\n", strerror(errno));
        _exit(127);
    }
    if (g_tor_pid < 0) {
        fprintf(stderr, "tor: fork failed: %s\n", strerror(errno));
        atomic_store(&g_tor_running, false);
        return NULL;
    }

    printf("Tor dynhost started (pid %d), waiting for .onion...\n", g_tor_pid);
    fflush(stdout);

    if (read_onion_from_log(g_tor_datadir)) {
        atomic_store(&g_tor_ready, true);
        printf("Tor hidden service: http://%s/\n", g_onion_address);
        printf("Blog:  http://%s/blog\n", g_onion_address);
        printf("Time:  http://%s/time\n", g_onion_address);
        printf("Calc:  http://%s/calculator\n", g_onion_address);
        fflush(stdout);
    } else {
        fprintf(stderr, "tor: timed out waiting for dynhost .onion address\n");
    }

    int status;
    waitpid(g_tor_pid, &status, 0);
    atomic_store(&g_tor_running, false);
    atomic_store(&g_tor_ready, false);
    g_tor_pid = -1;
    return NULL;
}

bool tor_integration_start(const char *datadir, uint16_t p2p_port)
{
    (void)p2p_port;

    if (atomic_load(&g_tor_running))
        return true;

    snprintf(g_tor_datadir, sizeof(g_tor_datadir), "%s", datadir);
    snprintf(g_tor_binary, sizeof(g_tor_binary), "%s", TOR_BINARY_DEFAULT);
    g_onion_address[0] = '\0';

    /* Verify the custom Tor binary exists */
    if (access(g_tor_binary, X_OK) != 0) {
        fprintf(stderr, "tor: binary not found at %s\n", g_tor_binary);
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

    if (pthread_create(&g_tor_thread, NULL, tor_monitor_thread, NULL) != 0) {
        atomic_store(&g_tor_running, false);
        return false;
    }
    pthread_detach(g_tor_thread);
    return true;
}

void tor_integration_stop(void)
{
    if (g_tor_pid > 0) {
        kill(g_tor_pid, SIGINT);
        int status;
        waitpid(g_tor_pid, &status, 0);
        g_tor_pid = -1;
    }
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
