#include "../product/faults.h"
#include "board.h"
#include "../debug/trace.h"
#include "CH58x_common.h"
#include "CONFIG.h"
#include "rbp/frame.h"
#include <string.h>
static uint32_t checked_read(uint32_t addr,void *data,uint32_t len) {
    uint32_t rc=EEPROM_READ(addr,data,len);if(rc)rbp_fault_record(RBP_FAULT_STORAGE,1,rc,0);return rc;
}
static uint32_t checked_write(uint32_t addr,void *data,uint32_t len) {
    uint32_t rc=EEPROM_WRITE(addr,data,len);if(rc)rbp_fault_record(RBP_FAULT_STORAGE,2,rc,0);return rc;
}
static uint32_t checked_erase(uint32_t addr,uint32_t len) {
    uint32_t rc=EEPROM_ERASE(addr,len);if(rc)rbp_fault_record(RBP_FAULT_STORAGE,3,rc,0);return rc;
}
static int checked_verify(const void *expected,const void *actual,uint32_t len) {
    int rc=memcmp(expected,actual,len);if(rc)rbp_fault_record(RBP_FAULT_STORAGE,4,1,0);return rc;
}
#ifndef BOARD_LED_PB4
#define BOARD_LED_PB4 0
#endif
_Static_assert(STORE_BASE + STORE_SLOT_SIZE * STORE_SLOT_COUNT <= BLE_SNV_ADDR, "EEPROM overlaps SDK SNV");
void board_init(void)
{
    /* clock is configured in main() before C runtime; LED pin here */
    #if BOARD_LED_PB4
    GPIOB_ModeCfg(GPIO_Pin_4, GPIO_ModeOut_PP_5mA);
#endif
    board_led(false);
}

void board_led(bool on)
{
    /* WeAct CH582F core board: active-low blue LED on PB4 (verify on
     * schematic before production; development builds blink with this). */
#if BOARD_LED_PB4
    if (on) GPIOB_ResetBits(GPIO_Pin_4);
    else GPIOB_SetBits(GPIO_Pin_4);
#else
    (void)on;
#endif
}

void board_led_blink(uint8_t count)
{
    while (count--) {
        board_led(true);
        mDelaymS(60);
        board_led(false);
        mDelaymS(120);
    }
}

const uint8_t *board_unique_id(void)
{
    /* 8-byte UID lives in the flash info block; expose 16 bytes by
     * doubling it (bridge_uid needs 16 stable bytes). */
    static uint8_t uid[16];
    static bool have;
    if (!have) {
        uint8_t raw[8]={0};
        if(FLASH_EEPROM_CMD(CMD_GET_UNIQUE_ID, 0, raw, 8)!=0)return NULL;
        memcpy(uid, raw, 8);
        memcpy(uid + 8, raw, 8);
        have = true;
    }
    return uid;
}


/* RBS2: full data protected by CRC, separate final commit word. Two independent
 * EEPROM erase pages; never use program-FLASH APIs. Generation/counter do not
 * wrap. State 2/3 records durable delete/pair intent spanning SDK bond storage. */
#define CRC_OFF 248u
#define COMMIT_OFF 252u
#define COMMIT_WORD 0x43544252u
/* Only these committed fields are needed in RAM; CRC/commit are verified on
 * the full EEPROM page before copying this prefix. */
static uint8_t current[80];
static int current_slot=-1;
static uint8_t storage_state;
static bool loaded;
static uint8_t staged_addr[6], staged_type;
static bool staged_valid;
static uint8_t staged_hid_flags=255;
static uint32_t get32(const uint8_t*p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put32(uint8_t*p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static bool valid(const uint8_t*p) {
    return !memcmp(p,"RBS2",4) && get32(p+COMMIT_OFF)==COMMIT_WORD &&
        get32(p+CRC_OFF)==rbp_crc32c(p,CRC_OFF) && p[8]>=1 && p[8]<=3 &&
        p[21]<=1 && p[22]<=1 && p[23]<=1;
}
static void load_slots(void) {
    if(loaded)return;
    loaded=true;storage_state=0;bool dirty=false;
    uint8_t page[STORE_SLOT_SIZE];
    for(unsigned i=0;i<STORE_SLOT_COUNT;i++) {
        if(checked_read(STORE_BASE+i*STORE_SLOT_SIZE,page,sizeof page)!=0) {storage_state=255;return;}
        for(unsigned j=0;j<sizeof page;j++)if(page[j]!=255)dirty=true;
        if(valid(page) && (current_slot<0 || get32(page+4)>get32(current+4))) {
            memcpy(current,page,sizeof current);current_slot=(int)i;
        }
    }
    if(current_slot>=0) {
        storage_state=current[8];staged_valid=current[22]!=0;staged_type=current[23];
        memcpy(staged_addr,current+24,6);
        staged_hid_flags=current[30]<=3?current[30]:255;
    } else if(dirty) {
        /* A first-ever torn write has no committed record. If the body says
         * RBS2 pending intent, recovery may complete its rollback. Arbitrary
         * corruption is an error, never an instruction to erase SDK bonds. */
        storage_state=255;
        for(unsigned i=0;i<STORE_SLOT_COUNT;i++) {
            if(checked_read(STORE_BASE+i*STORE_SLOT_SIZE,page,sizeof page)!=0)return;
            if(!memcmp(page,"RBS2",4) && page[8]==3 &&
               get32(page+CRC_OFF)==rbp_crc32c(page,CRC_OFF)) {
                memcpy(current,page,sizeof current);current_slot=(int)i;storage_state=3;return;
            }
        }
        /* Before the very first SDK bond mutation, a torn initial pending
         * journal can only occupy slot zero; slot one must still be erased.
         * Recognize only bytes compatible with that exact initial record,
         * including partial 1->0 programming. Never infer an arbitrary
         * corrupt committed record to mean "erase somebody's bond". */
        uint8_t expected[STORE_SLOT_SIZE];memset(expected,255,sizeof expected);
        memcpy(expected,"RBS2",4);put32(expected+4,1);expected[8]=3;
        put32(expected+12,1);put32(expected+16,0);expected[21]=expected[22]=expected[23]=0;
        memset(expected+24,0,6);memset(expected+32,0,48);
        put32(expected+CRC_OFF,rbp_crc32c(expected,CRC_OFF));put32(expected+COMMIT_OFF,COMMIT_WORD);
        if(checked_read(STORE_BASE+STORE_SLOT_SIZE,page,sizeof page)!=0)return;
        for(unsigned j=0;j<sizeof page;j++)if(page[j]!=255)return;
        if(checked_read(STORE_BASE,page,sizeof page)!=0)return;
        for(unsigned j=0;j<sizeof page;j++)if((page[j]&expected[j])!=expected[j])return;
        memcpy(current,expected,sizeof current);current_slot=0;storage_state=3;return;
    }
}
static bool write_record(uint8_t state,const rbp_peer_record_t *rec,uint32_t counter) {
    DT(DT_STORE,DT_INFO,1,state,counter,storage_state,0);
    load_slots();if(storage_state==255)return false;
    uint32_t gen=current_slot<0?0:get32(current+4);
    if(gen==UINT32_MAX)return false;
    uint8_t page[STORE_SLOT_SIZE],check[STORE_SLOT_SIZE];memset(page,255,sizeof page);
    memcpy(page,"RBS2",4);put32(page+4,gen+1);page[8]=state;
    put32(page+12,counter);put32(page+16,rec?rec->peer_id:0);
    page[21]=rec && rec->auto_reconnect;page[22]=rec && staged_valid;page[23]=staged_type;
    memcpy(page+24,staged_addr,6);memset(page+32,0,48);
    page[30]=rec?staged_hid_flags:255;
    if(rec)memcpy(page+32,rec->name,47);
    put32(page+CRC_OFF,rbp_crc32c(page,CRC_OFF));
    unsigned slot=current_slot<0?0:((unsigned)current_slot+1)%STORE_SLOT_COUNT;
    uint32_t addr=STORE_BASE+slot*STORE_SLOT_SIZE;
    if(checked_erase(addr,STORE_SLOT_SIZE)!=0 ||
       checked_write(addr,page,COMMIT_OFF)!=0 ||
       checked_read(addr,check,sizeof check)!=0 || checked_verify(page,check,COMMIT_OFF)) {DT(DT_STORE,DT_ERROR,3,state,1,slot,0);return false;}
    put32(page+COMMIT_OFF,COMMIT_WORD);
    if(checked_write(addr+COMMIT_OFF,page+COMMIT_OFF,4)!=0 ||
       checked_read(addr,check,sizeof check)!=0 || checked_verify(page,check,sizeof page)) {DT(DT_STORE,DT_ERROR,3,state,2,slot,0);return false;}
    memcpy(current,page,sizeof current);current_slot=slot;storage_state=state;
    DT(DT_STORE,DT_INFO,2,state,gen+1,slot,counter);return true;
}
uint8_t board_storage_state(void) {load_slots();return storage_state;}
uint32_t board_peer_counter(void) {load_slots();return current_slot<0?0:get32(current+12);}
bool board_begin_pair(void) {
    load_slots();uint32_t counter=board_peer_counter();
    if(storage_state==255 || (storage_state==1 && get32(current+16)) || counter==UINT32_MAX)return false;
    staged_valid=false;staged_hid_flags=255;return write_record(3,NULL,counter+1);
}
bool board_begin_forget(void) {return write_record(2,NULL,board_peer_counter());}
void board_addr_stage(const uint8_t addr[6],uint8_t type) {load_slots();memcpy(staged_addr,addr,6);staged_type=type;staged_valid=true;}
void board_addr_invalidate(void) {load_slots();staged_valid=false;}
void board_hid_flags_stage(uint8_t flags) {load_slots();staged_hid_flags=flags<=3?flags:255;}
uint8_t board_hid_flags_load(void) {load_slots();return current_slot>=0 && current[30]<=3?current[30]:255;}
bool board_addr_load(uint8_t out[6],uint8_t *type) {
    load_slots();if(storage_state!=1 || !current[22] || !get32(current+16))return false;
    memcpy(out,current+24,6);*type=current[23];return true;
}
static bool store_load(void *user,rbp_peer_record_t *out) {
    (void)user;load_slots();if(storage_state!=1)return false;
    memset(out,0,sizeof *out);out->peer_id=get32(current+16);
    out->auto_reconnect=current[21]!=0;memcpy(out->name,current+32,47);return true;
}
static bool store_save(void *user,const rbp_peer_record_t *rec) {
    (void)user;if(!rec || !rec->peer_id)return false;
    return write_record(1,rec,board_peer_counter());
}
static bool store_clear(void *user) {(void)user;staged_valid=false;return write_record(1,NULL,board_peer_counter());}
const rbp_store_t board_store={NULL,store_load,store_save,store_clear};

#include "cache_store.inc"
