/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP payment service — shielded payment helpers. */

#ifndef ZCL_ZSLP_PAYMENT_SERVICE_H
#define ZCL_ZSLP_PAYMENT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wallet;

bool zslp_payment_generate_address(struct wallet *wallet,
                                   char *z_addr_out, size_t max);
int64_t zslp_payment_check_received(const char *datadir,
                                    const char *z_addr,
                                    int64_t min_amount);

#endif
