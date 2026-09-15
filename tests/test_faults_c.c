#include "../firmware/product/faults.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    rbp_fault_t out[4];uint32_t seq,evicted;
    assert(rbp_fault_snapshot(out,&seq,&evicted)==0 && seq==0);
    rbp_fault_set_time(1234);rbp_fault_record(1,0x86,5,0);rbp_fault_record(1,0x86,5,0);
    assert(rbp_fault_snapshot(out,&seq,&evicted)==1&&seq==2&&out[0].count==2&&out[0].code==5&&out[0].board_ms==1234);
    for(unsigned i=0;i<5;i++)rbp_fault_record(2,i,0xabcd1234,i);
    assert(rbp_fault_snapshot(out,&seq,&evicted)==4&&seq==7&&evicted==2);
    for(unsigned i=0;i<4;i++)assert(out[i].sequence==i+4&&out[i].stage==i+1&&out[i].code==0xabcd1234);
    assert(rbp_fault_snapshot(out,&seq,&evicted)==4); /* read does not consume history */
    puts("release faults: raw 32-bit codes, coalescing, bounded eviction, nondestructive snapshot passed");
}
