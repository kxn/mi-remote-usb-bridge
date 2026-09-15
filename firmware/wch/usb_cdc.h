/* USB CDC-ACM serial link for the CH582F bridge.
 *
 * EP0: standard + CDC class control requests.  EP1: interrupt IN (CDC
 * notification).  EP2: bulk IN/OUT data.  DTR changes do not reset the
 * session and line coding is accepted but ignored (native USB speed).
 *
 * The bridge feeds rbp_server_on_usb_rx() with OUT data and drains the
 * server's output ring into EP2 IN (full 64-byte packets + ZLP handled by
 * usb_cdc_poll() from the task context).
 */
#ifndef RBP_WCH_USB_CDC_H
#define RBP_WCH_USB_CDC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_cdc_init(void);
void usb_cdc_task(uint32_t now_ms); /* task-context pump */

/* All functions below are task-context only; none may call product code
 * from an ISR. IRQ masking is internal and preserves the caller state. */
bool usb_cdc_take_lost(void); /* reset/deconfigure/suspend/overflow; task consumes */

/* bytes received from the host (device copies them out inside the ISR) */
uint16_t usb_cdc_read(uint8_t *buf, uint16_t cap);
/* queue bytes for the host; returns accepted count */
uint16_t usb_cdc_write(const uint8_t *buf, uint16_t len);
/* try to move queued bytes into EP2 IN; call from task context */
void usb_cdc_flush_tx(void);
bool usb_cdc_dtr(void);

#ifdef __cplusplus
}
#endif
#endif
