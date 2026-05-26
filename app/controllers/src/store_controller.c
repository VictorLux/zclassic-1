/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store controller — ZSLP token commerce. */

#include "platform/time_compat.h"
#include "views/format_helpers.h"
#include "controllers/store_controller.h"
#include "controllers/zslp_controller.h"
#include "models/database.h"
#include "models/shared_validators.h"
#include "models/store.h"
#include "services/zslp_service.h"
#include "script/standard.h"
#include "wallet/sapling_keys.h"
#include "crypto/hmac_sha256.h"
#include "core/random.h"
#include "util/template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

/* Forward declarations */
static size_t serve_gated_content(sqlite3 *db, const char *customer_addr,
                                   const char *token_id, uint64_t required,
                                   const char *datadir,
                                   uint8_t *resp, size_t max);
static void store_csrf_token(const char *context, char out[33]);
static void store_csrf_context(char *out, size_t outmax, int64_t product_id);
static bool store_csrf_verify(const char *context, const char *provided);
static const char *store_order_status_text(int status);
static const char *store_order_status_class(int status);
static bool store_parse_access_query(const char *path,
                                     char *addr, size_t addr_max,
                                     char *token, size_t token_max);
static int64_t store_chain_tip_height(sqlite3 *db);
static int64_t store_received_payment(sqlite3 *db, const char *pay_addr,
                                      int64_t min_height);
static bool store_mark_order_paid(const char *datadir,
                                  int64_t order_id,
                                  int status);

/* Format ZCL price: trim trailing zeros but keep at least 2 decimals. */
static void format_zcl_price(char *out, size_t out_len, int64_t zatoshi)
{
    snprintf(out, out_len, "%.8f", (double)zatoshi / 1e8);
    /* Find decimal point */
    char *dot = strchr(out, '.');
    if (!dot) return;
    /* Trim trailing zeros, but keep at least 2 decimal places */
    char *end = out + strlen(out) - 1;
    char *min_pos = dot + 2; /* keep at least ".XX" */
    while (end > min_pos && *end == '0') end--;
    *(end + 1) = '\0';
}

/* Ensure store tables exist */
static void store_ensure_schema(sqlite3 *db, const char *datadir)
{
    /* Load products from {datadir}/store/products.json if it exists,
     * otherwise seed with demo products. This lets node operators
     * customize their store by editing a simple JSON file. */
    sqlite3_stmt *cnt = NULL;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM products", -1, &cnt, NULL);
    bool empty = (AR_STEP_ROW_READONLY(cnt) == SQLITE_ROW &&
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
                    struct node_db ndb;
                    struct db_store_product product;
                    memset(&ndb, 0, sizeof(ndb));
                    ndb.db = db;
                    ndb.open = true;
                    memset(&product, 0, sizeof(product));
                    snprintf(product.name, sizeof(product.name), "%s", name);
                    snprintf(product.description, sizeof(product.description), "%s", desc);
                    snprintf(product.token_id, sizeof(product.token_id), "%s", token);
                    product.price_zatoshi =
                        (int64_t)(price_zcl * (double)ZATOSHI_PER_ZCL);
                    product.tokens_per_purchase = tokens;
                    product.active = true;
                    if (!db_store_product_save(&ndb, &product)) {
                        p = end + 1;
                        continue;
                    }
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
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        ndb.db = db;
        ndb.open = true;
        struct db_store_product products[] = {
            {
                .name = "ZCL23 Access Token",
                .description =
                    "1 token grants access to premium .onion services on the "
                    "ZClassic23 network. Tokens are ZSLP tokens on the ZClassic "
                    "blockchain.",
                .price_zatoshi = 1000000,
                .token_id = "ZCL23ACCESS",
                .tokens_per_purchase = 10,
                .active = true
            },
            {
                .name = "VPN Credit (1 month)",
                .description =
                    "Route traffic through the ZClassic23 onion network. "
                    "1 month of encrypted relay service.",
                .price_zatoshi = 5000000,
                .token_id = "ZCL23VPN",
                .tokens_per_purchase = 1,
                .active = true
            },
            {
                .name = "Storage (1 GB)",
                .description =
                    "Encrypted storage on the ZClassic23 distributed network. "
                    "Data replicated across multiple .onion nodes.",
                .price_zatoshi = 2000000,
                .token_id = "ZCL23STORE",
                .tokens_per_purchase = 1,
                .active = true
            }
        };
        for (size_t i = 0; i < sizeof(products) / sizeof(products[0]); i++)
            (void)db_store_product_save(&ndb, &products[i]);
    }
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
        ".failed{background:#3a1a1a;color:#ff4444}"
        "input{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:8px;font-family:monospace;width:100%%;margin:5px 0;"
        "box-sizing:border-box}"
        "</style></head><body>"
        "<div class='header-nav'>"
        "<h1 style='margin:0'><a href='/store'>ZCL Store</a></h1>"
        "<a href='/'>Home</a>"
        "<a href='/store/products'>Products</a>"
        "<a href='/store/orders'>Orders</a>"
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
    "<h3><a href='/store/products/{{id}}'>{{name}}</a></h3>"
    "<p>{{description}}</p>"
    "<div class='price'>{{price}} ZCL</div>"
    "<a href='/store/products/{{id}}' class='btn'>View</a>"
    "</div>";

static size_t serve_order_index(sqlite3 *db, uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_order_summary orders[50];
    char body[16384];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Orders");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Recent Orders</h2>");
    if (n > 0) off += (size_t)n;

    int count = db_store_order_list_recent(&ndb, orders,
        sizeof(orders) / sizeof(orders[0]));
    for (int i = 0; i < count && off + 768 < sizeof(body); ++i) {
        char safe_product[256], price_str[32];
        html_escape(safe_product, sizeof(safe_product),
                    orders[i].product_name[0] ? orders[i].product_name
                                              : "Unknown Product");
        format_zcl_price(price_str, sizeof(price_str), orders[i].amount_zatoshi);
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='product'>"
            "<h3><a href='/store/orders/%lld'>Order #%lld</a></h3>"
            "<p>%s</p>"
            "<div class='price'>%s ZCL</div>"
            "<p>Status: %s</p>"
            "</div>",
            (long long)orders[i].id, (long long)orders[i].id,
            safe_product, price_str, store_order_status_text(orders[i].status));
        if (n > 0) off += (size_t)n;
    }

    if (count == 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<p style='color:#666'>No orders yet.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off, "</body></html>");
    if (n > 0) off += (size_t)n;
    return store_html_response(body, off, resp, max);
}

/* GET /store — list products */
static size_t serve_product_list(sqlite3 *db, uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_product products[64];
    char body[16384];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "ZCL Store");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Products</h2>");
    if (n > 0) off += (size_t)n;

    int count = db_store_product_list_active(&ndb, products,
        sizeof(products) / sizeof(products[0]));
    for (int i = 0; i < count && off + 1024 < sizeof(body); ++i) {
        char id_str[32], price_str[32];
        snprintf(id_str, sizeof(id_str), "%lld", (long long)products[i].id);
        format_zcl_price(price_str, sizeof(price_str), products[i].price_zatoshi);

        struct template_var vars[] = {
            { "id",          id_str },
            { "name",        products[i].name[0] ? products[i].name : "?" },
            { "description", products[i].description },
            { "price",       price_str },
        };

        size_t rendered = template_render(PRODUCT_CARD_TEMPLATE,
            vars, sizeof(vars) / sizeof(vars[0]),
            body + off, sizeof(body) - off);
        off += rendered;
    }

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
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_product product;
    memset(&product, 0, sizeof(product));

    if (!db_store_product_find_active(&ndb, product_id, &product)) {
        const char *body = "<h1>Product not found</h1>"
            "<p><a href='/store/products'>Back to store</a></p>";
        return store_error_response("404 Not Found", body, strlen(body),
                                     resp, max);
    }

    char safe_name[256], safe_desc[512], safe_token[128];
    html_escape(safe_name, sizeof(safe_name),
                product.name[0] ? product.name : "?");
    html_escape(safe_desc, sizeof(safe_desc), product.description);
    html_escape(safe_token, sizeof(safe_token),
                product.token_id[0] ? product.token_id : "TOKENS");

    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), safe_name);
    if (n > 0) off = (size_t)n;

    char detail_price[32];
    format_zcl_price(detail_price, sizeof(detail_price), product.price_zatoshi);

    char csrf_ctx[64], csrf_tok[33];
    store_csrf_context(csrf_ctx, sizeof(csrf_ctx), product_id);
    store_csrf_token(csrf_ctx, csrf_tok);

    n = snprintf(body + off, sizeof(body) - off,
        "<div class='product'>"
        "<h2>%s</h2>"
        "<p>%s</p>"
        "<div class='price'>%s ZCL</div>"
        "<p>You will receive <b>%lld</b> %s tokens.</p>"
        "<h3>Purchase</h3>"
        "<form method='post' action='/store/orders'>"
        "<input type='hidden' name='product_id' value='%lld'>"
        "<input type='hidden' name='csrf_token' value='%s'>"
        "<label>Your t-address (to receive tokens):</label>"
        "<input type='text' name='customer_addr' placeholder='t1...' required>"
        "<br><br>"
        "<button type='submit' class='btn'>Generate Payment Address</button>"
        "</form>"
        "</div>"
        "<p><a href='/store/products'>&larr; Back to store</a></p>"
        "</body></html>",
        safe_name,
        safe_desc,
        detail_price,
        (long long)product.tokens_per_purchase,
        safe_token,
        (long long)product_id,
        csrf_tok);
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* POST /store/buy/:id — create order */
static size_t serve_create_order(sqlite3 *db, int64_t product_id,
                                  const char *customer_addr,
                                  const char *datadir,
                                  uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_product product;
    memset(&product, 0, sizeof(product));

    if (!db_store_product_find_active(&ndb, product_id, &product)) {
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n<h1>Product not found</h1>");
    }

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
            "<p><a href='/store/products'>Back to Store</a></p>");
    }

    struct db_store_order order;
    memset(&order, 0, sizeof(order));
    order.product_id = product_id;
    snprintf(order.customer_addr, sizeof(order.customer_addr), "%s",
             customer_addr ? customer_addr : "");
    snprintf(order.payment_addr, sizeof(order.payment_addr), "%s", payment_addr);
    order.amount_zatoshi = product.price_zatoshi;
    order.status = STORE_ORDER_PENDING;
    if (!db_store_order_save(&ndb, &order)) {
        printf("store: order INSERT failed: %s\n", sqlite3_errmsg(db));
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n<h1>Order creation failed</h1>");
    }
    int64_t order_id = order.id;

    /* Show payment page */
    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Payment");
    if (n > 0) off = (size_t)n;

    char safe_pay[256], safe_cust[256];
    html_escape(safe_pay, sizeof(safe_pay), payment_addr);
    html_escape(safe_cust, sizeof(safe_cust),
                customer_addr ? customer_addr : "(not provided)");

    char order_price[32];
    format_zcl_price(order_price, sizeof(order_price), product.price_zatoshi);

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Order #%lld</h2>"
        "<div class='product'>"
        "<p>Send exactly <span class='price'>%s ZCL</span> to:</p>"
        "<div class='addr'>%s</div>"
        "<button class='btn' style='font-size:12px;padding:6px 12px;cursor:pointer;border:none' "
        "onclick=\"navigator.clipboard?navigator.clipboard.writeText('%s'):void(0);"
        "this.textContent='Copied!'\">Copy Address</button>"
        "<p>After payment confirms, tokens will be sent to:</p>"
        "<div class='addr'>%s</div>"
        "<p><a href='/store/orders/%lld'>Check payment status</a></p>"
        "</div>"
        "<p><a href='/store/products'>&larr; Back to store</a></p>"
        "</body></html>",
        (long long)order_id,
        order_price,
        safe_pay,
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
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_order_view order;
    memset(&order, 0, sizeof(order));

    if (!db_store_order_find_view(&ndb, order_id, &order)) {
        const char *body = "<h1>Order not found</h1>"
            "<p><a href='/store/orders'>Back to store</a></p>";
        return store_error_response("404 Not Found", body, strlen(body),
                                     resp, max);
    }

    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Order Status");
    if (n > 0) off = (size_t)n;

    /* Auto-refresh while pending */
    if (order.status == STORE_ORDER_PENDING) {
        n = snprintf(body + off, sizeof(body) - off,
            "<meta http-equiv='refresh' content='15'>");
        if (n > 0) off += (size_t)n;
    }

    char safe_pay[256], safe_cust[256];
    char safe_ptxid[256], safe_mtxid[256];
    char safe_product[256];
    html_escape(safe_pay, sizeof(safe_pay), order.payment_addr[0] ? order.payment_addr : "?");
    html_escape(safe_cust, sizeof(safe_cust), order.customer_addr[0] ? order.customer_addr : "?");
    html_escape(safe_ptxid, sizeof(safe_ptxid), order.payment_txid);
    html_escape(safe_mtxid, sizeof(safe_mtxid), order.mint_txid);
    html_escape(safe_product, sizeof(safe_product),
                order.product_name[0] ? order.product_name : "Unknown Product");

    char status_price[32];
    format_zcl_price(status_price, sizeof(status_price), order.amount_zatoshi);

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Order #%lld</h2>"
        "<div class='product'>"
        "<h3>%s</h3>"
        "<div class='status %s'>%s</div>"
        "<p>Amount: <span class='price'>%s ZCL</span></p>"
        "<p>Payment address:</p><div class='addr'>%s</div>"
        "<p>Deliver to:</p><div class='addr'>%s</div>"
        "%s%s%s%s%s%s"
        "<p><a href='/store/orders/%lld'>Refresh</a> | "
        "<a href='/store/orders'>&larr; Back to orders</a></p>"
        "</div></body></html>",
        (long long)order_id,
        safe_product,
        store_order_status_class(order.status),
        store_order_status_text(order.status),
        status_price,
        safe_pay,
        safe_cust,
        order.payment_txid[0] ? "<p>Payment: <code>" : "",
        order.payment_txid[0] ? safe_ptxid : "",
        order.payment_txid[0] ? "</code></p>" : "",
        order.mint_txid[0] ? "<p>Mint: <code>" : "",
        order.mint_txid[0] ? safe_mtxid : "",
        order.mint_txid[0] ? "</code></p>" : "",
        (long long)order_id);
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* Parse resource id from the last path segment and reject malformed ids. */
static bool parse_positive_path_id(const char *path, int64_t *id_out)
{
    const char *last = strrchr(path, '/');
    char *end = NULL;
    long long value;

    if (!id_out)
        return false;
    *id_out = -1;
    if (!last || !last[1])
        return false;
    value = strtoll(last + 1, &end, 10);
    if (!end || *end != '\0' || value <= 0)
        return false;
    *id_out = (int64_t)value;
    return true;
}

static bool path_eq(const char *path, const char *expected)
{
    return path && expected && strcmp(path, expected) == 0;
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    return path && prefix && strncmp(path, prefix, strlen(prefix)) == 0;
}

static bool route_is_product_index(const char *path)
{
    return path_eq(path, "/store") || path_eq(path, "/store/") ||
           path_eq(path, "/store/products") || path_eq(path, "/store/products/");
}

static bool route_is_product_show(const char *path)
{
    return path_has_prefix(path, "/store/product/") ||
           path_has_prefix(path, "/store/products/");
}

static bool route_is_order_show(const char *path)
{
    return path_has_prefix(path, "/store/order/") ||
           path_has_prefix(path, "/store/orders/");
}

static bool route_is_order_index(const char *path)
{
    return path_eq(path, "/store/orders") || path_eq(path, "/store/orders/");
}

static bool route_is_order_create(const char *method, const char *path)
{
    return method && strcmp(method, "POST") == 0 &&
           (path_eq(path, "/store/orders") ||
            path_eq(path, "/store/orders/") ||
            path_has_prefix(path, "/store/buy/"));
}

/* Validate address: must be a valid ZClassic t-address or z-address,
 * with the Base58Check / Bech32 *checksum* verified — not just a
 * syntactically-plausible prefix.  A one-character typo in a t-addr
 * passes the old shape check but decodes to a random 20-byte hash
 * whose payments are unspendable: funds sent to such an order are
 * burned.  Also prevents XSS via customer_addr in HTML output.
 *
 * Implementation: zcl_validate_zcl_address in app/models/src/shared_validators.c. */

static bool store_validate_access_addr(const char *addr)
{
    return addr && addr[0] &&
           zslp_service_validate_recipient_addr(addr, false);
}

static bool store_validate_access_token(const char *token)
{
    return token && token[0] &&
           zslp_service_validate_token_key(token);
}

static bool store_parse_query_field(const char *path, const char *field,
                                    char *out, size_t out_max)
{
    const char *p;
    size_t i = 0;
    char needle[32];

    if (!path || !field || !out || out_max == 0)
        return false;
    out[0] = '\0';
    snprintf(needle, sizeof(needle), "%s=", field);
    p = strstr(path, needle);
    if (!p)
        return false;
    p += strlen(needle);
    while (p[i] && p[i] != '&' && i < out_max - 1) {
        if ((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126)
            return false;
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool store_parse_access_query(const char *path,
                                     char *addr, size_t addr_max,
                                     char *token, size_t token_max)
{
    if (!addr || !token || addr_max == 0 || token_max == 0)
        return false;
    addr[0] = '\0';
    token[0] = '\0';

    if (!store_parse_query_field(path, "addr", addr, addr_max))
        return false;
    if (!store_parse_query_field(path, "token", token, token_max))
        snprintf(token, token_max, "%s", "ZCL23ACCESS");

    return store_validate_access_addr(addr) &&
           store_validate_access_token(token);
}

static int64_t store_chain_tip_height(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    int64_t tip_height = 0;

    if (!db)
        return 0;
    if (sqlite3_prepare_v2(db, "SELECT MAX(height) FROM blocks",
                           -1, &s, NULL) != SQLITE_OK || !s)
        return 0;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
        tip_height = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return tip_height;
}

static int64_t store_received_payment(sqlite3 *db, const char *pay_addr,
                                      int64_t min_height)
{
    sqlite3_stmt *s = NULL;
    int64_t received = 0;

    if (!db || !pay_addr || !pay_addr[0])
        return 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(value), 0) FROM wallet_sapling_notes "
            "WHERE spent_txid IS NULL AND address = ? "
            "AND block_height IS NOT NULL AND block_height <= ?",
            -1, &s, NULL) != SQLITE_OK || !s)
        return 0;

    sqlite3_bind_text(s, 1, pay_addr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, min_height);
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
        received = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return received;
}

static bool store_mark_order_paid(const char *datadir,
                                  int64_t order_id,
                                  int status)
{
    char db_path[1024];
    struct node_db ndb;
    bool ok;

    if (!datadir || order_id <= 0)
        return false;

    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, db_path))
        return false;

    ok = db_store_order_mark_paid(&ndb, order_id, status);
    if (!ok) {
        LOG_WARN("controller", "Store: failed to persist status=%d for order #%lld: %s", status, (long long)order_id, sqlite3_errmsg(ndb.db));
    }
    node_db_close(&ndb);
    return ok;
}

static const char *store_order_status_text(int status)
{
    switch (status) {
    case STORE_ORDER_PENDING: return "Pending Payment";
    case STORE_ORDER_PAID: return "Payment Received";
    case STORE_ORDER_SENT: return "Tokens Sent";
    case STORE_ORDER_FAILED: return "Mint Failed (contact support)";
    default: return "Unknown";
    }
}

static const char *store_order_status_class(int status)
{
    switch (status) {
    case STORE_ORDER_PENDING: return "pending";
    case STORE_ORDER_FAILED: return "failed";
    default: return "paid";
    }
}

/* ── CSRF form token ─────────────────────────────────────
 *
 * Without a token, a malicious third-party page can `<form action=
 * 'http://<onion>/store/orders'>` and trick any visiting browser into
 * silently POSTing an unwanted order.  The store has no login/session
 * cookie to bind to, so classical per-session CSRF isn't reachable
 * without plumbing cookies through onion_service.c.  Instead, sign a
 * small context string (order form scope + product-id) with a
 * per-process random HMAC key and embed it as a hidden field.  The
 * browser's same-origin policy prevents JS on a third-party page from
 * reading our GET response body, so it cannot learn the signed token.
 * A server-side attacker with their own curl can, but that's the same
 * capability as direct submission — no amplification from the victim
 * browser. */
static unsigned char s_csrf_key[32];
static bool s_csrf_key_ready = false;

static void store_csrf_init(void)
{
    if (s_csrf_key_ready) return;
    GetRandBytes(s_csrf_key, sizeof(s_csrf_key));
    s_csrf_key_ready = true;
}

/* Write 32-char lowercase-hex token for `context` into out (33 bytes incl NUL). */
static void store_csrf_token(const char *context, char out[33])
{
    store_csrf_init();
    struct hmac_sha256_ctx ctx;
    unsigned char mac[HMAC_SHA256_OUTPUT_SIZE];
    hmac_sha256_init(&ctx, s_csrf_key, sizeof(s_csrf_key));
    hmac_sha256_write(&ctx, (const unsigned char *)context, strlen(context));
    hmac_sha256_finalize(&ctx, mac);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; i++) {
        out[i * 2]     = hex[(mac[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[mac[i] & 0x0f];
    }
    out[32] = '\0';
}

/* Constant-time check: does `provided` match the token for `context`? */
static bool store_csrf_verify(const char *context, const char *provided)
{
    if (!context || !provided) return false;
    if (strlen(provided) != 32) return false;
    char expected[33];
    store_csrf_token(context, expected);
    unsigned char diff = 0;
    for (size_t i = 0; i < 32; i++)
        diff |= (unsigned char)(expected[i] ^ provided[i]);
    return diff == 0;
}

/* Context string — the token is bound to the specific order form so a
 * leaked token from one product page can't be replayed to another.
 * Format: "store:order:<product_id>".  Writes into a caller buffer. */
static void store_csrf_context(char *out, size_t outmax, int64_t product_id)
{
    snprintf(out, outmax, "store:order:%lld", (long long)product_id);
}

/* Decode `%XX` and `+` escapes in an x-www-form-urlencoded value.
 * Ported from app/controllers/src/wallet_view_helpers.c. Without
 * this, "a%20b" in a form field is stored literally as the four bytes
 * '%','2','0','b' in the DB and rendered back to the user unchanged —
 * breaking display, search, and anything downstream that interprets
 * the stored value. */
static void store_url_decode(char *dst, size_t dstmax, const char *src, size_t srclen)
{
    size_t di = 0;
    if (!dstmax) return;
    for (size_t si = 0; si < srclen && di < dstmax - 1; si++) {
        char c = src[si];
        if (c == '%' && si + 2 < srclen) {
            char h1 = src[si + 1], h2 = src[si + 2];
            int hi = (h1 >= '0' && h1 <= '9') ? h1 - '0' :
                     (h1 >= 'a' && h1 <= 'f') ? h1 - 'a' + 10 :
                     (h1 >= 'A' && h1 <= 'F') ? h1 - 'A' + 10 : -1;
            int lo = (h2 >= '0' && h2 <= '9') ? h2 - '0' :
                     (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10 :
                     (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10 : -1;
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 2;
                continue;
            }
        }
        dst[di++] = (c == '+') ? ' ' : c;
    }
    dst[di] = '\0';
}

/* Parse x-www-form-urlencoded body for `field=value` and URL-decode
 * the value into `out`. */
static const char *parse_form_field(const char *body, size_t len,
                                     const char *field, char *out, size_t outmax)
{
    if (!body || !len || !field || !out || outmax == 0)
        LOG_NULL("store", "parse_form_field: null args body=%p len=%zu field=%s",
                 (void *)body, len, field ? field : "(null)");
    char search[128];
    snprintf(search, sizeof(search), "%s=", field);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    /* Value ends at &, space, or end of body. */
    size_t remaining = len - (size_t)(p - body);
    size_t vlen = 0;
    while (vlen < remaining && p[vlen] && p[vlen] != '&' && p[vlen] != ' ')
        vlen++;
    store_url_decode(out, outmax, p, vlen);
    return out;
}

static bool parse_positive_form_id(const char *body, size_t body_len,
                                   const char *field, int64_t *id_out)
{
    char raw[32];
    char *end = NULL;
    long long value;

    if (!id_out)
        return false;
    *id_out = -1;
    if (!parse_form_field(body, body_len, field, raw, sizeof(raw)))
        return false;
    value = strtoll(raw, &end, 10);
    if (!end || *end != '\0' || value <= 0)
        return false;
    *id_out = (int64_t)value;
    return true;
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
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, db_path)) return 0;
    sqlite3 *db = ndb.db;
    store_ensure_schema(db, datadir);

    size_t result = 0;

    if (route_is_product_index(path)) {
        result = serve_product_list(db, response, response_max);

    } else if (route_is_product_show(path)) {
        int64_t id = -1;
        if (!parse_positive_path_id(path, &id)) {
            const char *err_body = "<h1>Invalid product</h1>"
                "<p>Product id must be a positive integer.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_product_detail(db, id, response, response_max);
        }

    } else if (route_is_order_index(path) &&
               method && strcmp(method, "GET") == 0) {
        result = serve_order_index(db, response, response_max);

    } else if (route_is_order_create(method, path)) {
        int64_t id = -1;
        char addr[128] = "";
        char csrf[64] = "";
        if (body && body_len > 0) {
            parse_form_field((const char *)body, body_len,
                             "customer_addr", addr, sizeof(addr));
            parse_form_field((const char *)body, body_len,
                             "csrf_token", csrf, sizeof(csrf));
        }
        if (path_has_prefix(path, "/store/buy/")) {
            if (!parse_positive_path_id(path, &id))
                id = -1;
        } else if (!parse_positive_form_id((const char *)body, body_len,
                                           "product_id", &id)) {
            const char *err_body = "<h1>Invalid product</h1>"
                "<p>product_id must be a positive integer.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
            node_db_close(&ndb);
            return result;
        }
        char csrf_ctx[64];
        store_csrf_context(csrf_ctx, sizeof(csrf_ctx), id);
        if (!store_csrf_verify(csrf_ctx, csrf)) {
            const char *err_body = "<h1>Invalid CSRF token</h1>"
                "<p>Form token missing or did not verify. "
                "Please reload the product page and resubmit.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
            node_db_close(&ndb);
            return result;
        }
        if (!zcl_validate_zcl_address(addr)) {
            const char *err_body = "<h1>Invalid address</h1>"
                "<p>Must be a ZClassic t-address (t1.../t3...) or "
                "z-address (zs1...).</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_create_order(db, id, addr, datadir,
                                          response, response_max);
        }

    } else if (route_is_order_show(path)) {
        int64_t id = -1;
        if (!parse_positive_path_id(path, &id)) {
            const char *err_body = "<h1>Invalid order</h1>"
                "<p>Order id must be a positive integer.</p>"
                "<p><a href='/store/orders'>&larr; Back to orders</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_order_status(db, id, response, response_max);
        }

    } else if (strncmp(path, "/store/access", 13) == 0) {
        /* Token-gated content: /store/access?addr=t1...&token=ZCL23ACCESS */
        char addr[128] = "", token[64] = "";
        if (!store_parse_access_query(path, addr, sizeof(addr),
                                      token, sizeof(token))) {
            const char *err_body = "<h1>Invalid access request</h1>"
                "<p>addr must be a valid ZClassic address and token must be a valid token id.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_gated_content(db, addr, token, 1, datadir,
                                          response, response_max);
        }
    }

    node_db_close(&ndb);
    return result;
}

/* Background payment processor — called periodically from boot.c.
 * Checks pending orders for payments, mints tokens when paid. */
void store_process_payments(const char *datadir)
{
    if (!datadir) return;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, db_path)) return;
    sqlite3 *db = ndb.db;

    struct db_store_pending_payment pending_orders[64];
    int pending_count = db_store_order_list_pending_payments(&ndb,
        pending_orders, sizeof(pending_orders) / sizeof(pending_orders[0]),
        (int64_t)platform_time_wall_time_t() - 3600);

    for (int i = 0; i < pending_count; ++i) {
        int64_t order_id = pending_orders[i].id;
        const char *pay_addr = pending_orders[i].payment_addr;
        int64_t expected = pending_orders[i].amount_zatoshi;
        const char *cust_addr = pending_orders[i].customer_addr;
        const char *token_id = pending_orders[i].token_id;
        int64_t tokens = pending_orders[i].tokens_per_purchase;

        if (!pay_addr[0] || !cust_addr[0] || !token_id[0])
            continue;

        /* Check if payment received at the order's z-address.
         * Primary: match notes by address (works with real z-addresses).
         * Fallback: match by exact amount (for placeholder z-addresses). */
        /* Require minimum 3 confirmations to prevent reorg-based
         * double-spend: payment reversed but tokens already minted. */
        int64_t tip_height = store_chain_tip_height(db);
        int64_t min_height = tip_height - 3; /* 3 confirmations */

        /* Primary: per-address query with confirmation depth */
        int64_t received = store_received_payment(db, pay_addr, min_height);

        /* Only match by z-address — never fall back to amount matching.
         * Amount-only matching is dangerous: could match unrelated
         * payments with the same value, minting tokens for wrong orders. */

        if (received >= expected) {
            /* Payment confirmed — mint tokens FIRST, then update status.
             * This ensures we never show "Tokens Sent" if mint failed. */
            bool mint_ok = zslp_mint(datadir, token_id, cust_addr,
                                      (uint64_t)tokens);
            int new_status = mint_ok ? STORE_ORDER_SENT : STORE_ORDER_FAILED;
            if (!store_mark_order_paid(datadir, order_id, new_status)) {
                printf("Store: order #%lld payment processed but status "
                       "persist failed\n", (long long)order_id);
                fflush(stdout);
            }

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
    node_db_close(&ndb);
}

/* Check if a customer has enough tokens to access a service.
 * Used as a before_action hook on protected routes. */
bool store_check_token_access(const char *datadir,
                               const char *customer_addr,
                               const char *token_id,
                               uint64_t required)
{
    if (!datadir ||
        !store_validate_access_addr(customer_addr) ||
        !store_validate_access_token(token_id))
        LOG_FAIL("store", "check_token_access: invalid args datadir=%p addr=%s token=%s",
                 (void *)datadir,
                 customer_addr ? customer_addr : "(null)",
                 token_id ? token_id : "(null)");

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
    uint64_t balance = zslp_balance(datadir, token_id, customer_addr);

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
            (unsigned long long)balance);
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
        (unsigned long long)balance,
        safe_token);
    if (blen < 0) blen = 0;
    return store_html_response(body, (size_t)blen, resp, max);
}
