#include "rbp_input.h"
#include <string.h>
static void emit(rbp_input_t *s,unsigned kind,unsigned key,bool synthetic,uint64_t time,uint32_t seq,uint8_t reason) {
    rbp_input_event_t e={s->source,time,seq,(uint16_t)key,(uint8_t)kind,reason,synthetic};
    if(s->emit)s->emit(s->user,&e);
}
void rbp_input_init(rbp_input_t*s,rbp_input_emit_t cb,void*u){memset(s,0,sizeof *s);s->emit=cb;s->user=u;}
void rbp_input_reset(rbp_input_t*s,uint8_t reason) {
    if(s->ready) {
        uint64_t held=s->bits;s->bits=0;s->ready=false;
        for(unsigned i=0;i<s->count;i++)if(held&((uint64_t)1<<i))emit(s,RBP_INPUT_UP,s->key_ids[i],true,0,0,reason);
        emit(s,RBP_INPUT_RESET,0,true,0,0,reason);
    }
    s->count=0;s->have_sequence=false;s->sequence=0;memset(&s->source,0,sizeof s->source);
}
bool rbp_input_install(rbp_input_t*s,const rbp_input_source_t*source,const uint16_t*ids,unsigned count) {
    if(!source||!memchr(source->model_id,0,sizeof source->model_id)||!source->session_id||!source->peer_id||!source->connection_id||count>64||(count&&!ids))return false;
    for(unsigned i=0;i<count;i++) {
        if(!ids[i])return false;
        for(unsigned j=0;j<i;j++)if(ids[j]==ids[i])return false;
    }
    rbp_input_reset(s,0);s->source=*source;s->count=count;
    if(count)memcpy(s->key_ids,ids,count*sizeof *ids);
    s->ready=true;return true;
}
bool rbp_input_has_key(const rbp_input_t*s,uint16_t id) {
    if(!s->ready)return false;
    for(unsigned i=0;i<s->count;i++)if(s->key_ids[i]==id)return true;
    return false;
}
bool rbp_input_feed(rbp_input_t*s,uint32_t seq,uint64_t time,uint64_t bits,uint8_t kind,uint8_t reason) {
    if(!s->ready||kind>2||reason>3||(kind==2&&bits)||(s->count<64&&(bits>>s->count)))return false;
    if(s->have_sequence&&(seq<s->sequence||(seq==s->sequence&&kind!=2)))return true;
    uint64_t previous=s->bits;s->bits=bits;s->sequence=seq;s->have_sequence=true;
    if(!kind){emit(s,RBP_INPUT_SNAPSHOT,0,true,time,seq,reason);return true;}
    for(unsigned i=0;i<s->count;i++)if((previous&~bits)&((uint64_t)1<<i))emit(s,RBP_INPUT_UP,s->key_ids[i],kind==2,time,seq,reason);
    for(unsigned i=0;i<s->count;i++)if((bits&~previous)&((uint64_t)1<<i))emit(s,RBP_INPUT_DOWN,s->key_ids[i],false,time,seq,reason);
    if(kind==2)emit(s,RBP_INPUT_RESET,0,true,time,seq,reason);
    return true;
}
