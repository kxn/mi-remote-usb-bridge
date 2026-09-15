/* WCH BLE Central radio backend + TMOS task.
 *
 * Implements rbp_radio_backend_t on the CH58x BLE library: scanning feeds
 * candidates to the server, pair_begin establishes a link and lets the
 * GAP bond manager run SMP (RC003 uses Just Works), the RC003 adapter
 * performs GATT discovery afterwards, and auto-reconnect re-establishes
 * the link from the peer address committed in the board store.
 */
#ifndef RBP_WCH_CENTRAL_H
#define RBP_WCH_CENTRAL_H

#include "rbp_server.h"
#include "rc003_adapter.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t wch_central_task_id; /* TMOS task id (used by wch_gatt.c) */

void wch_central_init(rbp_server_t *server, rc003_adapter_t *adapter);
const rbp_radio_backend_t *wch_central_backend(void);

#ifdef __cplusplus
}
#endif
#endif
