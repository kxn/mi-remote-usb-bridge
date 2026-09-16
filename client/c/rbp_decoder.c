#include "rbp_decoder.h"
#include <string.h>
#include <stddef.h>
#ifdef RBP_WITH_ICO
int ico_init_buffer(void *state,unsigned bytes);
int ico_decode(void *state,const uint8_t *frame,int len,int16_t *pcm);
#endif
#define LEGACY_SIZE ((uint32_t)offsetof(rbp_decoder_t,codec_id))
int rbp_decoder_init(rbp_decoder_t *d,uint32_t size,const rbp_audio_format_t *f) {
    if(!d || (size!=sizeof *d && size!=LEGACY_SIZE))return RBP_DECODE_CONFIG;
    memset(d,0,size);d->size=size;d->abi_version=1;
#ifdef RBP_WITH_ICO
    if(f && f->codec_id==RBP_CODEC_IFLYTEK_ICO && f->codec_revision==1) {
        if(size!=sizeof *d)return RBP_DECODE_CONFIG;
        if(f->channels!=1 || f->sample_rate!=16000 || f->config_len || f->max_unit_bytes!=40)
            return RBP_DECODE_CONFIG;
        if(ico_init_buffer(d->codec_state,sizeof d->codec_state))return RBP_DECODE_CONFIG;
        d->codec_id=RBP_CODEC_IFLYTEK_ICO;d->max_unit_bytes=40;d->sample_rate=16000;d->ready=1;
        return RBP_DECODE_OK;
    }
#endif
    if(!f || f->codec_id!=RBP_CODEC_IMA_HI || f->codec_revision!=1)return RBP_DECODE_UNSUPPORTED;
    if(f->channels!=1 || (f->sample_rate!=8000 && f->sample_rate!=16000) ||
       !f->config || f->config_len!=4 || f->config[2]>88 || f->config[3] ||
       !f->max_unit_bytes || f->max_unit_bytes>RBP_AUDIO_UNIT_MAX)return RBP_DECODE_CONFIG;
    uint16_t pred=(uint16_t)f->config[0]|((uint16_t)f->config[1]<<8);
    ima_state_reset(&d->ima,(int16_t)pred,f->config[2]);
    d->sample_rate=f->sample_rate;d->max_unit_bytes=f->max_unit_bytes;d->ready=1;
    return RBP_DECODE_OK;
}
int rbp_decoder_decode(rbp_decoder_t *d,const uint8_t *encoded,uint32_t bytes,
                       int16_t *pcm,uint32_t capacity,uint32_t *written) {
    if(written)*written=0;
    if(!d || (d->size!=sizeof *d && d->size!=LEGACY_SIZE) || d->abi_version!=1 || !d->ready || !written ||
       !encoded || !pcm || !bytes || bytes>d->max_unit_bytes)return RBP_DECODE_INPUT;
#ifdef RBP_WITH_ICO
    if(d->size==sizeof *d && d->codec_id==RBP_CODEC_IFLYTEK_ICO) {
        if(bytes!=40)return RBP_DECODE_INPUT;
        if(capacity<320)return RBP_DECODE_CAPACITY;
        if(ico_decode(d->codec_state,encoded,40,pcm)!=320)return RBP_DECODE_INPUT;
        *written=320;return RBP_DECODE_OK;
    }
#endif
    if(capacity<bytes*2u)return RBP_DECODE_CAPACITY;
    *written=(uint32_t)ima_decode(&d->ima,encoded,bytes,pcm,capacity,&d->ima);
    return RBP_DECODE_OK;
}
