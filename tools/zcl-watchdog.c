/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcl-watchdog: continuous health validator for zclassic23.
 * Runs as a systemd linger service, polls node via RPC every 30s.
 * Logs alerts when: height stalls, peers drop, sync fails, RPC dies.
 *
 * Usage: zcl-watchdog [-datadir=DIR] [-interval=N] [-rpcport=N] */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>

static volatile bool g_running = true;

static void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ── RPC call (reuses zcl-rpc pattern) ─────────────────────────── */

static int rpc_call(const char *cookie, int port,
                    const char *method, char *out, size_t out_len)
{
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"method\":\"%s\",\"params\":[],\"id\":1}",
        method);

    char tmpf[] = "/tmp/zcl-wd-XXXXXX";
    int tfd = mkstemp(tmpf);
    if (tfd < 0) return -1;
    ssize_t wr = write(tfd, body, strlen(body));
    (void)wr;
    close(tfd);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s --max-time 10 --user \"%s\" "
        "-d @%s -H 'content-type:text/plain;' "
        "http://127.0.0.1:%d/ 2>/dev/null",
        cookie, tmpf, port);

    FILE *p = popen(cmd, "r");
    if (!p) { unlink(tmpf); return -1; }

    size_t total = fread(out, 1, out_len - 1, p);
    out[total] = '\0';
    int rc = pclose(p);
    unlink(tmpf);
    return (rc == 0 && total > 0) ? (int)total : -1;
}

/* ── JSON value extractor (simple, no library) ─────────────────── */

static int json_int(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

static bool json_bool(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

static void json_str(const char *json, const char *key,
                     char *out, size_t out_len)
{
    out[0] = '\0';
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* ── Watchdog state ────────────────────────────────────────────── */

struct watchdog_state {
    int  last_height;
    int  last_peers;
    bool last_healthy;
    bool last_synced;
    time_t last_height_change;
    time_t last_alert;
    int  consecutive_rpc_failures;
    int  checks_total;
    int  alerts_total;
    time_t sync_violation_first_seen;  /* peer_max - tip > 100 onset */
    time_t last_restart;               /* rate-limit external restarts */
    bool ever_seen_rpc_up;             /* set once after the first RPC success */
    bool boot_stall_emerg_fired;       /* one-shot EMERG if RPC never opens */
    time_t watchdog_start_time;        /* when this watchdog process started */
    int  restart_count;
};

#define SYNC_VIOLATION_GAP            100   /* blocks */
#define SYNC_VIOLATION_TRIGGER_SECS   600   /* 10 min sustained */
#define EXTERNAL_RESTART_COOLDOWN     900   /* 15 min between restarts */

static void log_ts(const char *level, const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[%s] %s: ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void watchdog_check(struct watchdog_state *wd,
                           const char *cookie, int port)
{
    wd->checks_total++;

    char buf[16384];
    if (rpc_call(cookie, port, "getblockchaininfo", buf, sizeof(buf)) < 0) {
        wd->consecutive_rpc_failures++;
        if (wd->consecutive_rpc_failures >= 3) {
            log_ts("ALERT", "RPC unreachable (%d consecutive failures)",
                   wd->consecutive_rpc_failures);
            wd->alerts_total++;
        }
        /* Part N: RPC-not-listening EMERG. If 120s elapsed since this
         * watchdog process started AND we have NEVER seen RPC up, the
         * node booted but never opened its port. This is the silent
         * boot-stall case Part O fixes inside the node; the external
         * EMERG is defense-in-depth so the operator hears about it
         * even if both in-process guards regressed. One-shot. */
        time_t now = time(NULL);
        if (!wd->ever_seen_rpc_up && !wd->boot_stall_emerg_fired &&
            wd->watchdog_start_time > 0 &&
            now - wd->watchdog_start_time > 120) {
            wd->boot_stall_emerg_fired = true;
            wd->alerts_total++;
            log_ts("EMERG",
                "RPC has never been reachable %lds after watchdog start "
                "(zclassic23 boot is stuck or never opened port %d)",
                (long)(now - wd->watchdog_start_time), port);
            int rc = system("systemd-cat -p emerg -t zcl-watchdog "
                "echo 'zclassic23 boot-stall: RPC never opened "
                "120s after watchdog start'");
            (void)rc;
        }
        return;
    }

    if (wd->consecutive_rpc_failures > 0) {
        log_ts("OK", "RPC recovered after %d failures",
               wd->consecutive_rpc_failures);
    }
    wd->consecutive_rpc_failures = 0;
    wd->ever_seen_rpc_up = true;

    int height = json_int(buf, "blocks");
    int headers = json_int(buf, "headers");

    /* Get peer count */
    char buf2[8192];
    int peers = 0;
    if (rpc_call(cookie, port, "getconnectioncount", buf2, sizeof(buf2)) >= 0)
        peers = json_int(buf2, "result");

    /* Get sync state from getsyncdetail */
    char sync_state[64] = "unknown";
    bool healthy = false;
    bool synced = false;
    if (rpc_call(cookie, port, "getsyncdetail", buf2, sizeof(buf2)) >= 0) {
        json_str(buf2, "sync_state", sync_state, sizeof(sync_state));
        healthy = json_bool(buf2, "healthy");
        synced = json_bool(buf2, "synced");
    }

    time_t now = time(NULL);

    /* Check 1: Height advancing */
    if (height > wd->last_height) {
        wd->last_height_change = now;
        if (wd->last_height > 0 && height - wd->last_height > 100) {
            log_ts("INFO", "height jump: %d → %d (+%d)",
                   wd->last_height, height, height - wd->last_height);
        }
    } else if (wd->last_height > 0 &&
               now - wd->last_height_change > 300 &&
               !synced) {
        log_ts("ALERT", "height stall: stuck at %d for %lds (sync=%s)",
               height, (long)(now - wd->last_height_change), sync_state);
        wd->alerts_total++;
    }

    /* Check 2: Height regression */
    if (height > 0 && wd->last_height > 0 && height < wd->last_height) {
        log_ts("ALERT", "height REGRESSION: %d → %d",
               wd->last_height, height);
        wd->alerts_total++;
    }

    /* Check 3: Peers */
    if (peers < 2 && wd->last_peers >= 2) {
        log_ts("ALERT", "peers dropped: %d → %d", wd->last_peers, peers);
        wd->alerts_total++;
    }

    /* Check 4: Health transition */
    if (!healthy && wd->last_healthy) {
        log_ts("ALERT", "node became unhealthy (sync=%s, h=%d, peers=%d)",
               sync_state, height, peers);
        wd->alerts_total++;
    } else if (healthy && !wd->last_healthy && wd->checks_total > 1) {
        log_ts("OK", "node recovered to healthy (h=%d, peers=%d)",
               height, peers);
    }

    /* Check 5: Sync state */
    if (strcmp(sync_state, "failed") == 0) {
        log_ts("ALERT", "sync state is FAILED at h=%d", height);
        wd->alerts_total++;
    }

    /* Check 6: SYNC_VIOLATION — peer_max - tip > 100 for > 10 min.
     * Read peer_max_height from getsyncdiag (a zclassic23-native RPC).
     * If the in-process L2 recovery didn't recover us, restart the
     * service as the outside-the-process backstop. Rate-limited to
     * one restart per 15 min so a network outage doesn't crash-loop. */
    char buf3[8192];
    int peer_max = -1;
    if (rpc_call(cookie, port, "getsyncdiag", buf3, sizeof(buf3)) >= 0)
        peer_max = json_int(buf3, "peer_max_height");

    if (peer_max > 0 && height > 0 &&
        peer_max - height > SYNC_VIOLATION_GAP) {
        if (wd->sync_violation_first_seen == 0)
            wd->sync_violation_first_seen = now;
        if (now - wd->sync_violation_first_seen >
            SYNC_VIOLATION_TRIGGER_SECS) {
            log_ts("ALERT", "SYNC_VIOLATION: tip=%d peer_max=%d gap=%d "
                   "for %lds — external restart pending",
                   height, peer_max, peer_max - height,
                   (long)(now - wd->sync_violation_first_seen));
            wd->alerts_total++;

            if (now - wd->last_restart >= EXTERNAL_RESTART_COOLDOWN) {
                log_ts("ACTION", "restarting zclassic23 service "
                       "(restart #%d, cooldown %ds)",
                       ++wd->restart_count,
                       EXTERNAL_RESTART_COOLDOWN);
                /* systemd-cat to make this visible at emerg level for
                 * any operator monitoring journalctl */
                int rc = system("systemd-cat -p emerg -t zcl-watchdog "
                                "echo 'SYNC_VIOLATION: restarting "
                                "zclassic23' ; systemctl --user restart "
                                "zclassic23");
                (void)rc;
                wd->last_restart = now;
                wd->sync_violation_first_seen = 0;  /* re-arm */
            } else {
                log_ts("INFO", "restart suppressed by cooldown "
                       "(%lds remaining)",
                       (long)(EXTERNAL_RESTART_COOLDOWN -
                              (now - wd->last_restart)));
            }
        }
    } else {
        wd->sync_violation_first_seen = 0;
    }

    /* Periodic status (every 10 minutes) */
    if (now - wd->last_alert >= 600 || wd->checks_total == 1) {
        log_ts("STATUS", "h=%d headers=%d peers=%d sync=%s healthy=%s "
               "checks=%d alerts=%d",
               height, headers, peers, sync_state,
               healthy ? "yes" : "no",
               wd->checks_total, wd->alerts_total);
        wd->last_alert = now;
    }

    wd->last_height = height;
    wd->last_peers = peers;
    wd->last_healthy = healthy;
    wd->last_synced = synced;
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *datadir = NULL;
    int interval = 30;
    int port = 18232;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0)
            datadir = argv[i] + 9;
        else if (strncmp(argv[i], "-interval=", 10) == 0)
            interval = atoi(argv[i] + 10);
        else if (strncmp(argv[i], "-rpcport=", 9) == 0)
            port = atoi(argv[i] + 9);
    }

    if (interval < 5) interval = 5;
    if (interval > 600) interval = 600;

    /* Read cookie */
    char cookie_path[512];
    const char *home = getenv("HOME");
    if (datadir)
        snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", datadir);
    else if (home)
        snprintf(cookie_path, sizeof(cookie_path),
                 "%s/.zclassic-c23/.cookie", home);
    else {
        fprintf(stderr, "No HOME or -datadir set\n");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    log_ts("START", "watchdog starting (interval=%ds port=%d cookie=%s)",
           interval, port, cookie_path);

    struct watchdog_state wd = {0};
    wd.last_height_change = time(NULL);
    wd.watchdog_start_time = time(NULL);

    while (g_running) {
        /* Re-read cookie each check (node may restart) */
        char cookie[256] = "";
        FILE *cf = fopen(cookie_path, "r");
        if (cf) {
            if (fgets(cookie, sizeof(cookie), cf)) {
                char *nl = strchr(cookie, '\n');
                if (nl) *nl = '\0';
            }
            fclose(cf);
        }

        if (cookie[0])
            watchdog_check(&wd, cookie, port);
        else
            log_ts("WARN", "no cookie at %s — node not running?",
                   cookie_path);

        for (int s = 0; s < interval && g_running; s++)
            sleep(1);
    }

    log_ts("STOP", "watchdog exiting (checks=%d alerts=%d)",
           wd.checks_total, wd.alerts_total);
    return 0;
}
