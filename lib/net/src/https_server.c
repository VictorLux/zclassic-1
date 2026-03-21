/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Public HTTPS server — serves the block explorer on port 443.
 * Uses OpenSSL for TLS. Port 80 redirects to HTTPS.
 * Thread-per-connection model (explorer pages are fast). */

#include "net/https_server.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/time.h>

static SSL_CTX *g_ssl_ctx = NULL;
static int g_https_fd = -1;
static int g_http_fd = -1;
static pthread_t g_https_thread;
static pthread_t g_http_thread;
static volatile bool g_running = false;
static char g_hostname[256] = "";

/* Connection limit — prevents OOM under heavy load.
 * Each connection mallocs 512KB for response buffer. */
#define MAX_HTTPS_CONNECTIONS 64
static _Atomic int g_active_connections = 0;

/* ── HTTP helpers ─────────────────────────────────────────── */

static bool ssl_read_line(SSL *ssl, char *buf, size_t max)
{
    size_t pos = 0;
    while (pos < max - 1) {
        char c;
        int r = SSL_read(ssl, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return true;
}

static bool plain_read_line(int fd, char *buf, size_t max)
{
    size_t pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return true;
}

/* ── HTTPS handler ────────────────────────────────────────── */

static void handle_https_client(SSL *ssl)
{
    char line[4096];
    if (!ssl_read_line(ssl, line, sizeof(line)))
        return;

    char method[16] = "", path[2048] = "";
    if (sscanf(line, "%15s %2047s", method, path) != 2)
        return;

    /* Read remaining headers (discard) */
    while (ssl_read_line(ssl, line, sizeof(line))) {
        if (line[0] == '\0') break;
    }

    /* Only serve GET requests to explorer routes */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        const char *resp =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n\r\n"
            "Only GET is supported.\n";
        SSL_write(ssl, resp, (int)strlen(resp));
        return;
    }

    /* Redirect root to explorer */
    if (strcmp(path, "/") == 0) {
        const char *resp =
            "HTTP/1.1 302 Found\r\n"
            "Location: /explorer\r\n"
            "Connection: close\r\n\r\n";
        SSL_write(ssl, resp, (int)strlen(resp));
        return;
    }

    /* Explorer + API routes — call the explorer handler (which delegates /api/) */
    if (strncmp(path, "/explorer", 9) == 0 ||
        strncmp(path, "/api", 4) == 0) {
        extern size_t explorer_handle_request(const char *, const char *,
            const unsigned char *, size_t, unsigned char *, size_t);

        unsigned char *buf = malloc(512 * 1024); /* 512 KB response buffer */
        if (!buf) return;

        size_t n = explorer_handle_request(method, path, NULL, 0, buf, 512 * 1024);
        if (n > 0) {
            /* Write in chunks — SSL_write may not accept large buffers at once */
            size_t written = 0;
            while (written < n) {
                size_t chunk = n - written;
                if (chunk > 16384) chunk = 16384;
                int w = SSL_write(ssl, buf + written, (int)chunk);
                if (w <= 0) break;
                written += (size_t)w;
            }
        } else {
            const char *resp =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n\r\n"
                "Not found.\n";
            SSL_write(ssl, resp, (int)strlen(resp));
        }
        free(buf);
        return;
    }

    /* Anything else → 404 */
    const char *resp =
        "HTTP/1.1 302 Found\r\n"
        "Location: /explorer\r\n"
        "Connection: close\r\n\r\n";
    SSL_write(ssl, resp, (int)strlen(resp));
}

struct client_arg {
    int fd;
};

static void *https_client_thread(void *arg)
{
    struct client_arg *ca = (struct client_arg *)arg;
    int fd = ca->fd;
    free(ca);

    atomic_fetch_add(&g_active_connections, 1);

    SSL *ssl = SSL_new(g_ssl_ctx);
    if (!ssl) { close(fd); atomic_fetch_sub(&g_active_connections, 1); return NULL; }

    SSL_set_fd(ssl, fd);

    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(fd);
        atomic_fetch_sub(&g_active_connections, 1);
        return NULL;
    }

    handle_https_client(ssl);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    atomic_fetch_sub(&g_active_connections, 1);
    return NULL;
}

static void *https_listen_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_https_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running && errno != EINVAL)
                perror("https accept");
            continue;
        }

        /* Reject if too many concurrent connections (prevents OOM) */
        if (atomic_load(&g_active_connections) >= MAX_HTTPS_CONNECTIONS) {
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\n"
                "Retry-After: 5\r\nConnection: close\r\n\r\n";
            write(client_fd, busy, strlen(busy));
            close(client_fd);
            continue;
        }

        /* Set timeouts — allow longer for heavy pages like HODL chart */
        struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct client_arg *ca = malloc(sizeof(*ca));
        if (!ca) { close(client_fd); continue; }
        ca->fd = client_fd;

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, https_client_thread, ca) != 0) {
            free(ca);
            close(client_fd);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ── HTTP redirect handler (port 80) ─────────────────────── */

static void *http_client_handler(void *arg)
{
    struct client_arg *ca = (struct client_arg *)arg;
    int fd = ca->fd;
    free(ca);

    /* Read the request line to get the path */
    char line[4096];
    if (!plain_read_line(fd, line, sizeof(line))) {
        close(fd);
        return NULL;
    }

    char method[16] = "", path[2048] = "";
    sscanf(line, "%15s %2047s", method, path);

    /* Drain headers */
    while (plain_read_line(fd, line, sizeof(line)))
        if (line[0] == '\0') break;

    /* ACME challenge passthrough for cert renewal */
    if (strncmp(path, "/.well-known/acme-challenge/", 28) == 0) {
        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "/var/www/html%s", path);
        /* Sanitize */
        if (strstr(filepath, "..") == NULL) {
            FILE *f = fopen(filepath, "r");
            if (f) {
                char body[4096];
                size_t n = fread(body, 1, sizeof(body), f);
                fclose(f);
                char hdr[512];
                int hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n", n);
                (void)write(fd, hdr, (size_t)hlen);
                (void)write(fd, body, n);
                close(fd);
                return NULL;
            }
        }
    }

    /* Redirect everything to HTTPS */
    char resp[4096];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://%s%s\r\n"
        "Connection: close\r\n\r\n",
        g_hostname[0] ? g_hostname : "zclnet.net", path);
    (void)write(fd, resp, (size_t)n);
    close(fd);
    return NULL;
}

static void *http_listen_fn(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_http_fd,
                                (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_running && errno != EINVAL)
                perror("http accept");
            continue;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct client_arg *ca = malloc(sizeof(*ca));
        if (!ca) { close(client_fd); continue; }
        ca->fd = client_fd;

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, http_client_handler, ca) != 0) {
            free(ca);
            close(client_fd);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ── Bind helper ──────────────────────────────────────────── */

static int bind_port(uint16_t port, bool any_addr)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = any_addr ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "HTTPS: bind port %u: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 32) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Public API ───────────────────────────────────────────── */

bool https_server_start(const char *cert_path, const char *key_path,
                         const char *hostname)
{
    signal(SIGPIPE, SIG_IGN);

    if (hostname)
        snprintf(g_hostname, sizeof(g_hostname), "%s", hostname);

    /* Init OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) {
        fprintf(stderr, "HTTPS: SSL_CTX_new failed\n");
        ERR_print_errors_fp(stderr);
        return false;
    }

    /* Set minimum TLS 1.2 */
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(g_ssl_ctx, cert_path) <= 0) {
        fprintf(stderr, "HTTPS: failed to load cert: %s\n", cert_path);
        ERR_print_errors_fp(stderr);
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "HTTPS: failed to load key: %s\n", key_path);
        ERR_print_errors_fp(stderr);
        return false;
    }
    if (!SSL_CTX_check_private_key(g_ssl_ctx)) {
        fprintf(stderr, "HTTPS: cert/key mismatch\n");
        return false;
    }

    /* Bind HTTPS port 443 */
    g_https_fd = bind_port(443, true);
    if (g_https_fd < 0) {
        fprintf(stderr, "HTTPS: cannot bind port 443 (need root or CAP_NET_BIND_SERVICE)\n");
        return false;
    }

    /* Bind HTTP port 80 (redirect) */
    g_http_fd = bind_port(80, true);
    if (g_http_fd < 0) {
        fprintf(stderr, "HTTPS: cannot bind port 80, redirect won't work\n");
        /* Non-fatal — continue with HTTPS only */
    }

    g_running = true;

    if (pthread_create(&g_https_thread, NULL, https_listen_fn, NULL) != 0) {
        perror("HTTPS: pthread_create");
        close(g_https_fd);
        g_https_fd = -1;
        g_running = false;
        return false;
    }

    if (g_http_fd >= 0) {
        if (pthread_create(&g_http_thread, NULL, http_listen_fn, NULL) != 0) {
            fprintf(stderr, "HTTPS: HTTP redirect thread failed\n");
            close(g_http_fd);
            g_http_fd = -1;
        }
    }

    printf("HTTPS server listening on 0.0.0.0:443 (TLS)\n");
    if (g_http_fd >= 0)
        printf("HTTP redirect on 0.0.0.0:80 -> https://%s\n", g_hostname);

    return true;
}

void https_server_stop(void)
{
    g_running = false;
    if (g_https_fd >= 0) {
        shutdown(g_https_fd, SHUT_RDWR);
        close(g_https_fd);
        g_https_fd = -1;
    }
    if (g_http_fd >= 0) {
        shutdown(g_http_fd, SHUT_RDWR);
        close(g_http_fd);
        g_http_fd = -1;
    }
    pthread_join(g_https_thread, NULL);
    if (g_http_fd >= 0)
        pthread_join(g_http_thread, NULL);

    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    printf("HTTPS server stopped.\n");
}

bool https_server_is_running(void)
{
    return g_running;
}
