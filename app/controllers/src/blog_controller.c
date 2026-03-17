/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Blog controller — static file server + ZSLP node registry. */

#include "controllers/blog_controller.h"
#include "sapling/slp.h"
#include "core/uint256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Static file server ─────────────────────────────────────── */

static const char *content_type_for(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "text/html; charset=utf-8";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

/* Sanitize path: no .., no absolute paths */
static bool safe_path(const char *path)
{
    if (!path || path[0] == '\0') return false;
    if (strstr(path, "..")) return false;
    if (path[0] == '/' && path[1] == '/') return false;
    return true;
}

static size_t http_response(char *out, size_t out_len,
                             int status, const char *content_type,
                             const char *body, size_t body_len)
{
    const char *status_text = (status == 200) ? "OK" :
                              (status == 404) ? "Not Found" :
                              (status == 403) ? "Forbidden" : "Error";
    int hdr_len = snprintf(out, out_len,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    if (hdr_len < 0 || (size_t)hdr_len + body_len > out_len)
        return 0;
    memcpy(out + hdr_len, body, body_len);
    return (size_t)hdr_len + body_len;
}

size_t blog_serve(const char *datadir, const char *path,
                  char *out, size_t out_len)
{
    if (!path || !out || out_len < 256) return 0;

    /* Default to index.html */
    const char *rel = path;
    if (rel[0] == '/') rel++;
    if (rel[0] == '\0') rel = "index.html";

    if (!safe_path(rel)) {
        const char *body = "<h1>403 Forbidden</h1>";
        return http_response(out, out_len, 403, "text/html",
                             body, strlen(body));
    }

    /* Read file from {datadir}/blog/{rel} */
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/blog/%s", datadir, rel);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        /* Try with .html extension */
        snprintf(filepath, sizeof(filepath), "%s/blog/%s.html", datadir, rel);
        f = fopen(filepath, "rb");
    }
    if (!f) {
        const char *body = "<h1>404 Not Found</h1>";
        return http_response(out, out_len, 404, "text/html",
                             body, strlen(body));
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || (size_t)fsize > out_len - 512) {
        fclose(f);
        const char *body = "<h1>500 File too large</h1>";
        return http_response(out, out_len, 500, "text/html",
                             body, strlen(body));
    }

    char *body = malloc((size_t)fsize);
    if (!body) { fclose(f); return 0; }
    size_t nread = fread(body, 1, (size_t)fsize, f);
    fclose(f);

    size_t result = http_response(out, out_len, 200,
                                   content_type_for(filepath),
                                   body, nread);
    free(body);
    return result;
}

/* ── ZSLP Node Registry ────────────────────────────────────── */

size_t blog_build_node_registry_genesis(uint8_t *out, size_t out_len)
{
    return slp_build_genesis(out, out_len,
        "ZCL23NODES",                /* ticker */
        "ZClassic23 Node Registry",  /* name */
        "",                           /* document_url */
        NULL,                         /* document_hash */
        0,                            /* decimals */
        2,                            /* mint_baton_vout */
        1);                           /* initial_quantity */
}

size_t blog_build_node_announce(uint8_t *out, size_t out_len,
                                 const uint8_t token_id[32],
                                 const char *onion_hostname)
{
    /* Encode .onion hostname as a SEND with quantity=1.
     * The hostname is stored in the OP_RETURN after the SLP data
     * as an additional push. This is non-standard SLP but allows
     * any node to parse it by reading past the SLP fields. */
    struct uint256 tid;
    memcpy(tid.data, token_id, 32);
    uint64_t qty = 1;
    size_t slp_len = slp_build_send(out, out_len, &tid, &qty, 1);
    if (slp_len == 0 || !onion_hostname) return slp_len;

    /* Append hostname as additional pushdata */
    size_t hlen = strlen(onion_hostname);
    if (slp_len + 1 + hlen > out_len) return slp_len;
    out[slp_len] = (uint8_t)hlen;
    memcpy(out + slp_len + 1, onion_hostname, hlen);
    return slp_len + 1 + hlen;
}

int blog_discover_onion_peers(const char *datadir,
                               struct onion_peer *out, size_t max)
{
    (void)datadir;
    /* TODO: scan block chain for ZCL23NODES SEND txs,
     * parse .onion hostnames from OP_RETURN data after SLP fields.
     * For now, return 0 (no peers discovered). */
    if (out && max > 0)
        memset(out, 0, sizeof(out[0]));
    return 0;
}
