/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block view — JSON serializers for block data.
 *
 * Pattern:
 *   struct json_value result;
 *   json_init(&result);
 *   block_view_summary(&result, bi, datadir);
 *   // result now contains the JSON representation
 */

#ifndef ZCL_VIEWS_BLOCK_VIEW_H
#define ZCL_VIEWS_BLOCK_VIEW_H

#include "json/json.h"

struct block_index;

/* Minimal block info: hash, height, confirmations, time */
void block_view_brief(struct json_value *out,
                      const struct block_index *bi,
                      int tip_height);

/* Full block with header fields + tx list */
void block_view_full(struct json_value *out,
                     const struct block_index *bi,
                     int tip_height,
                     const char *datadir);

#endif
