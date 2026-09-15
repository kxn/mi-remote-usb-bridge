#include "rbp_decoder.h"
#include <string.h>
int rbp_decoder_init(rbp_decoder_t *d,uint32_t size,const rbp_audio_format_t *f) {
    if(!d || size!=sizeof *d)return RBP_DECODE_CONFIG;
    memset(d,0,sizeof *d);d->size=size;d->abi_version=1;
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
    if(!d || d->size!=sizeof *d || d->abi_version!=1 || !d->ready || !written ||
       !encoded || !pcm || !bytes || bytes>d->max_unit_bytes)return RBP_DECODE_INPUT;
    if(capacity<bytes*2u)return RBP_DECODE_CAPACITY;
    *written=(uint32_t)ima_decode(&d->ima,encoded,bytes,pcm,capacity,&d->ima);
    return RBP_DECODE_OK;
}
