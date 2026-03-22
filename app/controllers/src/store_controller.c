/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store controller — ZSLP token commerce. */

#include "controllers/store_controller.h"
#include "controllers/zslp_controller.h"
#include "util/template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

/* Forward declarations */
static size_t serve_gated_content(sqlite3 *db, const char *customer_addr,
                                   const char *token_id, uint64_t required,
                                   const char *datadir,
                                   uint8_t *resp, size_t max);

/* Ensure store tables exist */
static void store_ensure_schema(sqlite3 *db, const char *datadir)
{
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS products ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "description TEXT,"
        "price_zatoshi INTEGER NOT NULL,"
        "token_id TEXT,"
        "tokens_per_purchase INTEGER NOT NULL DEFAULT 1,"
        "active INTEGER NOT NULL DEFAULT 1"
        ")", NULL, NULL, NULL);

    /* Load products from {datadir}/store/products.json if it exists,
     * otherwise seed with demo products. This lets node operators
     * customize their store by editing a simple JSON file. */
    sqlite3_stmt *cnt = NULL;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM products", -1, &cnt, NULL);
    bool empty = (sqlite3_step(cnt) == SQLITE_ROW &&
                  sqlite3_column_int(cnt, 0) == 0);
    sqlite3_finalize(cnt);

    if (empty && datadir) {
        char json_path[1024];
        snprintf(json_path, sizeof(json_path), "%s/store/products.json", datadir);
        FILE *f = fopen(json_path, "r");
        if (f) {
            /* Parse simple JSON array of products:
             * [{"name":"...","description":"...","price_zcl":0.01,
             *   "token_id":"...","tokens_per_purchase":1}, ...] */
            char buf[16384];
            size_t len = fread(buf, 1, sizeof(buf) - 1, f);
            buf[len] = '\0';
            fclose(f);

            /* Simple JSON array parser — find each {...} object */
            const char *p = buf;
            int loaded = 0;
            while ((p = strchr(p, '{')) != NULL) {
                const char *end = strchr(p, '}');
                if (!end) break;

                /* Extract fields with simple string search */
                char name[256] = "", desc[1024] = "", token[64] = "";
                double price_zcl = 0.0;
                int tokens = 1;

                /* name */
                const char *q = strstr(p, "\"name\"");
                if (q && q < end) {
                    q = strchr(q + 6, '"'); if (q) { q++;
                    const char *e = strchr(q, '"');
                    if (e && (size_t)(e-q) < sizeof(name)) {
                        memcpy(name, q, (size_t)(e-q)); name[e-q] = '\0';
                    }}
                }
                /* description */
                q = strstr(p, "\"description\"");
                if (q && q < end) {
                    q = strchr(q + 13, '"'); if (q) { q++;
                    const char *e = strchr(q, '"');
                    if (e && (size_t)(e-q) < sizeof(desc)) {
                        memcpy(desc, q, (size_t)(e-q)); desc[e-q] = '\0';
                    }}
                }
                /* token_id */
                q = strstr(p, "\"token_id\"");
                if (q && q < end) {
                    q = strchr(q + 10, '"'); if (q) { q++;
                    const char *e = strchr(q, '"');
                    if (e && (size_t)(e-q) < sizeof(token)) {
                        memcpy(token, q, (size_t)(e-q)); token[e-q] = '\0';
                    }}
                }
                /* price_zcl */
                q = strstr(p, "\"price_zcl\"");
                if (q && q < end) {
                    q += 11; while (*q == ':' || *q == ' ') q++;
                    price_zcl = strtod(q, NULL);
                }
                /* tokens_per_purchase */
                q = strstr(p, "\"tokens_per_purchase\"");
                if (q && q < end) {
                    q += 20; while (*q == ':' || *q == ' ') q++;
                    long tval = strtol(q, NULL, 10);
                    tokens = (tval > 0 && tval <= 10000) ? (int)tval : 1;
                }

                if (name[0] && price_zcl > 0) {
                    int64_t price_sat = (int64_t)(price_zcl * 100000000.0);
                    sqlite3_stmt *ins = NULL;
                    if (sqlite3_prepare_v2(db,
                        "INSERT INTO products (name, description, price_zatoshi, "
                        "token_id, tokens_per_purchase) VALUES (?,?,?,?,?)",
                        -1, &ins, NULL) != SQLITE_OK || !ins) {
                        fprintf(stderr, "store: failed to prepare product insert\n");
                        p = end + 1;
                        continue;
                    }
                    sqlite3_bind_text(ins, 1, name, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, desc, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 3, price_sat);
                    sqlite3_bind_text(ins, 4, token, -1, SQLITE_STATIC);
                    sqlite3_bind_int(ins, 5, tokens);
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                    loaded++;
                }
                p = end + 1;
            }
            if (loaded > 0)
                printf("Store: loaded %d products from %s\n", loaded, json_path);
            else
                printf("Store: %s exists but no valid products found\n", json_path);
            fflush(stdout);
            empty = (loaded == 0);
        }
    }

    /* Fallback: seed demo products if still empty */
    if (empty) {
        sqlite3_exec(db,
            "INSERT INTO products (name, description, price_zatoshi, "
            "token_id, tokens_per_purchase) VALUES "
            "('ZCL23 Access Token', "
            "'1 token grants access to premium .onion services on the "
            "ZClassic23 network. Tokens are ZSLP tokens on the ZClassic "
            "blockchain.', "
            "1000000, 'ZCL23ACCESS', 10)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO products (name, description, price_zatoshi, "
            "token_id, tokens_per_purchase) VALUES "
            "('VPN Credit (1 month)', "
            "'Route traffic through the ZClassic23 onion network. "
            "1 month of encrypted relay service.', "
            "5000000, 'ZCL23VPN', 1)",
            NULL, NULL, NULL);
        sqlite3_exec(db,
            "INSERT INTO products (name, description, price_zatoshi, "
            "token_id, tokens_per_purchase) VALUES "
            "('Storage (1 GB)', "
            "'Encrypted storage on the ZClassic23 distributed network. "
            "Data replicated across multiple .onion nodes.', "
            "2000000, 'ZCL23STORE', 1)",
            NULL, NULL, NULL);
    }

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS orders ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "product_id INTEGER NOT NULL,"
        "customer_addr TEXT,"
        "payment_addr TEXT NOT NULL,"
        "amount_zatoshi INTEGER NOT NULL,"
        "payment_txid TEXT,"
        "mint_txid TEXT,"
        "status INTEGER NOT NULL DEFAULT 0,"
        "created_at INTEGER NOT NULL,"
        "paid_at INTEGER"
        ")", NULL, NULL, NULL);
}

/* Get the .onion address from the onion service layer (may be NULL). */
static const char *store_get_onion_address(void)
{
    extern const char *onion_service_get_address(void);
    return onion_service_get_address();
}

/* HTML body start (no HTTP headers — those are added by store_wrap_response) */
static int html_body_start(char *buf, size_t max, const char *title)
{
    const char *onion = store_get_onion_address();
    return snprintf(buf, max,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>%s</title><style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88}h2{color:#00cc66}"
        "a{color:#00aaff;text-decoration:none}"
        ".header-nav{display:flex;align-items:center;gap:16px;"
        "border-bottom:1px solid #333;padding-bottom:12px;margin-bottom:16px;"
        "flex-wrap:wrap}"
        ".header-nav a{font-size:13px}"
        ".onion-id{font-size:11px;color:#666;word-break:break-all}"
        ".product{background:#1a1a1a;padding:20px;margin:15px 0;"
        "border-radius:8px;border-left:3px solid #00ff88}"
        ".price{color:#00ff88;font-size:20px;font-weight:bold}"
        ".btn{display:inline-block;background:#00ff88;color:#0a0a0a;"
        "padding:10px 20px;border-radius:4px;font-weight:bold;margin-top:10px}"
        ".addr{background:#111;padding:10px;border-radius:4px;word-break:break-all;"
        "font-size:12px;margin:10px 0}"
        ".status{padding:8px 16px;border-radius:4px;display:inline-block}"
        ".pending{background:#333;color:#ff8800}"
        ".paid{background:#1a3a1a;color:#00ff88}"
        "input{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:8px;font-family:monospace;width:100%%;margin:5px 0;"
        "box-sizing:border-box}"
        "</style></head><body>"
        "<div class='header-nav'>"
        "<h1 style='margin:0'><a href='/store'>ZCL Store</a></h1>"
        "<a href='/'>Home</a>"
        "<a href='/store'>Products</a>"
        "%s%s%s"
        "</div>",
        title,
        onion ? "<div class='onion-id'>" : "",
        onion ? onion : "",
        onion ? "</div>" : "");
}

/* Wrap an HTML body with HTTP headers including Content-Length. */
static size_t store_wrap_response(const char *body, size_t body_len,
                                   const char *status,
                                   const char *content_type,
                                   uint8_t *resp, size_t max)
{
    return (size_t)snprintf((char *)resp, max,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%.*s",
        status, content_type, body_len,
        (int)body_len, body);
}

/* Convenience: wrap a 200 OK HTML response with Content-Length. */
static size_t store_html_response(const char *body, size_t body_len,
                                   uint8_t *resp, size_t max)
{
    return store_wrap_response(body, body_len,
        "200 OK", "text/html; charset=utf-8", resp, max);
}

/* Convenience: wrap an error HTML response with Content-Length. */
static size_t store_error_response(const char *status_code,
                                    const char *body, size_t body_len,
                                    uint8_t *resp, size_t max)
{
    return store_wrap_response(body, body_len,
        status_code, "text/html; charset=utf-8", resp, max);
}

/* Product card template: {{var}} = HTML-escaped, {{{var}}} = raw. */
static const char PRODUCT_CARD_TEMPLATE[] =
    "<div class='product'>"
    "<h3><a href='/store/product/{{id}}'>{{name}}</a></h3>"
    "<p>{{description}}</p>"
    "<div class='price'>{{price}} ZCL</div>"
    "<a href='/store/product/{{id}}' class='btn'>View</a>"
    "</div>";

/* GET /store — list products */
static size_t serve_product_list(sqlite3 *db, uint8_t *resp, size_t max)
{
    char body[16384];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "ZCL Store");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Products</h2>");
    if (n > 0) off += (size_t)n;

    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, name, description, price_zatoshi FROM products "
        "WHERE active=1 ORDER BY id", -1, &s, NULL);

    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && off + 1024 < sizeof(body)) {
        int64_t id = sqlite3_column_int64(s, 0);
        const char *name = (const char *)sqlite3_column_text(s, 1);
        const char *desc = (const char *)sqlite3_column_text(s, 2);
        int64_t price = sqlite3_column_int64(s, 3);

        char id_str[32], price_str[32];
        snprintf(id_str, sizeof(id_str), "%lld", (long long)id);
        snprintf(price_str, sizeof(price_str), "%.8f", (double)price / 1e8);

        struct template_var vars[] = {
            { "id",          id_str },
            { "name",        name ? name : "?" },
            { "description", desc ? desc : "" },
            { "price",       price_str },
        };

        size_t rendered = template_render(PRODUCT_CARD_TEMPLATE,
            vars, sizeof(vars) / sizeof(vars[0]),
            body + off, sizeof(body) - off);
        off += rendered;
        count++;
    }
    sqlite3_finalize(s);

    if (count == 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<p style='color:#666'>No products yet. "
            "Add products to the SQLite database.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off, "</body></html>");
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* GET /store/product/:id — product detail */
static size_t serve_product_detail(sqlite3 *db, int64_t product_id,
                                    uint8_t *resp, size_t max)
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT name, description, price_zatoshi, token_id, tokens_per_purchase "
        "FROM products WHERE id=? AND active=1", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, product_id);

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        const char *body = "<h1>Product not found</h1>"
            "<p><a href='/store'>Back to store</a></p>";
        return store_error_response("404 Not Found", body, strlen(body),
                                     resp, max);
    }

    const char *name = (const char *)sqlite3_column_text(s, 0);
    const char *desc = (const char *)sqlite3_column_text(s, 1);
    int64_t price = sqlite3_column_int64(s, 2);
    const char *token = (const char *)sqlite3_column_text(s, 3);
    int64_t tokens = sqlite3_column_int64(s, 4);

    char safe_name[256], safe_desc[512], safe_token[128];
    html_escape(safe_name, sizeof(safe_name), name ? name : "?");
    html_escape(safe_desc, sizeof(safe_desc), desc ? desc : "");
    html_escape(safe_token, sizeof(safe_token), token ? token : "TOKENS");

    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), safe_name);
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<div class='product'>"
        "<h2>%s</h2>"
        "<p>%s</p>"
        "<div class='price'>%.8f ZCL</div>"
        "<p>You will receive <b>%lld</b> %s tokens.</p>"
        "<h3>Purchase</h3>"
        "<form method='post' action='/store/buy/%lld'>"
        "<label>Your t-address (to receive tokens):</label>"
        "<input type='text' name='customer_addr' placeholder='t1...' required>"
        "<br><br>"
        "<button type='submit' class='btn'>Generate Payment Address</button>"
        "</form>"
        "</div>"
        "<p><a href='/store'>&larr; Back to store</a></p>"
        "</body></html>",
        safe_name,
        safe_desc,
        (double)price / 1e8,
        (long long)tokens,
        safe_token,
        (long long)product_id);
    if (n > 0) off += (size_t)n;

    sqlite3_finalize(s);
    return store_html_response(body, off, resp, max);
}

/* POST /store/buy/:id — create order */
static size_t serve_create_order(sqlite3 *db, int64_t product_id,
                                  const char *customer_addr,
                                  const char *datadir,
                                  uint8_t *resp, size_t max)
{
    /* Look up product */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT price_zatoshi FROM products WHERE id=? AND active=1",
        -1, &s, NULL);
    sqlite3_bind_int64(s, 1, product_id);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n<h1>Product not found</h1>");
    }
    int64_t price = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    /* Generate a unique Sapling z-address for this payment.
     * NEVER fall back to a fake address — that loses user funds. */
    char payment_addr[128];
    if (!zslp_generate_payment_address(datadir, payment_addr,
                                        sizeof(payment_addr))) {
        printf("store: CRITICAL — z-address generation failed for product %lld\n",
               (long long)product_id);
        fflush(stdout);
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<h1>Payment Temporarily Unavailable</h1>"
            "<p>The node is still loading cryptographic keys. "
            "Please try again in a few minutes.</p>"
            "<p><a href='/store'>Back to Store</a></p>");
    }

    /* Create order */
    sqlite3_prepare_v2(db,
        "INSERT INTO orders (product_id, customer_addr, payment_addr, "
        "amount_zatoshi, status, created_at) VALUES (?,?,?,?,0,?)",
        -1, &s, NULL);
    sqlite3_bind_int64(s, 1, product_id);
    sqlite3_bind_text(s, 2, customer_addr ? customer_addr : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, payment_addr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 4, price);
    sqlite3_bind_int64(s, 5, (int64_t)time(NULL));
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) {
        printf("store: order INSERT failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(s);
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n<h1>Order creation failed</h1>");
    }
    int64_t order_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(s);

    /* Show payment page */
    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Payment");
    if (n > 0) off = (size_t)n;

    char safe_pay[256], safe_cust[256];
    html_escape(safe_pay, sizeof(safe_pay), payment_addr);
    html_escape(safe_cust, sizeof(safe_cust),
                customer_addr ? customer_addr : "(not provided)");

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Order #%lld</h2>"
        "<div class='product'>"
        "<p>Send exactly <span class='price'>%.8f ZCL</span> to:</p>"
        "<div class='addr'>%s</div>"
        "<p>After payment confirms, tokens will be sent to:</p>"
        "<div class='addr'>%s</div>"
        "<p><a href='/store/order/%lld'>Check payment status</a></p>"
        "</div>"
        "<p><a href='/store'>&larr; Back to store</a></p>"
        "</body></html>",
        (long long)order_id,
        (double)price / 1e8,
        safe_pay,
        safe_cust,
        (long long)order_id);
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* GET /store/order/:id — check status */
static size_t serve_order_status(sqlite3 *db, int64_t order_id,
                                  uint8_t *resp, size_t max)
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT status, amount_zatoshi, payment_addr, customer_addr, "
        "payment_txid, mint_txid FROM orders WHERE id=?", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, order_id);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        const char *body = "<h1>Order not found</h1>"
            "<p><a href='/store'>Back to store</a></p>";
        return store_error_response("404 Not Found", body, strlen(body),
                                     resp, max);
    }

    int status = sqlite3_column_int(s, 0);
    int64_t amount = sqlite3_column_int64(s, 1);
    const char *pay_addr = (const char *)sqlite3_column_text(s, 2);
    const char *cust_addr = (const char *)sqlite3_column_text(s, 3);
    const char *pay_txid = (const char *)sqlite3_column_text(s, 4);
    const char *mint_txid = (const char *)sqlite3_column_text(s, 5);

    const char *status_text = status == 0 ? "Pending Payment" :
                               status == 1 ? "Payment Received" :
                               status == 2 ? "Tokens Sent" :
                               status == 3 ? "Mint Failed (contact support)" :
                               "Unknown";
    const char *status_class = status == 0 ? "pending" :
                                status >= 1 ? "paid" : "pending";

    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Order Status");
    if (n > 0) off = (size_t)n;

    char safe_pay[256], safe_cust[256];
    char safe_ptxid[256], safe_mtxid[256];
    html_escape(safe_pay, sizeof(safe_pay), pay_addr ? pay_addr : "?");
    html_escape(safe_cust, sizeof(safe_cust), cust_addr ? cust_addr : "?");
    html_escape(safe_ptxid, sizeof(safe_ptxid), pay_txid ? pay_txid : "");
    html_escape(safe_mtxid, sizeof(safe_mtxid), mint_txid ? mint_txid : "");

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Order #%lld</h2>"
        "<div class='product'>"
        "<div class='status %s'>%s</div>"
        "<p>Amount: <span class='price'>%.8f ZCL</span></p>"
        "<p>Payment address:</p><div class='addr'>%s</div>"
        "<p>Deliver to:</p><div class='addr'>%s</div>"
        "%s%s%s%s%s%s"
        "<p><a href='/store/order/%lld'>Refresh</a> | "
        "<a href='/store'>&larr; Back to store</a></p>"
        "</div></body></html>",
        (long long)order_id,
        status_class, status_text,
        (double)amount / 1e8,
        safe_pay,
        safe_cust,
        pay_txid ? "<p>Payment: <code>" : "",
        pay_txid ? safe_ptxid : "",
        pay_txid ? "</code></p>" : "",
        mint_txid ? "<p>Mint: <code>" : "",
        mint_txid ? safe_mtxid : "",
        mint_txid ? "</code></p>" : "",
        (long long)order_id);
    if (n > 0) off += (size_t)n;
    sqlite3_finalize(s);

    return store_html_response(body, off, resp, max);
}

/* Parse URL parameter: extract number after last '/' */
static int64_t parse_id_from_path(const char *path)
{
    const char *last = strrchr(path, '/');
    if (!last) return -1;
    return (int64_t)atoll(last + 1);
}

/* Validate address: alphanumeric + limited special chars only.
 * Prevents XSS via customer_addr in HTML output. */
static bool validate_address(const char *addr)
{
    if (!addr || !addr[0]) return false;
    for (size_t i = 0; addr[i]; i++) {
        char c = addr[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') continue;
        return false;
    }
    return true;
}

/* Parse POST body for customer_addr=value */
static const char *parse_form_field(const char *body, size_t len,
                                     const char *field, char *out, size_t outmax)
{
    if (!body || !len || !field || !out || outmax == 0) return NULL;
    char search[128];
    snprintf(search, sizeof(search), "%s=", field);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    /* Compute remaining bytes in body from current position */
    size_t remaining = len - (size_t)(p - body);
    size_t i = 0;
    while (i < outmax - 1 && i < remaining && p[i] && p[i] != '&' && p[i] != ' ') {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

/* Main request handler */
size_t store_handle_request(const char *method, const char *path,
                             const uint8_t *body, size_t body_len,
                             uint8_t *response, size_t response_max,
                             const char *datadir)
{
    if (!path || !response) return 0;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return 0;
    sqlite3_busy_timeout(db, 5000);
    store_ensure_schema(db, datadir);

    size_t result = 0;

    if (strcmp(path, "/store") == 0 || strcmp(path, "/store/") == 0) {
        result = serve_product_list(db, response, response_max);

    } else if (strncmp(path, "/store/product/", 15) == 0) {
        int64_t id = parse_id_from_path(path);
        result = serve_product_detail(db, id, response, response_max);

    } else if (strncmp(path, "/store/buy/", 11) == 0 &&
               method && strcmp(method, "POST") == 0) {
        int64_t id = parse_id_from_path(path);
        char addr[128] = "";
        if (body && body_len > 0)
            parse_form_field((const char *)body, body_len,
                             "customer_addr", addr, sizeof(addr));
        if (!validate_address(addr)) {
            const char *err_body = "<h1>Invalid address</h1>"
                "<p>Address must be alphanumeric.</p>"
                "<p><a href='/store'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_create_order(db, id, addr, datadir,
                                          response, response_max);
        }

    } else if (strncmp(path, "/store/order/", 13) == 0) {
        int64_t id = parse_id_from_path(path);
        result = serve_order_status(db, id, response, response_max);

    } else if (strncmp(path, "/store/access", 13) == 0) {
        /* Token-gated content: /store/access?addr=t1...&token=ZCL23ACCESS */
        char addr[128] = "", token[64] = "";
        const char *a = strstr(path, "addr=");
        const char *t = strstr(path, "token=");
        if (a) { a += 5; size_t i = 0; while (a[i] && a[i] != '&' && i < 127) { addr[i] = a[i]; i++; } addr[i] = '\0'; }
        if (t) { t += 6; size_t i = 0; while (t[i] && t[i] != '&' && i < 63) { token[i] = t[i]; i++; } token[i] = '\0'; }
        if (!token[0]) snprintf(token, sizeof(token), "ZCL23ACCESS");
        result = serve_gated_content(db, addr, token, 1, datadir,
                                      response, response_max);
    }

    sqlite3_close(db);
    return result;
}

/* Background payment processor — called periodically from boot.c.
 * Checks pending orders for payments, mints tokens when paid. */
void store_process_payments(const char *datadir)
{
    if (!datadir) return;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return;
    sqlite3_busy_timeout(db, 5000);

    /* Find pending orders */
    sqlite3_stmt *pending = NULL;
    sqlite3_prepare_v2(db,
        "SELECT o.id, o.payment_addr, o.amount_zatoshi, o.customer_addr, "
        "p.token_id, p.tokens_per_purchase "
        "FROM orders o JOIN products p ON o.product_id = p.id "
        "WHERE o.status = 0 AND o.created_at > strftime('%%s','now') - 3600",
        -1, &pending, NULL);

    while (sqlite3_step(pending) == SQLITE_ROW) {
        int64_t order_id = sqlite3_column_int64(pending, 0);
        const char *pay_addr = (const char *)sqlite3_column_text(pending, 1);
        int64_t expected = sqlite3_column_int64(pending, 2);
        const char *cust_addr = (const char *)sqlite3_column_text(pending, 3);
        const char *token_id = (const char *)sqlite3_column_text(pending, 4);
        int64_t tokens = sqlite3_column_int64(pending, 5);

        if (!pay_addr || !cust_addr || !token_id) continue;

        /* Check if payment received at the order's z-address.
         * Primary: match notes by address (works with real z-addresses).
         * Fallback: match by exact amount (for placeholder z-addresses). */
        int64_t received = 0;
        sqlite3_stmt *check = NULL;

        /* Require minimum 3 confirmations to prevent reorg-based
         * double-spend: payment reversed but tokens already minted. */
        int64_t tip_height = 0;
        {
            sqlite3_stmt *th = NULL;
            sqlite3_prepare_v2(db, "SELECT MAX(height) FROM blocks",
                               -1, &th, NULL);
            if (sqlite3_step(th) == SQLITE_ROW)
                tip_height = sqlite3_column_int64(th, 0);
            sqlite3_finalize(th);
        }
        int64_t min_height = tip_height - 3; /* 3 confirmations */

        /* Primary: per-address query with confirmation depth */
        sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
            "WHERE spent_txid IS NULL AND address = ? "
            "AND block_height IS NOT NULL AND block_height <= ?",
            -1, &check, NULL);
        sqlite3_bind_text(check, 1, pay_addr, -1, SQLITE_STATIC);
        sqlite3_bind_int64(check, 2, min_height);
        if (sqlite3_step(check) == SQLITE_ROW)
            received = sqlite3_column_int64(check, 0);
        sqlite3_finalize(check);

        /* Only match by z-address — never fall back to amount matching.
         * Amount-only matching is dangerous: could match unrelated
         * payments with the same value, minting tokens for wrong orders. */

        if (received >= expected) {
            /* Payment confirmed — mint tokens FIRST, then update status.
             * This ensures we never show "Tokens Sent" if mint failed. */
            bool mint_ok = zslp_mint(datadir, token_id, cust_addr,
                                      (uint64_t)tokens);

            int new_status = mint_ok ? 2 : 3; /* 2=sent, 3=mint_failed */
            sqlite3_stmt *upd = NULL;
            sqlite3_prepare_v2(db,
                "UPDATE orders SET status=?, paid_at=strftime('%%s','now') "
                "WHERE id=?", -1, &upd, NULL);
            sqlite3_bind_int(upd, 1, new_status);
            sqlite3_bind_int64(upd, 2, order_id);
            sqlite3_step(upd);
            sqlite3_finalize(upd);

            if (mint_ok) {
                printf("Store: order #%lld paid, minted %lld %s -> %s\n",
                       (long long)order_id, (long long)tokens,
                       token_id, cust_addr);
            } else {
                printf("Store: order #%lld paid but MINT FAILED for %s\n",
                       (long long)order_id, cust_addr);
            }
            fflush(stdout);
        }
    }
    sqlite3_finalize(pending);
    sqlite3_close(db);
}

/* Check if a customer has enough tokens to access a service.
 * Used as a before_action hook on protected routes. */
bool store_check_token_access(const char *datadir,
                               const char *customer_addr,
                               const char *token_id,
                               uint64_t required)
{
    if (!datadir || !customer_addr || !token_id) return false;

    uint64_t balance = zslp_balance(datadir, token_id, customer_addr);
    return balance >= required;
}

/* Serve a token-gated page. Checks balance, returns content or 403. */
static size_t serve_gated_content(sqlite3 *db, const char *customer_addr,
                                   const char *token_id, uint64_t required,
                                   const char *datadir,
                                   uint8_t *resp, size_t max)
{
    (void)db;

    char safe_token[128], safe_addr[256];
    html_escape(safe_token, sizeof(safe_token), token_id ? token_id : "");
    html_escape(safe_addr, sizeof(safe_addr),
                customer_addr ? customer_addr : "");

    if (!store_check_token_access(datadir, customer_addr, token_id, required)) {
        char body[2048];
        int blen = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head><style>"
            "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
            "max-width:800px;margin:0 auto;padding:40px}"
            "h1{color:#ff4444}a{color:#00aaff;text-decoration:none}"
            "</style></head><body>"
            "<h1>Access Denied</h1>"
            "<p>This service requires %llu %s tokens.</p>"
            "<p>Your balance: %llu</p>"
            "<p><a href='/store'>&larr; Get tokens</a> | "
            "<a href='/'>Home</a></p>"
            "</body></html>",
            (unsigned long long)required, safe_token,
            (unsigned long long)zslp_balance(datadir, token_id, customer_addr));
        if (blen < 0) blen = 0;
        return store_error_response("403 Forbidden",
            body, (size_t)blen, resp, max);
    }

    /* Customer has tokens — serve the content */
    char body[2048];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:40px}"
        "h1{color:#00ff88}a{color:#00aaff;text-decoration:none}"
        ".card{background:#1a1a1a;padding:20px;margin:15px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        "</style></head><body>"
        "<h1>Premium Service</h1>"
        "<div class='card'>"
        "<p>Welcome, %s</p>"
        "<p>Your token balance: %llu %s</p>"
        "<p>You have access to this service.</p>"
        "</div>"
        "<p><a href='/store'>&larr; Back to store</a> | "
        "<a href='/'>Home</a></p>"
        "</body></html>",
        safe_addr,
        (unsigned long long)zslp_balance(datadir, token_id, customer_addr),
        safe_token);
    if (blen < 0) blen = 0;
    return store_html_response(body, (size_t)blen, resp, max);
}
