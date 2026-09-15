#include <assert.h>
#include <stdio.h>
#define RBP_DEBUG
#define RBP_SDK_RX_PROBE
#include "../firmware/wch/sdk_rx_probe.c"

volatile uint8_t gBleIPPara[32];
static volatile uint32_t aes[16],bb[16];
volatile uint32_t *gptrAESReg=aes,*gptrBBReg=bb;
static uint8_t packet[32];
static uint32_t wanted,result_calls,arm_calls,logs;
static dt_record emitted[32];
uint32_t __real_ble_ll_chkcrc(void *p,uint32_t enc,void *r,void *f) {
    assert(p==packet && enc==1 && r==(void *)1 && f==(void *)2);
    ++result_calls;return wanted;
}
uint32_t __real_AES_DevPktDec(uint32_t direction,void *conn) {
    assert(direction==0 && conn==(void *)3);++arm_calls;
    gBleIPPara[6]=128;return 19;
}
void dt_log(uint8_t m,uint8_t l,uint16_t code,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {
    assert(logs<32);emitted[logs++]=(dt_record){.module=m,.level=l,.code=code,.a=a,.b=b,.c=c,.d=d};
}
static void check(uint32_t value) {
    wanted=value;uint32_t calls=result_calls,oldlogs=logs;
    assert(__wrap_ble_ll_chkcrc(packet,1,(void *)1,(void *)2)==value);
    assert(result_calls==calls+1 && logs==oldlogs);
}
int main(void) {
    assert(__wrap_AES_DevPktDec(0,(void *)3)==19 && arm_calls==1);
    packet[0]=0x0a;packet[1]=5;packet[14]=0x81;
    aes[0]=0x1234;bb[14]=0x5678;gBleIPPara[6]=0;
    check(0);check(1);check(2);
    assert(count==1 && pending[0].before==0 && pending[0].arm_state==128);
    assert(pending[0].header==0x050a && pending[0].tail==0xffffffff);
    /* Snapshot must not drift when hardware/packet changes before poll. */
    aes[0]=0;bb[14]=0;packet[0]=0;gBleIPPara[6]=64;
    sdk_rx_probe_poll();assert(logs==4 && count==0);
    assert(emitted[0].code==5 && emitted[0].a==2 && emitted[0].c==0);
    assert(emitted[1].b==0x1234 && emitted[1].c==0x5678);
    assert(emitted[2].b==0x050a && emitted[2].c==0xffffffff);
    assert(emitted[3].b==1 && emitted[3].c==128);
    check(6);check(14);check(2);check(2);check(14);
    assert(count==4 && snapshot_dropped==1);
    assert(pending[1].tail==0x81 && pending[2].tail==0x81);
    while(count)sdk_rx_probe_poll();
    assert(reported_dropped==1);
    uint32_t total,crc;sdk_rx_probe_counts(&total,&crc);
    assert(total==8 && crc==1);
    assert(aes[0]==0 && bb[14]==0 && gBleIPPara[6]==64);
    puts("SDK observer: arguments/results preserved; receive path never logs; stable snapshots and explicit overflow");
}
