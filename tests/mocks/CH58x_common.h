#ifndef TEST_CH58X_COMMON_H
#define TEST_CH58X_COMMON_H
#include <stdint.h>
#include "CH583SFR.h"
#define __HIGH_CODE
#define USB_IRQn 0
#define PFIC_SetPriority(x,p) ((void)(x),(void)(p))
#define PFIC_EnableIRQ(x) ((void)(x))
#define PFIC_DisableIRQ(x) ((void)(x))
void mDelaymS(uint16_t ms);
uint8_t EEPROM_READ(uint32_t addr,void *data,uint32_t len);
uint8_t EEPROM_WRITE(uint32_t addr,void *data,uint32_t len);
uint8_t EEPROM_ERASE(uint32_t addr,uint32_t len);
#define CMD_GET_UNIQUE_ID 1
uint8_t FLASH_EEPROM_CMD(uint8_t cmd,uint32_t addr,void *data,uint32_t len);
#endif
