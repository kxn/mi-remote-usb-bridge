#include <assert.h>
#include <stdio.h>
#include "rc003_atvv.h"
static unsigned pcm,starts,ends,faults,writes,formats;static uint8_t command[2];static bool busy;
static bool tx(void*u,const uint8_t*d,uint16_t n){(void)u;assert(n==2);command[0]=d[0];command[1]=d[1];writes++;return !busy;}
static void start(void*u,const rbp_audio_format_t*f){(void)u;assert(f->sample_rate==16000||f->sample_rate==8000);assert(f->config_len==4);starts++;}
static void data(void*u,const uint8_t*p,uint16_t n){(void)u;(void)p;pcm+=n*2;}
static void format(void*u,const rbp_audio_format_t*f){(void)u;assert(f->sample_rate==8000||f->sample_rate==16000);formats++;}
static void end(void*u,uint8_t r){(void)u;(void)r;ends++;}
static void faulted(void*u,uint8_t r){(void)u;(void)r;faults++;}
static bool want(void*u){(void)u;return true;}
int main(void){
    rc003_atvv_sink_t sink={.write_tx=tx,.on_started=start,.on_encoded=data,.on_format=format,.on_ended=end,.want_voice=want,.on_fault=faulted};rc003_atvv_t a;
    uint8_t caps[]={11,1,0,3,3,0,120,0,0},on[]={4,3,2,1},sync[]={10,2,0,0,0,0,0},stop[]={0,2},audio[120]={0};
    rc003_atvv_init(&a,&sink);assert(!rc003_atvv_on_control(&a,on,4));
    assert(!rc003_atvv_on_control(&a,caps,7));assert(rc003_atvv_on_control(&a,caps,9));
    uint8_t extended_caps[]={11,1,0,3,3,0,120,1,0,0x26,0x71};
    assert(atvv_parse_caps(extended_caps,sizeof extended_caps,&a.caps)&&a.caps.request_dle);
    extended_caps[7]=2;assert(!atvv_parse_caps(extended_caps,sizeof extended_caps,&a.caps));
    assert(atvv_parse_caps(caps,sizeof caps,&a.caps));
    assert(rc003_atvv_on_control(&a,on,4));rc003_atvv_on_audio(&a,audio,60);
    assert(pcm==0&&faults==1&&a.stream_active&&command[0]==13);
    assert(rc003_atvv_on_control(&a,stop,2));assert(rc003_atvv_on_control(&a,on,4));
    assert(rc003_atvv_on_control(&a,sync,7));
    rc003_atvv_on_audio(&a,audio,60);assert(a.frame_no==0&&a.frame_bytes==60);
    rc003_atvv_on_audio(&a,audio,60);assert(a.frame_no==1&&a.frame_bytes==0&&pcm==240);
    sync[3]=1;assert(rc003_atvv_on_control(&a,sync,7)&&formats==1); /* same rate still resets */
    a.last_audio_ms=4999;assert(rc003_atvv_tick(&a,5000)&&command[0]==14);
    sync[3]=1;sync[1]=1;assert(rc003_atvv_on_control(&a,sync,7));
    sync[6]=255;assert(!rc003_atvv_on_control(&a,sync,7)&&faults==2);
    assert(rc003_atvv_on_control(&a,stop,2));
    uint8_t search=8;assert(rc003_atvv_on_control(&a,&search,1)&&command[0]==12&&command[1]==1);
    assert(!rc003_atvv_on_control(&a,&search,1));rc003_atvv_tick(&a,6002);assert(faults==2&&a.open_pending);
    rc003_atvv_tick(&a,10000);assert(faults==3);
    caps[1]=0;caps[2]=4;assert(!atvv_parse_caps(caps,9,&a.caps));
    rc003_atvv_init(&a,&sink);a.profile_init_without_sync=true;
    uint8_t short_caps[]={11,1,0,3,0,0,120},requested[]={4,0,2,0};
    assert(!rc003_atvv_on_control(&a,short_caps,sizeof short_caps)&&!a.caps_valid);
    uint8_t swapped_caps[]={11,1,0,0,3,0,120,0,0};
    assert(!rc003_atvv_on_control(&a,swapped_caps,sizeof swapped_caps)&&!a.caps_valid);
    /* Actual RC003 capture: 16 kHz only, HTT, 120 bytes per audio frame. */
    uint8_t hardware_caps[]={11,1,0,2,3,0,120,0,0};
    assert(rc003_atvv_on_control(&a,hardware_caps,sizeof hardware_caps));
    assert(a.caps.codecs==2&&a.caps.interaction==3&&a.caps.frame_size==120);
    assert(a.sample_rate==16000&&!a.caps.request_dle);
    uint8_t requested_caps[]={11,1,0,3,0,0,120,0,0};
    assert(rc003_atvv_on_control(&a,requested_caps,sizeof requested_caps));
    busy=true;assert(rc003_atvv_request_start(&a)&&a.open_queued);
    unsigned before=writes;rc003_atvv_tick(&a,10);assert(writes==before+1);
    busy=false;rc003_atvv_tick(&a,20);assert(!a.open_queued&&a.open_pending);
    before=writes;rc003_atvv_tick(&a,30);assert(writes==before); /* accepted OPEN never replayed */
    rc003_atvv_tick(&a,1002);assert(a.open_pending&&!a.open_queued&&writes==before);
    assert(rc003_atvv_request_stop(&a)&&a.closing); /* release before START */
    assert(rc003_atvv_on_control(&a,requested,4)&&a.closing&&command[0]==13);
    assert(rc003_atvv_on_control(&a,stop,2));
    assert(rc003_atvv_request_start(&a));assert(rc003_atvv_on_control(&a,requested,4)&&a.decoder_ready);
    before=pcm;rc003_atvv_on_audio(&a,audio,120);assert(pcm==before+240); /* known profile seed */
    busy=true;assert(!rc003_atvv_request_stop(&a)&&!a.closing);
    assert(a.close_queued);busy=false;rc003_atvv_tick(&a,40);assert(a.closing&&!a.close_queued);
    before=writes;rc003_atvv_tick(&a,50);assert(writes==before); /* no accepted CLOSE replay */
    assert(rc003_atvv_on_control(&a,stop,2));
    busy=true;assert(rc003_atvv_request_start(&a)&&a.open_queued);
    assert(rc003_atvv_request_stop(&a)&&!a.open_pending&&!a.open_queued);
    busy=false;
    puts("ATVV: trusted SYNC, fragmentation, periodic extend, Capture, validation passed");return 0;
}
