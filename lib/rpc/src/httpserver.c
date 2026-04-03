/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/httpserver.h"
#include "json/json.h"
#include "rpc/protocol.h"
#include "core/random.h"
#include "encoding/utilstrencodings.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RPC_HTTP_WORKERS 4
#define RPC_HTTP_QUEUE_CAP 64

static int g_listen_fd = -1;
static const struct rpc_table *g_table = NULL;
static pthread_t g_listen_thread;
static bool g_listen_thread_started = false;
static volatile bool g_running = false;
static char g_rpc_user[128];
static char g_rpc_password[128];
static char g_cookie_file[1024];
static bool g_auth_required = false;
static pthread_t g_worker_threads[RPC_HTTP_WORKERS];
static size_t g_workers_started = 0;
static int g_client_queue[RPC_HTTP_QUEUE_CAP];
static size_t g_client_queue_head = 0;
static size_t g_client_queue_tail = 0;
static size_t g_client_queue_count = 0;
static pthread_mutex_t g_client_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_client_queue_cond = PTHREAD_COND_INITIALIZER;

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_decode(const char *in, size_t inlen,
                             unsigned char *out, size_t outmax)
{
    unsigned char table[256];
    memset(table, 64, sizeof(table));
    for (int i = 0; i < 64; i++)
        table[(unsigned char)base64_chars[i]] = (unsigned char)i;

    size_t olen = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < inlen && olen < outmax; i++) {
        unsigned char c = table[(unsigned char)in[i]];
        if (c == 64) continue;
        buf = (buf << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[olen++] = (unsigned char)(buf >> bits);
        }
    }
    return olen;
}

static bool check_auth(const char *auth_header)
{
    if (!g_auth_required) return true;
    if (!auth_header) return false;

    while (*auth_header == ' ') auth_header++;
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;
    const char *b64 = auth_header + 6;
    while (*b64 == ' ') b64++;

    unsigned char decoded[512];
    size_t dlen = base64_decode(b64, strlen(b64), decoded, sizeof(decoded) - 1);
    decoded[dlen] = '\0';

    char expected[512];
    snprintf(expected, sizeof(expected), "%s:%s", g_rpc_user, g_rpc_password);
    return strcmp((const char *)decoded, expected) == 0;
}

static bool read_line(int fd, char *buf, size_t buflen)
{
    size_t pos = 0;
    while (pos < buflen - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') {
            if (pos > 0 && buf[pos - 1] == '\r')
                pos--;
            buf[pos] = '\0';
            return true;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return true;
}

static bool read_exact(int fd, char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t r = read(fd, buf + total, len - total);
        if (r <= 0) return false;
        total += (size_t)r;
    }
    return true;
}

static void send_response(int fd, int status_code, const char *status_text,
                            const char *body, size_t body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, body_len);
    (void)write(fd, header, (size_t)hlen);
    if (body_len > 0)
        (void)write(fd, body, body_len);
}

static bool enqueue_client_fd(int client_fd)
{
    bool ok = false;

    pthread_mutex_lock(&g_client_queue_mutex);
    if (g_client_queue_count < RPC_HTTP_QUEUE_CAP) {
        g_client_queue[g_client_queue_tail] = client_fd;
        g_client_queue_tail =
            (g_client_queue_tail + 1) % RPC_HTTP_QUEUE_CAP;
        g_client_queue_count++;
        ok = true;
        pthread_cond_signal(&g_client_queue_cond);
    }
    pthread_mutex_unlock(&g_client_queue_mutex);

    return ok;
}

static int dequeue_client_fd(void)
{
    int client_fd = -1;

    pthread_mutex_lock(&g_client_queue_mutex);
    while (g_client_queue_count == 0 && g_running)
        pthread_cond_wait(&g_client_queue_cond, &g_client_queue_mutex);

    if (g_client_queue_count > 0) {
        client_fd = g_client_queue[g_client_queue_head];
        g_client_queue_head =
            (g_client_queue_head + 1) % RPC_HTTP_QUEUE_CAP;
        g_client_queue_count--;
    }
    pthread_mutex_unlock(&g_client_queue_mutex);

    return client_fd;
}

static void handle_client(int client_fd)
{
    /* Set socket timeout to prevent slowloris attacks.
     * 5 seconds to send complete request — generous for local RPC,
     * fatal for attackers trying to hold connections open. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char method[16];
    char path[256];
    char line[4096];

    if (!read_line(client_fd, line, sizeof(line)))
        goto done;

    if (sscanf(line, "%15s %255s", method, path) != 2)
        goto done;

    /* No blog over clearnet. Blog is Tor-only via dynhost.
     * Clearnet serves ONLY authenticated RPC (POST). */
    if (strcmp(method, "POST") != 0) {
        const char *msg = "Method not allowed";
        send_response(client_fd, 405, "Method Not Allowed", msg, strlen(msg));
        goto done;
    }

    size_t content_length = 0;
    char auth_value[512] = {0};
    while (read_line(client_fd, line, sizeof(line))) {
        if (line[0] == '\0') break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *endp = NULL;
            long v = strtol(line + 15, &endp, 10);
            if (endp == line + 15 || v < 0 || v > 10 * 1024 * 1024)
                content_length = 0;
            else
                content_length = (size_t)v;
        }
        if (strncasecmp(line, "Authorization:", 14) == 0)
            snprintf(auth_value, sizeof(auth_value), "%s", line + 14);
    }

    if (!check_auth(auth_value[0] ? auth_value : NULL)) {
        const char *msg = "Unauthorized";
        send_response(client_fd, 401, "Unauthorized", msg, strlen(msg));
        goto done;
    }

    if (content_length == 0 || content_length > 10 * 1024 * 1024) {
        if (content_length > 10 * 1024 * 1024) {
            const char *msg = "{\"error\":\"Request body too large\"}";
            send_response(client_fd, 413, "Payload Too Large", msg, strlen(msg));
        }
        goto done;
    }

    char *body = malloc(content_length + 1);
    if (!body) goto done;

    if (!read_exact(client_fd, body, content_length)) {
        free(body);
        goto done;
    }
    body[content_length] = '\0';

    struct json_value request;
    json_init(&request);
    if (!json_read(&request, body, content_length)) {
        free(body);
        json_free(&request);
        const char *err = "{\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}";
        send_response(client_fd, 200, "OK", err, strlen(err));
        goto done;
    }
    free(body);

    struct json_request req;
    json_request_init(&req);
    if (!json_request_parse(&req, &request)) {
        json_free(&request);
        json_request_free(&req);
        const char *err = "{\"error\":{\"code\":-32600,\"message\":\"Invalid request\"}}";
        send_response(client_fd, 200, "OK", err, strlen(err));
        goto done;
    }
    json_free(&request);

    struct json_value result;
    json_init(&result);
    rpc_table_execute(g_table, req.method, &req.params, &result);

    struct json_value response;
    json_set_object(&response);

    json_push_kv(&response, "result", &result);

    struct json_value null_err;
    json_set_null(&null_err);
    json_push_kv(&response, "error", &null_err);
    json_free(&null_err);

    json_push_kv(&response, "id", &req.id);

    char *resp_buf = malloc(4 * 1024 * 1024);
    if (resp_buf) {
        size_t resp_len = json_write(&response, resp_buf, 4 * 1024 * 1024);
        send_response(client_fd, 200, "OK", resp_buf, resp_len);
        free(resp_buf);
    } else {
        const char *oom = "{\"error\":\"Internal error: out of memory\"}";
        send_response(client_fd, 500, "Internal Server Error", oom, strlen(oom));
    }

    json_free(&result);
    json_free(&response);
    json_request_free(&req);

done:
    close(client_fd);
}

static void *rpc_worker_thread_fn(void *arg)
{
    (void)arg;

    while (g_running) {
        int client_fd = dequeue_client_fd();
        if (client_fd < 0)
            continue;
        handle_client(client_fd);
    }

    return NULL;
}

static void *listen_thread_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running)
                perror("accept");
            continue;
        }
        if (!enqueue_client_fd(client_fd)) {
            const char *msg = "{\"error\":\"RPC server busy\"}";
            send_response(client_fd, 503, "Service Unavailable",
                          msg, strlen(msg));
            close(client_fd);
        }
    }
    return NULL;
}

bool rpc_http_start(const struct rpc_table *table, uint16_t port,
                     const char *rpc_user, const char *rpc_password,
                     const char *datadir)
{
    if (g_running || g_listen_thread_started || g_workers_started > 0)
        return false;

    g_client_queue_head = 0;
    g_client_queue_tail = 0;
    g_client_queue_count = 0;
    g_table = table;
    g_rpc_user[0] = '\0';
    g_rpc_password[0] = '\0';
    g_cookie_file[0] = '\0';
    g_auth_required = false;
    if (rpc_user && rpc_password) {
        snprintf(g_rpc_user, sizeof(g_rpc_user), "%s", rpc_user);
        snprintf(g_rpc_password, sizeof(g_rpc_password), "%s", rpc_password);
        g_auth_required = true;
    } else if (datadir) {
        snprintf(g_rpc_user, sizeof(g_rpc_user), "__cookie__");
        uint64_t r1 = GetRand(UINT64_MAX);
        uint64_t r2 = GetRand(UINT64_MAX);
        snprintf(g_rpc_password, sizeof(g_rpc_password),
                 "%016llx%016llx",
                 (unsigned long long)r1, (unsigned long long)r2);
        g_auth_required = true;

        snprintf(g_cookie_file, sizeof(g_cookie_file),
                 "%s/.cookie", datadir);
        FILE *f = fopen(g_cookie_file, "w");
        if (f) {
            fprintf(f, "%s:%s", g_rpc_user, g_rpc_password);
            fclose(f);
            printf("RPC cookie written to %s\n", g_cookie_file);
        }
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return false;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    if (listen(g_listen_fd, 8) < 0) {
        perror("listen");
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    g_running = true;
    printf("RPC server listening on 127.0.0.1:%u\n", port);

    for (size_t i = 0; i < RPC_HTTP_WORKERS; i++) {
        if (pthread_create(&g_worker_threads[i], NULL,
                           rpc_worker_thread_fn, NULL) != 0) {
            perror("pthread_create");
            g_running = false;
            shutdown(g_listen_fd, SHUT_RDWR);
            close(g_listen_fd);
            g_listen_fd = -1;
            pthread_mutex_lock(&g_client_queue_mutex);
            pthread_cond_broadcast(&g_client_queue_cond);
            pthread_mutex_unlock(&g_client_queue_mutex);
            for (size_t j = 0; j < i; j++)
                pthread_join(g_worker_threads[j], NULL);
            g_workers_started = 0;
            return false;
        }
        g_workers_started = i + 1;
    }

    if (pthread_create(&g_listen_thread, NULL, listen_thread_fn, NULL) != 0) {
        perror("pthread_create");
        g_running = false;
        pthread_mutex_lock(&g_client_queue_mutex);
        pthread_cond_broadcast(&g_client_queue_cond);
        pthread_mutex_unlock(&g_client_queue_mutex);
        for (size_t i = 0; i < g_workers_started; i++)
            pthread_join(g_worker_threads[i], NULL);
        g_workers_started = 0;
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }
    g_listen_thread_started = true;

    return true;
}

void rpc_http_stop(void)
{
    g_running = false;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_mutex_lock(&g_client_queue_mutex);
    pthread_cond_broadcast(&g_client_queue_cond);
    pthread_mutex_unlock(&g_client_queue_mutex);
    if (g_listen_thread_started) {
        pthread_join(g_listen_thread, NULL);
        g_listen_thread_started = false;
    }
    if (g_workers_started > 0) {
        for (size_t i = 0; i < g_workers_started; i++)
            pthread_join(g_worker_threads[i], NULL);
        g_workers_started = 0;
    }

    pthread_mutex_lock(&g_client_queue_mutex);
    while (g_client_queue_count > 0) {
        int client_fd = g_client_queue[g_client_queue_head];
        g_client_queue_head =
            (g_client_queue_head + 1) % RPC_HTTP_QUEUE_CAP;
        g_client_queue_count--;
        close(client_fd);
    }
    g_client_queue_head = 0;
    g_client_queue_tail = 0;
    pthread_mutex_unlock(&g_client_queue_mutex);

    if (g_cookie_file[0]) {
        unlink(g_cookie_file);
        g_cookie_file[0] = '\0';
    }
    g_table = NULL;
    g_auth_required = false;
    g_rpc_user[0] = '\0';
    g_rpc_password[0] = '\0';
    printf("RPC server stopped.\n");
}

bool rpc_http_is_running(void)
{
    return g_running;
}
