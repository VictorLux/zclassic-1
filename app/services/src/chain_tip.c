/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/chain_tip.h"

#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"

#include <stdio.h>
#include <string.h>

static const char *g_tip_source_names[] = {
    [TIP_FROM_UNSPECIFIED] = "unspecified",
    [TIP_FROM_CONNECT]     = "connect",
    [TIP_FROM_DISCONNECT]  = "disconnect",
    [TIP_FROM_SNAPSHOT]    = "snapshot",
    [TIP_FROM_RESTORE]     = "restore",
    [TIP_FROM_BOOT_REPAIR] = "boot_repair",
    [TIP_FROM_P2P_REPAIR]  = "p2p_repair",
    [TIP_FROM_UTXO_REPAIR] = "utxo_repair",
    [TIP_FROM_TEST]        = "test",
};

const char *tip_source_name(enum tip_source src)
{
    if ((int)src < 0 || (size_t)src >=
        sizeof(g_tip_source_names) / sizeof(g_tip_source_names[0]))
        return "unknown";
    return g_tip_source_names[src] ? g_tip_source_names[src] : "unknown";
}

bool chain_set_active_tip(struct main_state *ms,
                          struct block_index *new_tip,
                          enum tip_source src,
                          const char *reason)
{
    if (!ms) return false;

    int from_h = active_chain_height(&ms->chain_active);

    if (!new_tip) {
        if (!active_chain_set_tip(&ms->chain_active, NULL))
            return false;
        printf("[tip] CLEARED (from h=%d) src=%s reason=%s\n",
               from_h, tip_source_name(src),
               reason ? reason : "");
        event_emitf(EV_CHAIN_TIP_COMMIT, 0,
                    "from=%d to=-1 reason=%s",
                    from_h, reason ? reason : "");
        return true;
    }

    if (!active_chain_set_tip(&ms->chain_active, new_tip)) {
        fprintf(stderr,
            "[tip] set_active_tip FAILED at h=%d src=%s reason=%s\n",
            new_tip->nHeight, tip_source_name(src),
            reason ? reason : "");
        return false;
    }

    char hex16[33] = "(no-hash)";
    if (new_tip->phashBlock) {
        char full[65];
        uint256_get_hex(new_tip->phashBlock, full);
        memcpy(hex16, full, 32);
        hex16[32] = '\0';
    }
    printf("[tip] h=%d hash=%s src=%s reason=%s\n",
           new_tip->nHeight, hex16, tip_source_name(src),
           reason ? reason : "");

    /* EV_TIP_UPDATED payload: hash[32] + height(i32). The event
     * library has a typed helper, but emitf with a structured string
     * is sufficient for observers that just want a notification. */
    event_emitf(EV_TIP_UPDATED, 0,
                "h=%d hash=%s src=%s",
                new_tip->nHeight, hex16, tip_source_name(src));
    event_emitf(EV_CHAIN_TIP_COMMIT, 0,
                "from=%d to=%d reason=%s",
                from_h, new_tip->nHeight, reason ? reason : "");
    return true;
}
