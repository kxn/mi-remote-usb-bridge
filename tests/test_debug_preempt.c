#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../firmware/debug/trace.h"
static unsigned locked,locks,inject;
uint32_t trace_test_lock(void){assert(!locked);locked=1;return ++locks;}
void trace_test_unlock(uint32_t s){(void)s;assert(locked);locked=0;if(inject){inject=0;dt_log(DT_SYS,1,9,99,0,0,0);}}
int main(void){
    dt_record out[16];uint32_t seq,drop,high;
    dt_config(1,1);
    unsigned before=locks;
    dt_log(DT_USB,1,1,0,0,0,0);dt_blob(DT_SYS,10,0,120);
    assert(locks==before); /* Filtering must not disable interrupts. */
    dt_log(DT_SYS,1,1,11,0,0,0);inject=1;
    assert(dt_peek(out,16)==1 && out[0].a==11);
    dt_consume(1);assert(dt_peek(out,16)==1 && out[0].a==99);dt_consume(1);
    for(unsigned i=0;i<16;i++)dt_log(DT_SYS,1,1,i,0,0,0);
    inject=1;assert(dt_peek(out,16)==16);
    for(unsigned i=0;i<16;i++)assert(out[i].a==i);
    dt_stats(&seq,&drop,&high);assert(drop==1 && high==16);
    puts("trace preemption: concurrent append/full-ring drop preserves snapshot; filters do not mask IRQs");
}
