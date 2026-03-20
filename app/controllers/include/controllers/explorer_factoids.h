/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer factoids page -- historian nerd stats with data receipts. */

#ifndef ZCL_CONTROLLERS_EXPLORER_FACTOIDS_H
#define ZCL_CONTROLLERS_EXPLORER_FACTOIDS_H

#include <stdint.h>
#include <stddef.h>

size_t explorer_factoids_build(uint8_t *buf, size_t buf_max, const char *datadir);

#endif
