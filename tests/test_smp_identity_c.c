#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/wch/wch_smp_identity.c"
static unsigned calls;
static uint8_t expected_type;
static bool verify_commit;
static uint8_t *identity_record;
uint8_t __real_smpInitiatorProcessIncoming(linkDBItem_t *l,uint8_t op,void *p) {
    (void)l;calls++;
    if(verify_commit) {
        assert(op==9);
        /* Real SDK copies six address bytes, then may immediately finish pairing. */
        memcpy(identity_record+16,(uint8_t*)p+1,6);
        assert(identity_record[22]==expected_type);
    }
    return 0x42;
}
int main(void) {
    uint8_t pair[120+sizeof(void*)],id[23],msg[7]={0,0x40,0xb2,0xc2,0x39,0x5d,0xc0};
    linkDBItem_t link={0};link.connectionHandle=7;link.pPairingParams=pair;
    identity_record=id;
    for(unsigned irk_first=0;irk_first<256;irk_first++)for(unsigned type=0;type<2;type++) {
        memset(pair,0,sizeof pair);memset(id,0xa5,sizeof id);
        memcpy(pair,&link.connectionHandle,2);pair[2]=1;pair[3]=0x25;
        memcpy(pair+120,&identity_record,sizeof identity_record);
        id[22]=(uint8_t)irk_first;msg[0]=expected_type=(uint8_t)type;verify_commit=true;
        assert(__wrap_smpInitiatorProcessIncoming(&link,9,msg)==0x42);
        assert(!memcmp(id+16,msg+1,6));
        for(unsigned i=0;i<16;i++)assert(id[i]==0xa5); /* IRK unchanged */
    }
    verify_commit=false;
    for(unsigned type=2;type<256;type++) {
        unsigned before=calls;msg[0]=(uint8_t)type;id[22]=0x68;
        assert(__wrap_smpInitiatorProcessIncoming(&link,9,msg)==SMP_PAIRING_FAILED_INVALID_PARAMERERS);
        assert(calls==before && id[22]==0x68);
    }
    msg[0]=1;id[22]=0x68;
    assert(__wrap_smpInitiatorProcessIncoming(&link,8,msg)==0x42);assert(id[22]==0x68);
    pair[3]=0x24;assert(__wrap_smpInitiatorProcessIncoming(&link,9,msg)==0x42);assert(id[22]==0x68);
    pair[3]=0x25;pair[0]=8;assert(__wrap_smpInitiatorProcessIncoming(&link,9,msg)==0x42);assert(id[22]==0x68);
    pair[0]=7;pair[2]=0;assert(__wrap_smpInitiatorProcessIncoming(&link,9,msg)==0x42);assert(id[22]==0x68);
    pair[2]=1;uint8_t *none=NULL;memcpy(pair+120,&none,sizeof none);
    assert(__wrap_smpInitiatorProcessIncoming(&link,9,msg)==0x42);
    puts("SMP identity: 512 type/IRK cases, 254 invalid types, state/handle/opcode gates, key preservation passed");
}
