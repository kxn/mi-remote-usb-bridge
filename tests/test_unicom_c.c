/* Independent HOGP/ATVV contract driver. No sim_remote.c.
 * Sources: BTstack hids_host procedure ordering, WCH async API semantics,
 * captured RC003 Map and mi-remote-gateway boot-keyboard payload format.
 * Handles below are a synthetic topology, not a claimed complete capture. */
#include <assert.h>
#include <stdio.h>
#include "../firmware/adapters/rc003/rc003_adapter.c"
static unsigned failures,ready_count,key_events,battery,commands,calls;
static unsigned voice_starts,voice_ends,voice_faults,voice_bytes;
static uint8_t recorded[20000];
static uint8_t ended_reason;static uint64_t last_keys;
static uint16_t requested_uuid,requested_handle;static uint8_t last_cmd;
static int submit_status;static bool voice_wanted=true;
static char last_failure[96];
static uint32_t capture_session=1;
void rbp_server_adapter_failed(rbp_server_t*s,const char*r){(void)s;snprintf(last_failure,sizeof last_failure,"%s",r);failures++;}
void rbp_server_set_profile(rbp_server_t*s,const rbp_device_profile_t*p){(void)s;(void)p;}
void rbp_server_on_battery(rbp_server_t*s,uint8_t b,uint8_t c){(void)s;(void)c;battery=b;}
void rbp_server_on_keys(rbp_server_t*s,const rbp_keys_report_t*r){(void)s;last_keys=r->pressed_bits;key_events++;}
void rbp_server_on_link(rbp_server_t*s,rbp_link_state_t st,uint32_t id,uint32_t now){(void)s;(void)id;(void)now;if(st==RBP_LINK_READY)ready_count++;}
void rbp_server_on_voice(rbp_server_t*s,const rbp_voice_evt_t*e,uint32_t now){
    (void)s;(void)now;
    if(e->type==RBP_VOICE_EVT_START){
        voice_starts++;assert(e->u.start.config_len==0 && e->u.start.codec_id==2 && e->u.start.max_unit_bytes==40);
    }
    if(e->type==RBP_VOICE_EVT_ENCODED){
        assert(voice_bytes+e->u.encoded.len<=sizeof recorded);
        memcpy(recorded+voice_bytes,e->u.encoded.data,e->u.encoded.len);
        voice_bytes+=e->u.encoded.len;
    }
    if(e->type==RBP_VOICE_EVT_END){voice_ends++;ended_reason=e->u.end.reason;}
    if(e->type==RBP_VOICE_EVT_FAULT)voice_faults++;
}
void rbp_server_on_voice_state(rbp_server_t*s,uint8_t v,uint8_t i,uint32_t rate){(void)s;(void)v;(void)i;(void)rate;}
bool rbp_server_voice_wanted(const rbp_server_t*s){(void)s;return voice_wanted;}
static int mtu(void*u,uint16_t m){(void)u;assert(m==RBP_ATT_MTU);calls++;return submit_status;}
static int svc(void*u,uint16_t id){(void)u;requested_uuid=id;calls++;return submit_status;}
static int svc128(void*u,const uint8_t*id){return svc(u,id[12]|id[13]<<8);}
static int chr(void*u,uint16_t s,uint16_t e,uint16_t id){(void)u;assert(s<=e);requested_uuid=id;calls++;return submit_status;}
static int chr128(void*u,uint16_t s,uint16_t e,const uint8_t*id){return chr(u,s,e,id[12]|id[13]<<8);}
static int desc(void*u,uint16_t s,uint16_t e){(void)u;assert(s<=e);requested_handle=s;calls++;return submit_status;}
static int readval(void*u,uint16_t h){(void)u;requested_handle=h;calls++;return submit_status;}
static int readlong(void*u,uint16_t h,uint16_t off){assert(off);return readval(u,h);}
static int writeval(void*u,uint16_t h,const uint8_t*d,uint16_t n){(void)u;assert((n==2&&(d[0]==1||d[0]==2)&&d[1]==0)||(n==1&&d[0]<=1));if(n==1){commands++;last_cmd=d[0];}requested_handle=h;calls++;return submit_status;}
static int command(void*u,uint16_t h,const uint8_t*d,uint16_t n){(void)u;(void)n;commands++;requested_handle=h;last_cmd=d[0];return submit_status;}
static const rbp_gatt_client_t ops={NULL,mtu,svc,svc128,chr,chr128,desc,readval,readlong,writeval,command};
static rc003_adapter_t a;
static rc003_adapter_t discovered;
static void event(rbp_gatt_evt_type_t type){rbp_gatt_evt_t e={0};e.type=type;rc003_adapter_on_gatt(&a,&e);}
static void found(uint16_t start,uint16_t end){rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_SERVICE_FOUND;e.svc_start=start;e.svc_end=end;rc003_adapter_on_gatt(&a,&e);}
static void characteristic(uint16_t h,uint8_t props){uint8_t val[]={props,h,h>>8};rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_CHARS_FOUND;e.handle=h-1;e.value=val;e.len=3;rc003_adapter_on_gatt(&a,&e);}
static void descriptor(uint16_t h,uint16_t uuid){rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_DESC_FOUND;e.desc_handle=h;e.uuid16=uuid;rc003_adapter_on_gatt(&a,&e);}
static void value(const uint8_t*d,uint16_t n){rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_READ_RSP;e.value=d;e.len=n;e.proc_complete=true;rc003_adapter_on_gatt(&a,&e);}
static void notify(uint16_t h,const uint8_t*d,uint16_t n){rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_NOTIFY;e.handle=h;e.value=d;e.len=n;rc003_adapter_on_gatt(&a,&e);}

void rbp_server_set_voice_caps(rbp_server_t*s,const rbp_audio_caps_t*c){(void)s;(void)c;}
static const uint8_t hello[]={0x10,1,0x4b,0x2a,3,2,4,0,2,3};
static const uint8_t press[20]={0x82,3,1},release[20]={0x82,3,0};
static void discover(bool early) {
    failures=ready_count=voice_starts=voice_ends=voice_bytes=commands=0;
    submit_status=0;voice_wanted=true;
    rc003_adapter_init(&a,&ops,NULL,NULL);rc003_adapter_start_bound(&a,0,77);
    event(RBP_GATT_EVT_MTU_UPDATED);event(RBP_GATT_EVT_SERVICE_NOT_FOUND);
    found(0x100,0x200);event(RBP_GATT_EVT_PROC_DONE);
    characteristic(0x102,2);event(RBP_GATT_EVT_PROC_DONE);
    uint8_t info[]={0x11,1,0,2};value(info,4);event(RBP_GATT_EVT_PROC_DONE); /* no protocol mode */
    characteristic(0x104,2);event(RBP_GATT_EVT_PROC_DONE);
    uint8_t map[169];FILE *f=fopen("tests/fixtures/unicom/report-map.bin","rb");assert(f);
    assert(fread(map,1,169,f)==169);fclose(f);
    for(unsigned off=0;off<sizeof map;) {
        unsigned n=sizeof map-off;if(n>RBP_ATT_MTU-1)n=RBP_ATT_MTU-1;
        rbp_gatt_evt_t fragment={0};fragment.type=RBP_GATT_EVT_READ_RSP;
        fragment.value=map+off;fragment.len=n;fragment.offset=off;
        fragment.proc_complete=off+n==sizeof map;
        rc003_adapter_on_gatt(&a,&fragment);off+=n;
    }
    assert(a.unicom.selected && requested_uuid==0x2a4d);
    const uint8_t ids[]={1,3,0xfc,0xfb,0xf8,0xfa,0xf9,4};
    for(unsigned i=0;i<8;i++)characteristic(0x110+i*8,(ids[i]==0xfb||ids[i]==0xfa)?0x0e:0x1a);
    event(RBP_GATT_EVT_PROC_DONE);
    for(unsigned i=0;i<8;i++) {
        bool output=ids[i]==0xfb||ids[i]==0xfa;
        unsigned h=0x110+i*8;
        if(!output)descriptor(h+1,0x2902);
        descriptor(h+2,0x2908);descriptor(h+3,0x2803);descriptor(h+4,0x2902);
        event(RBP_GATT_EVT_PROC_DONE);
        uint8_t ref[]={ids[i],output?2:1};value(ref,2);
        if(!output)event(RBP_GATT_EVT_WRITE_DONE);
    }
    assert(requested_uuid==0x180f);event(RBP_GATT_EVT_SERVICE_NOT_FOUND);
    assert(requested_uuid==0xfd00 && a.unicom.fb==0x128);
    found(0x300,0x320);event(RBP_GATT_EVT_PROC_DONE);
    characteristic(0x305,0x14);event(RBP_GATT_EVT_PROC_DONE);
    descriptor(0x306,0x2902);event(RBP_GATT_EVT_PROC_DONE);
    if(early){notify(0x305,hello,10);assert(a.state==ST_UNICOM_SUB && !a.ready);}
    event(RBP_GATT_EVT_WRITE_DONE);
    if(early)assert(a.ready && a.voice_ready && !failures && !commands);
}
static void start_voice(void) {
    notify(a.unicom.f8,press,20);assert(a.unicom.active && !voice_starts);
    rc003_adapter_tick(&a,a.now_ms+1);assert(last_cmd==1 && a.unicom.in_flight);
    event(RBP_GATT_EVT_WRITE_DONE);
}
int main(void) {
    discover(true);discovered=a;
    uint8_t key[]={0,0,0x66,0,0,0,0,0};notify(0x110,key,8);assert(last_keys==1);
    memset(key,0,8);notify(0x110,key,8);assert(!last_keys);
    for(unsigned j=0;j<sizeof(unicom_keys_map)/sizeof(unicom_keys_map[0]);j++) {
        uint8_t data[8]={0};unsigned id=unicom_keys_map[j].report;
        if(id==1)data[2]=unicom_keys_map[j].usage;
        else {data[0]=unicom_keys_map[j].usage;data[1]=unicom_keys_map[j].usage>>8;}
        unsigned h=id==1?0x110:0x118,n=id==1?8:2;
        notify(h,data,n);assert(last_keys==((uint64_t)1<<j));
        memset(data,0,8);notify(h,data,n);assert(!last_keys);
    }
    start_voice();
    FILE *f=fopen("tests/fixtures/unicom/fb_single_01_00.att20.bin","rb");assert(f);
    uint8_t packet[20];unsigned packets=0;
    while(fread(packet,1,20,f)==20){notify(a.unicom.fc,packet,20);assert(!failures);packets++;}
    fclose(f);assert(packets==471 && voice_starts==1 && voice_bytes==157*40);
    f=fopen("build/unicom-adapter-encoded.bin","wb");assert(f);assert(fwrite(recorded,1,voice_bytes,f)==voice_bytes);fclose(f);
    notify(a.unicom.f8,release,20);rc003_adapter_tick(&a,10);assert(last_cmd==0);
    event(RBP_GATT_EVT_WRITE_DONE);rc003_adapter_tick(&a,400);assert(voice_ends==1 && ended_reason==RBP_END_NORMAL);
    assert(rc003_adapter_mic_start(&a)==RBP_STATUS_VOICE_UNAVAILABLE);
    /* Missing hello: keys only, no business writes; late valid notification promotes voice. */
    discover(false);rc003_adapter_tick(&a,2001);assert(a.ready && !a.voice_ready);
    notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,2002);assert(!commands);
    notify(a.unicom.fd,hello,10);assert(a.voice_ready);
    notify(a.unicom.f8,release,20);start_voice();
    /* Accepted write never replayed; stop queues behind it. */
    a=discovered;start_voice();a.unicom.command=1;rc003_adapter_tick(&a,10);
    unsigned old=commands;notify(a.unicom.f8,release,20);rc003_adapter_tick(&a,20);assert(commands==old);
    event(RBP_GATT_EVT_WRITE_DONE);rc003_adapter_tick(&a,21);assert(commands==old+1 && last_cmd==0);
    /* Wrong initialization is incompatible, no FB probe. */
    discover(false);uint8_t bad[10]={0};notify(a.unicom.fd,bad,10);assert(failures==1 && !commands);
    /* Missing fragment and malformed F8 fail closed. */
    a=discovered;failures=voice_bytes=voice_starts=voice_ends=0;start_voice();
    memset(packet,0,20);packet[2]=1;notify(a.unicom.fc,packet,20);assert(failures==1 && voice_ends==1);
    a=discovered;failures=0;notify(a.unicom.f8,press,19);assert(failures==1);
    /* Wrong reference direction and duplicate report ID are not compatible. */
    a=discovered;a.chars[3].report_type=1;assert(!unicom_reports(&a));
    a=discovered;a.chars[7].report_id=0xf8;assert(!unicom_reports(&a));
    /* Stop has bounded tail; partial final group is loss, not normal end. */
    a=discovered;failures=voice_ends=0;start_voice();memset(packet,0,20);
    notify(a.unicom.fc,packet,20);notify(a.unicom.f8,release,20);
    rc003_adapter_tick(&a,10);event(RBP_GATT_EVT_WRITE_DONE);rc003_adapter_tick(&a,400);
    assert(voice_ends==1 && ended_reason==RBP_END_SOURCE_DATA_LOST);
    /* A 5-second refresh preserves the current source/decoder epoch. */
    a=discovered;failures=voice_starts=voice_ends=voice_bytes=0;commands=0;start_voice();
    a.unicom.started=true;a.unicom.last_audio_ms=5001;
    rc003_adapter_tick(&a,5001);assert(commands==2 && last_cmd==1 && !voice_starts && !voice_ends);
    event(RBP_GATT_EVT_WRITE_DONE);
    /* Duplicate hello cannot publish another link-ready during a stream. */
    unsigned ready_before=ready_count;notify(a.unicom.fd,hello,10);assert(ready_count==ready_before && a.unicom.active);
    /* Wrapping group sequence is valid, while a stale sequence is rejected. */
    a.unicom.previous_valid=true;a.unicom.previous=65535;
    memset(packet,0,20);notify(a.unicom.fc,packet,20);assert(!failures && a.unicom.part==1);
    packet[2]=1;notify(a.unicom.fc,packet,20);packet[2]=2;notify(a.unicom.fc,packet,20);
    assert(!failures && a.unicom.previous==0 && !a.unicom.part);
    packet[2]=0;notify(a.unicom.fc,packet,20);assert(failures==1);
    /* Same-bond restore reuses verified handles, never stream or held state. */
    for(unsigned iteration=0;iteration<4;iteration++) {
        a=discovered;failures=commands=voice_starts=voice_ends=0;
        a.unicom.active=a.unicom.down=true;a.unicom.part=2;a.unicom.in_flight=1;
        rc003_adapter_detach(&a);
        assert(a.cache_valid && a.unicom.selected && !a.unicom.hello && !a.unicom.active &&
               !a.unicom.down && !a.unicom.pending_voice && !a.unicom.part && !a.unicom.in_flight);
        unsigned baseline=calls;
        rc003_adapter_start_bound(&a,100,77);assert(a.restoring);
        /* Notification may precede even the MTU response. */
        notify(a.unicom.f8,press,20);assert(a.unicom.pending_voice && !commands);
        if(iteration==1)notify(a.unicom.f8,release,20);
        event(RBP_GATT_EVT_MTU_UPDATED);assert(a.state==ST_UNICOM_SUB);
        assert(calls-baseline==1); /* Default MTU 23: only FD02 CCCD. */
        notify(a.unicom.fd,hello,10);assert(!a.ready);
        event(RBP_GATT_EVT_WRITE_DONE);assert(a.ready && a.voice_ready && a.cache_valid);
        if(iteration==2)voice_wanted=false;
        rc003_adapter_tick(&a,101);
        if(iteration==1)assert(!commands && !a.unicom.active);
        else assert(commands==1 && a.unicom.active && a.unicom.in_flight);
        if(iteration==2){voice_wanted=true;rc003_adapter_tick(&a,102);assert(commands==1 && a.unicom.active);}
        if(iteration!=1) {
            event(RBP_GATT_EVT_WRITE_DONE);
            FILE *audio=fopen("tests/fixtures/unicom/fb_single_01_00.att20.bin","rb");assert(audio);
            voice_bytes=0;
            for(unsigned i=0;i<3;i++){assert(fread(packet,1,20,audio)==20);notify(a.unicom.fc,packet,20);}
            fclose(audio);assert(voice_starts==1 && voice_bytes==40 && !failures);
        }
    }
    /* Optional database-change subscription and protocol mode retain ordering. */
    a=discovered;commands=failures=0;
    a.service_changed_cccd=0x98;a.protocol_mode_handle=0x97;
    rc003_adapter_detach(&a);rc003_adapter_start_bound(&a,150,77);
    notify(a.unicom.f8,press,20);
    event(RBP_GATT_EVT_MTU_UPDATED);assert(a.state==ST_SUB_CHANGED);
    event(RBP_GATT_EVT_WRITE_DONE);assert(a.state==ST_READ_PROTOCOL_MODE);
    uint8_t mode=1;value(&mode,1);assert(a.state==ST_UNICOM_SUB);
    event(RBP_GATT_EVT_WRITE_DONE);notify(a.unicom.fd,hello,10);
    rc003_adapter_tick(&a,151);assert(a.ready && commands==1 && !failures);
    /* Power-cycle serialization restores metadata only, then follows the same
     * encrypted-link path. Corrupt length/profile/handles are rejected. */
    uint8_t saved[384];a=discovered;
    size_t saved_len=rc003_adapter_cache_export(&a,saved,sizeof saved);assert(saved_len>0);
    rc003_adapter_init(&a,&ops,NULL,NULL);
    assert(rc003_adapter_cache_import(&a,77,saved,saved_len));
    assert(a.cache_valid && !a.ready && !a.unicom.hello && !a.unicom.down);
    commands=failures=0;unsigned req_before=calls;
    rc003_adapter_start_bound(&a,160,77);notify(a.unicom.f8,press,20);
    event(RBP_GATT_EVT_MTU_UPDATED);event(RBP_GATT_EVT_WRITE_DONE);notify(a.unicom.fd,hello,10);
    rc003_adapter_tick(&a,161);assert(a.ready && commands==1 && calls-req_before==2 && !failures);
    rc003_adapter_init(&a,&ops,NULL,NULL);
    assert(!rc003_adapter_cache_import(&a,77,saved,saved_len-1));
    saved[48]=saved[49]=0;assert(!rc003_adapter_cache_import(&a,77,saved,saved_len));
    assert(!a.ready && !a.cache_valid);
    /* READY precedes host enable: capture now, deliver only after enable. */
    a=discovered;voice_wanted=false;commands=failures=voice_starts=voice_bytes=voice_ends=0;
    notify(a.unicom.f8,press,20);assert(a.unicom.active && !a.unicom.announced);
    rc003_adapter_tick(&a,1);assert(commands==1);event(RBP_GATT_EVT_WRITE_DONE);
    FILE *pre=fopen("tests/fixtures/unicom/fb_single_01_00.att20.bin","rb");assert(pre);
    uint8_t expected[320];
    for(unsigned unit=0;unit<8;unit++) {
        for(unsigned part=0;part<3;part++) {
            assert(fread(packet,1,20,pre)==20);
            if(part<2)memcpy(expected+unit*40+part*16,packet+4,16);
            else memcpy(expected+unit*40+32,packet+4,8);
            notify(a.unicom.fc,packet,20);
        }
    }
    assert(a.unicom.pre_count==8 && !voice_bytes && !voice_starts && !failures);
    voice_wanted=true;
    for(unsigned t=0;t<8;t++)rc003_adapter_tick(&a,160+t*5);
    assert(voice_starts==1 && voice_bytes==320 && !memcmp(expected,recorded,320) && !a.unicom.pre_count);
    for(unsigned part=0;part<3;part++){assert(fread(packet,1,20,pre)==20);notify(a.unicom.fc,packet,20);}
    fclose(pre);assert(voice_bytes==360 && voice_starts==1 && !failures);
    /* Buffered prefix and concurrent live units remain FIFO across ring wrap.
     * Release drains already admitted units; a later stream cannot inherit any. */
    a=discovered;voice_wanted=false;commands=failures=voice_starts=voice_bytes=voice_ends=0;
    notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,1);event(RBP_GATT_EVT_WRITE_DONE);
    uint8_t fifo_expected[24*40];
    pre=fopen("tests/fixtures/unicom/fb_single_01_00.att20.bin","rb");assert(pre);
    for(unsigned unit=0;unit<24;unit++) {
        if(unit==4)voice_wanted=true;
        if(unit>=4)rc003_adapter_tick(&a,unit*5);
        for(unsigned part=0;part<3;part++) {
            assert(fread(packet,1,20,pre)==20);
            memcpy(fifo_expected+unit*40+part*16,packet+4,part==2?8:16);
            notify(a.unicom.fc,packet,20);
        }
        assert(!failures && a.unicom.pre_count== (unit<4?unit+1:4));
    }
    notify(a.unicom.f8,release,20);rc003_adapter_tick(&a,125);
    assert(last_cmd==0);event(RBP_GATT_EVT_WRITE_DONE);
    for(unsigned t=130;t<=150;t+=5)rc003_adapter_tick(&a,t);
    assert(voice_starts==1 && voice_bytes==sizeof fifo_expected && !a.unicom.pre_count);
    assert(!memcmp(recorded,fifo_expected,sizeof fifo_expected));
    rc003_adapter_tick(&a,450);assert(voice_ends==1 && ended_reason==RBP_END_NORMAL);
    /* Continue the fixture with distinct audio in the next physical capture. */
    voice_starts=voice_bytes=0;start_voice();
    uint8_t next_expected[40];
    for(unsigned part=0;part<3;part++) {
        assert(fread(packet,1,20,pre)==20);
        memcpy(next_expected+part*16,packet+4,part==2?8:16);
        notify(a.unicom.fc,packet,20);
    }
    fclose(pre);
    assert(!failures && voice_starts==1 && voice_bytes==40 && !memcmp(recorded,next_expected,40));
    /* Release before admission cancels captured bytes, not a ghost stream. */
    a=discovered;voice_wanted=false;commands=voice_starts=voice_bytes=0;
    notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,1);event(RBP_GATT_EVT_WRITE_DONE);
    notify(a.unicom.f8,release,20);voice_wanted=true;rc003_adapter_tick(&a,20);
    assert(a.unicom.closing && !a.unicom.announced && !voice_starts && !voice_bytes);
    /* No negotiation: bounded timeout stops, does not silently restart. */
    a=discovered;voice_wanted=false;commands=0;
    notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,1);event(RBP_GATT_EVT_WRITE_DONE);
    rc003_adapter_tick(&a,251);assert(a.unicom.closing && last_cmd==0 && !a.unicom.announced);
    /* Explicit disable is different from initial negotiation. */
    a=discovered;voice_wanted=true;rc003_adapter_tick(&a,0);voice_wanted=false;capture_session=0;
    notify(a.unicom.f8,press,20);assert(!a.unicom.pending_voice && !a.unicom.active);
    voice_wanted=true;
    capture_session=1;
    /* A USB session replacement or explicit refusal discards all preroll. */
    for(unsigned session=0;session<3;session+=2) {
        a=discovered;voice_wanted=false;commands=voice_starts=voice_bytes=voice_ends=0;capture_session=1;
        notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,1);event(RBP_GATT_EVT_WRITE_DONE);
        pre=fopen("tests/fixtures/unicom/fb_single_01_00.att20.bin","rb");assert(pre);
        for(unsigned j=0;j<3;j++){assert(fread(packet,1,20,pre)==20);notify(a.unicom.fc,packet,20);}fclose(pre);
        assert(a.unicom.pre_count==1);
        capture_session=session;voice_wanted=true;rc003_adapter_tick(&a,20);
        assert(a.unicom.closing && !a.unicom.pre_count && !voice_starts && !voice_bytes && !voice_ends);
    }
    capture_session=0;a=discovered;voice_wanted=false;commands=0;
    notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,1);assert(!commands && !a.unicom.active);
    capture_session=1;
    /* More than 160ms before negotiation: bounded drop/stop, no prefix clipping. */
    a=discovered;voice_wanted=false;commands=voice_starts=voice_bytes=0;
    notify(a.unicom.f8,press,20);rc003_adapter_tick(&a,1);event(RBP_GATT_EVT_WRITE_DONE);
    pre=fopen("tests/fixtures/unicom/fb_single_01_00.att20.bin","rb");assert(pre);
    for(unsigned j=0;j<27;j++){assert(fread(packet,1,20,pre)==20);notify(a.unicom.fc,packet,20);}fclose(pre);
    assert(a.unicom.closing && !a.unicom.pre_count && !voice_bytes && !voice_starts);
    voice_wanted=true;rc003_adapter_tick(&a,20);assert(!voice_bytes && !voice_starts);
    /* Commit after initial pairing makes its discovered database reusable. */
    a=discovered;a.cache_peer_id=0;a.cache_valid=false;
    rc003_adapter_commit_bond(&a,88);assert(a.cache_valid && a.cache_peer_id==88);
    rc003_adapter_detach(&a);rc003_adapter_start_bound(&a,200,89);
    assert(!a.unicom.selected && !a.cache_valid && !a.restoring);
    /* Cache validity requires a safe database and is lost on Service Changed. */
    a=discovered;a.cache_safe=false;unicom_ready(&a);assert(!a.cache_valid);
    a=discovered;a.service_changed_handle=0x99;rc003_adapter_detach(&a);
    uint8_t change[]={1,0,0xff,0xff};notify(0x99,change,4);assert(!a.cache_valid);
    a=discovered;rc003_adapter_detach(&a);rc003_adapter_start_bound(&a,300,77);
    a.service_changed_handle=0x99;notify(0x99,change,4);assert(!a.cache_valid && a.state==ST_FAILED);
    /* A lost media fragment invalidates this stream, not the database. */
    a=discovered;failures=voice_starts=0;start_voice();memset(packet,0,20);packet[2]=1;
    notify(a.unicom.fc,packet,20);assert(failures==1 && a.cache_valid);
    assert(strstr(last_failure,"Unicom FC continuity") && strlen(last_failure)<48);
    rbp_fault_t fc_faults[RBP_FAULT_CAPACITY];uint32_t fault_seq,fault_evicted;
    uint8_t fc_count=rbp_fault_snapshot(fc_faults,&fault_seq,&fault_evicted);
    bool found_fc=false;
    for(unsigned i=0;i<fc_count;i++)if(fc_faults[i].domain==RBP_FAULT_ADAPTER && fc_faults[i].stage==0x91) {
        assert(fc_faults[i].code==0x00010000u && fc_faults[i].context==0x0000ffffu);
        found_fc=true;
    }
    assert(found_fc);
    /* A different peer cannot reuse retained Unicom metadata. */
    a=discovered;rc003_adapter_detach(&a);rc003_adapter_start_bound(&a,400,78);
    assert(!a.unicom.selected && !a.unicom.hello && !a.unicom.active);
    /* A valid unrelated HID map is rejected despite broad HIDS scan admission. */
    const uint8_t generic[]={5,1,9,6,0xa1,1,0x85,1,5,7,0x19,0,0x29,0xff,
        0x15,0,0x26,0xff,0,0x75,8,0x95,6,0x81,0,0xc0};
    a.state=ST_READ_MAP;a.map_len=0;failures=0;commands=0;
    value(generic,sizeof generic);assert(failures==1 && a.state==ST_FAILED && !commands);
    /* Unlike legacy RC003, the measured Unicom topology must be exact. */
    a=discovered;a.state=ST_ENUM_REPORT_CHARS;failures=0;
    characteristic(0x999,0x12);assert(failures==1&&a.state==ST_FAILED);
    assert(strstr(last_failure,"T=0"));
    a.state=ST_ENUM_REPORT_CHARS;fail_transport(&a,"timeout total");
    assert(strstr(last_failure,"T=1"));
    puts("unicom adapter: discovery, early/late/missing hello, captured FC, keys, stop and negative cases OK");
    return 0;
}

uint32_t rbp_server_voice_capture_session(const rbp_server_t *s){(void)s;return capture_session;}
