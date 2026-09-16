/* Compile the actual board store against fault-injected EEPROM. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/wch/board.c"
static uint8_t nv[32768];
static int fail_write=-1, writes;
static unsigned partial;
void mDelaymS(uint16_t n){(void)n;}
uint8_t FLASH_EEPROM_CMD(uint8_t c,uint32_t a,void*p,uint32_t n){(void)c;(void)a;memset(p,0x42,n);return 0;}
uint8_t EEPROM_READ(uint32_t a,void*p,uint32_t n){assert(a+n<=sizeof nv);memcpy(p,nv+a,n);return 0;}
uint8_t EEPROM_ERASE(uint32_t a,uint32_t n){assert(a>=BOARD_CACHE_BASE&&a+n<=BLE_SNV_ADDR);assert(!(a%256)&&n==256);memset(nv+a,255,n);return 0;}
uint8_t EEPROM_WRITE(uint32_t a,void*p,uint32_t n){
    assert(a>=BOARD_CACHE_BASE&&a+n<=BLE_SNV_ADDR);
    if(writes++==fail_write){if(partial>n)partial=n;memcpy(nv+a,p,partial);return 1;}
    memcpy(nv+a,p,n);return 0;
}
static void reboot(void){gac_loaded=false;gac_slot=-1;gac_generation=0;loaded=false;current_slot=-1;storage_state=0;staged_valid=false;memset(current,0,sizeof current);}
int main(void){
    for(unsigned step=0;step<2;step++)for(unsigned cut=0;cut<(step?4:252);cut++) {
        memset(nv,255,sizeof nv);reboot();writes=0;fail_write=step;partial=cut;
        assert(!board_begin_pair());reboot();assert(board_storage_state()==0||board_storage_state()==3);
        fail_write=-1;assert(board_store.clear(NULL));reboot();assert(board_storage_state()==1);
    }
    fail_write=-1;
    memset(nv,255,sizeof nv);reboot();assert(board_storage_state()==0);
    assert(board_begin_pair());assert(board_peer_counter()==1);
    uint8_t addr[]={1,2,3,4,5,6},out[6],type;board_addr_stage(addr,1);
    assert(board_hid_flags_load()==255);board_hid_flags_stage(2);
    rbp_peer_record_t peer={0},read={0};peer.peer_id=1;peer.auto_reconnect=true;strcpy(peer.name,"remote");
    assert(board_store.save(NULL,&peer));reboot();assert(board_store.load(NULL,&read));assert(read.peer_id==1);
    assert(board_addr_load(out,&type)&&type==1&&!memcmp(addr,out,6));
    assert(board_hid_flags_load()==2);
    uint8_t baseline[sizeof nv];memcpy(baseline,nv,sizeof nv);
    /* Cache is independent, peer-scoped, idempotent, and durable on reboot. */
    uint8_t blob[BOARD_CACHE_MAX],restored[BOARD_CACHE_MAX];memset(blob,0x35,sizeof blob);
    assert(board_cache_save(1,blob,sizeof blob));int before=writes;
    assert(board_cache_save(1,blob,sizeof blob) && writes==before);
    reboot();assert(board_cache_load(1,restored,sizeof restored)==sizeof blob && !memcmp(blob,restored,sizeof blob));
    assert(!board_cache_load(2,restored,sizeof restored));
    uint8_t cache_baseline[sizeof nv];memcpy(cache_baseline,nv,sizeof nv);
    blob[0]=0x36;
    for(unsigned step=0;step<2;step++)for(unsigned cut=0;cut<(step?4:508);cut++) {
        memcpy(nv,cache_baseline,sizeof nv);reboot();writes=0;fail_write=step;partial=cut;
        assert(!board_cache_save(1,blob,sizeof blob));reboot();
        assert(board_cache_load(1,restored,sizeof restored)==sizeof blob && restored[0]==0x35);
        assert(board_store.load(NULL,&read) && read.peer_id==1);
    }
    fail_write=-1;memcpy(nv,cache_baseline,sizeof nv);reboot();
    assert(board_cache_save(1,NULL,0));reboot();assert(!board_cache_load(1,restored,sizeof restored));
    /* Saving another setting cannot resurrect an invalidated cache. */
    assert(board_store.save(NULL,&peer));reboot();assert(!board_cache_load(1,restored,sizeof restored));
    memcpy(nv,baseline,sizeof nv);reboot();assert(board_storage_state()==1);
    /* Legacy RBS2 reserves byte 30 as 0xff: restore as UNKNOWN without re-pairing. */
    uint8_t *legacy=nv+STORE_BASE+current_slot*256;legacy[30]=255;
    put32(legacy+CRC_OFF,rbp_crc32c(legacy,CRC_OFF));reboot();assert(board_hid_flags_load()==255);
    /* Any torn body/commit while updating preserves the old committed record. */
    for(unsigned step=0;step<2;step++)for(unsigned cut=0;cut<(step?4:252);cut++){
        memcpy(nv,baseline,sizeof nv);reboot();writes=0;fail_write=step;partial=cut;
        peer.auto_reconnect=false;assert(!board_store.save(NULL,&peer));reboot();
        assert(board_store.load(NULL,&read)&&read.peer_id==1&&read.auto_reconnect);
        assert(board_hid_flags_load()==2);
    }
    fail_write=-1;memcpy(nv,baseline,sizeof nv);reboot();
    assert(board_begin_forget());reboot();assert(board_storage_state()==2);assert(!board_store.load(NULL,&read));
    assert(board_store.clear(NULL));reboot();assert(board_store.load(NULL,&read)&&read.peer_id==0);
    assert(board_begin_pair()&&board_peer_counter()==2);reboot();assert(board_storage_state()==3);
    assert(board_store.clear(NULL));assert(board_begin_pair()&&board_peer_counter()==3);
    /* Identity bytes are covered by CRC; corrupt newest slot falls back to journal. */
    board_addr_stage(addr,1);peer.peer_id=3;assert(board_store.save(NULL,&peer));
    nv[STORE_BASE+current_slot*256+24]^=1;reboot();assert(board_storage_state()==3);
    puts("board: torn body/commit, journal recovery, identity CRC, monotonic peer passed");return 0;
}
