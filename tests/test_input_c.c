#include "rbp_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static rbp_input_event_t events[256];
static unsigned count;
static void record(void *user,const rbp_input_event_t *e){(void)user;assert(count<256);events[count++]=*e;}
int main(void) {
    rbp_input_t a,b;rbp_input_init(&a,record,NULL);rbp_input_init(&b,record,NULL);
    rbp_input_source_t source={.receiver_id=1,.session_id=2,.peer_id=3,.connection_id=4,.catalog_revision=1};
    strcpy(source.model_id,"unicom.hid_ico.v1");
    uint16_t ids[]={RBP_KEY_MUTE,RBP_KEY_UP};
    assert(rbp_input_install(&a,&source,ids,2));
    assert(rbp_input_has_key(&a,RBP_KEY_UP));assert(!rbp_input_has_key(&a,RBP_KEY_TV));
    assert(rbp_input_feed(&a,1,100,1,0,0));
    assert(count==1&&events[0].kind==RBP_INPUT_SNAPSHOT);
    assert(rbp_input_feed(&a,2,200,2,1,0));
    assert(count==3&&events[1].kind==RBP_INPUT_UP&&events[1].key_id==RBP_KEY_MUTE);
    assert(events[2].kind==RBP_INPUT_DOWN&&events[2].key_id==RBP_KEY_UP);
    assert(rbp_input_feed(&a,1,100,1,0,0)&&a.bits==2&&count==3);
    assert(!rbp_input_feed(&a,3,300,4,1,0));
    source.receiver_id=2;assert(rbp_input_install(&b,&source,ids,2));
    assert(rbp_input_feed(&b,1,100,2,1,0));
    rbp_input_reset(&a,0);assert(!a.ready&&b.bits==2);
    assert(events[count-2].synthetic&&events[count-2].source.receiver_id==1);
    assert(rbp_input_feed(&b,2,200,0,2,1)&&b.ready&&b.bits==0);
    assert(events[count-1].kind==RBP_INPUT_RESET);
    uint16_t full[64];for(unsigned i=0;i<64;i++)full[i]=0x8000+i;
    assert(rbp_input_install(&a,&source,full,64));
    assert(rbp_input_feed(&a,1,100,UINT64_C(1)<<63,1,0));assert(events[count-1].key_id==0x803f);
    full[1]=full[0];assert(!rbp_input_install(&a,&source,full,64));assert(a.bits==(UINT64_C(1)<<63));
    memset(source.model_id,'x',sizeof source.model_id);assert(!rbp_input_install(&a,&source,ids,2));
    puts("logical input: OK");return 0;
}
