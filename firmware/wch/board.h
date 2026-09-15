/* Board EEPROM: two 256-byte pages below the SDK SNV reservation. */
#ifndef RBP_WCH_BOARD_H
#define RBP_WCH_BOARD_H

#include "rbp_server.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_BASE       0x00007C00u
#define STORE_SLOT_SIZE  256u
#define STORE_SLOT_COUNT 2u

/* The BLE stack SNV occupies 0x7E00..0x8000; our region must stay below. */
_Static_assert(STORE_BASE + STORE_SLOT_SIZE * STORE_SLOT_COUNT <= 0x7E00,
               "store overlaps BLE SNV region");

uint8_t board_storage_state(void); /* 0 empty, 1 committed, 2 deleting, 3 pending pair, 255 corrupt */
uint32_t board_peer_counter(void);
bool board_begin_pair(void);
bool board_begin_forget(void);
void board_init(void);
void board_led(bool on);
void board_led_blink(uint8_t count);

/* stable 16-byte chip unique id (flash UID page) */
const uint8_t *board_unique_id(void);

/* radio-layer peer address, committed atomically with the product record
 * (stored in the free tail of the same 256-byte slot) */
void board_addr_stage(const uint8_t addr[6], uint8_t type); /* take effect on next save */
void board_addr_invalidate(void);
bool board_addr_load(uint8_t out_addr[6], uint8_t *out_type);
void board_hid_flags_stage(uint8_t flags); /* 0xff means unknown; next record commit */
uint8_t board_hid_flags_load(void);

extern const rbp_store_t board_store;

#ifdef __cplusplus
}
#endif
#endif
