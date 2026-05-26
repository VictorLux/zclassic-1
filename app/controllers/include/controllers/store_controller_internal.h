/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal shared includes + helper/handler decls for the store
 * controller. Included by store_controller*.c only — not public.
 * Helpers below are defined in store_controller.c; the serve_*
 * page handlers are defined in store_controller_pages.c. */

#ifndef ZCL_CONTROLLERS_STORE_CONTROLLER_INTERNAL_H
#define ZCL_CONTROLLERS_STORE_CONTROLLER_INTERNAL_H

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
#include <sqlite3.h>

/* store_controller_schema.c — SQLite schema bootstrap. */
void store_ensure_schema(sqlite3 *db, const char *datadir);

/* ── Shared response/format/CSRF helpers (defined in store_controller.c) ── */
const char *store_get_onion_address(void);
void format_zcl_price(char *out, size_t out_len, int64_t zatoshi);
int html_body_start(char *buf, size_t max, const char *title);
size_t store_html_response(const char *body, size_t body_len,
                           uint8_t *resp, size_t max);
size_t store_error_response(const char *status_code,
                            const char *body, size_t body_len,
                            uint8_t *resp, size_t max);
void store_csrf_token(const char *context, char out[33]);
void store_csrf_context(char *out, size_t outmax, int64_t product_id);
const char *store_order_status_text(int status);
const char *store_order_status_class(int status);

/* ── Page handlers (defined in store_controller_pages.c) ── */
size_t serve_order_index(sqlite3 *db, uint8_t *resp, size_t max);
size_t serve_product_list(sqlite3 *db, uint8_t *resp, size_t max);
size_t serve_product_detail(sqlite3 *db, int64_t product_id,
                            uint8_t *resp, size_t max);
size_t serve_create_order(sqlite3 *db, int64_t product_id,
                          const char *customer_addr,
                          const char *datadir,
                          uint8_t *resp, size_t max);
size_t serve_order_status(sqlite3 *db, int64_t order_id,
                          uint8_t *resp, size_t max);

#endif /* ZCL_CONTROLLERS_STORE_CONTROLLER_INTERNAL_H */
