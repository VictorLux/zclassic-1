/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Store HTML page handlers (order index, product list/detail, order
 * create/status). Split out of store_controller.c (D5); behavior
 * byte-identical. Shared helpers come via the internal header. */

#include "controllers/store_controller_internal.h"

static const char PRODUCT_CARD_TEMPLATE[] =
    "<div class='product'>"
    "<h3><a href='/store/products/{{id}}'>{{name}}</a></h3>"
    "<p>{{description}}</p>"
    "<div class='price'>{{price}} ZCL</div>"
    "<a href='/store/products/{{id}}' class='btn'>View</a>"
    "</div>";

size_t serve_order_index(sqlite3 *db, uint8_t *resp, size_t max)
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
size_t serve_product_list(sqlite3 *db, uint8_t *resp, size_t max)
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
size_t serve_product_detail(sqlite3 *db, int64_t product_id,
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
size_t serve_create_order(sqlite3 *db, int64_t product_id,
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
size_t serve_order_status(sqlite3 *db, int64_t order_id,
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

