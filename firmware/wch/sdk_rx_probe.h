#ifndef RBP_SDK_RX_PROBE_H
#define RBP_SDK_RX_PROBE_H
#include <stdint.h>
void sdk_rx_probe_counts(uint32_t *checks, uint32_t *crc_failed);
void sdk_rx_probe_poll(void);
#endif
