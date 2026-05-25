/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain restore executor — mutable execution helpers for restore plans. */

#ifndef ZCL_CHAIN_RESTORE_EXECUTOR_H
#define ZCL_CHAIN_RESTORE_EXECUTOR_H

#include <stdbool.h>

struct main_state;
struct block_index;

bool chain_restore_commit_tip_via_csr(struct main_state *ms,
                                      struct block_index *target,
                                      bool update_header_tip,
                                      const char *reason);

bool chain_restore_commit_header_via_csr(struct main_state *ms,
                                         struct block_index *target,
                                         const char *reason);

#endif /* ZCL_CHAIN_RESTORE_EXECUTOR_H */
