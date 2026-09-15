#include <assert.h>
#include <stdio.h>
#include "../firmware/debug/trace.h"
int main(void){
    _Static_assert(sizeof(dt_record)==28,"debug wire record");
    dt_record batch[16];uint32_t seq,drop,high;
    dt_config(1u<<DT_GATT,3);dt_time(42);
    dt_log(DT_USB,2,1,0,0,0,0);
    for(unsigned i=0;i<20;i++)dt_log(DT_GATT,3,2,i,0,0,0);
    dt_stats(&seq,&drop,&high);assert(seq==20&&drop==4&&high==16);
    assert(dt_peek(batch,16)==16&&batch[0].ms==42&&batch[15].a==15);
    /* Backpressure must not pop or reorder a record. */
    assert(dt_peek(batch,1)==1&&batch[0].sequence==1);
    dt_consume(16);assert(!dt_peek(batch,16));
    dt_blob(DT_GATT,10,NULL,120); /* filtered before touching source */
    assert(!dt_peek(batch,16));
    dt_config(255,4);uint8_t blob[19];for(unsigned i=0;i<19;i++)blob[i]=i;
    dt_blob(DT_HID,10,blob,19);assert(dt_peek(batch,16)==2);
    assert(batch[0].a==(19u<<16)&&batch[1].a==((19u<<16)|12));
    assert(batch[0].b==0x03020100&&batch[1].b==0x0f0e0d0c);
    puts("debug: bounded ring, overflow accounting, filters, ordering, blobs passed");
}
