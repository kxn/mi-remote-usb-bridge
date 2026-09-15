/* WCH BLE-library GATT client -> rbp_gatt_client_t bridge.
 *
 * The adapter drives one procedure at a time; this layer translates the
 * WCH gattMsgEvent_t stream into rbp_gatt_evt_t and forwards notifications.
 * All events are consumed synchronously (zero-copy) from the TMOS task
 * that owns the message, which is also the task that calls the adapter.
 */
#ifndef RBP_WCH_GATT_H
#define RBP_WCH_GATT_H

#include "gatt_client.h"
#include "rc003_adapter.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bind the adapter that receives translated events (call before use) */
void wch_gatt_set_adapter(rc003_adapter_t *a);

/* connHandle changes when the link (re)establishes */
void wch_gatt_set_conn(uint16_t conn_handle);
/* Stop submissions and event delivery immediately when disconnect starts. */
void wch_gatt_quiesce(void);
uint16_t wch_gatt_conn(void);

const rbp_gatt_client_t *wch_gatt_client(void);

/* called from the central task's TMOS message pump for every GATT message */
struct gattMsgEvent;
void wch_gatt_on_msg(void *pMsg); /* gattMsgEvent_t* */

#ifdef __cplusplus
}
#endif
#endif
