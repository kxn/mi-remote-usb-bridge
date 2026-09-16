/* Independent HOGP/ATVV contract driver. No sim_remote.c.
 * Sources: BTstack hids_host procedure ordering, WCH async API semantics,
 * captured RC003 Map and mi-remote-gateway boot-keyboard payload format.
 * Handles below are a synthetic topology, not a claimed complete capture. */
#include <assert.h>
#include <stdio.h>
#include "../firmware/adapters/rc003/rc003_adapter.c"
static unsigned failures,ready_count,key_events,battery,commands,calls;
static unsigned voice_starts,voice_ends,voice_faults,voice_bytes;
static uint8_t recorded[512],seed[4];
static uint16_t requested_uuid,requested_handle;static uint8_t last_cmd;
static int submit_status;static bool voice_wanted=true;
void rbp_server_adapter_failed(rbp_server_t*s,const char*r){(void)s;(void)r;failures++;}
void rbp_server_set_profile(rbp_server_t*s,const rbp_device_profile_t*p){(void)s;(void)p;}
void rbp_server_on_battery(rbp_server_t*s,uint8_t b,uint8_t c){(void)s;(void)c;battery=b;}
void rbp_server_on_keys(rbp_server_t*s,const rbp_keys_report_t*r){(void)s;(void)r;key_events++;}
void rbp_server_on_link(rbp_server_t*s,rbp_link_state_t st,uint32_t id,uint32_t now){(void)s;(void)id;(void)now;if(st==RBP_LINK_READY)ready_count++;}
void rbp_server_on_voice(rbp_server_t*s,const rbp_voice_evt_t*e,uint32_t now){
    (void)s;(void)now;
    if(e->type==RBP_VOICE_EVT_START){
        voice_starts++;assert(e->u.start.config_len==4);
        memcpy(seed,e->u.start.config,4);
    }
    if(e->type==RBP_VOICE_EVT_ENCODED){
        assert(voice_bytes+e->u.encoded.len<=sizeof recorded);
        memcpy(recorded+voice_bytes,e->u.encoded.data,e->u.encoded.len);
        voice_bytes+=e->u.encoded.len;
    }
    if(e->type==RBP_VOICE_EVT_END)voice_ends++;
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
static int writeval(void*u,uint16_t h,const uint8_t*d,uint16_t n){(void)u;assert(n==2&&(d[0]==1||d[0]==2)&&d[1]==0);requested_handle=h;calls++;return submit_status;}
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
int main(void){
    rc003_adapter_init(&a,&ops,NULL,NULL);submit_status=RBP_GATT_RETRY;
    rc003_adapter_start_bound(&a,0,7);assert(a.retry&&calls==1);
    rc003_adapter_tick(&a,10);assert(a.retry&&calls==2);
    submit_status=0;rc003_adapter_tick(&a,20);assert(!a.retry&&calls==3);
    rc003_adapter_tick(&a,30);assert(calls==3); /* no replay of accepted op */
    event(RBP_GATT_EVT_MTU_UPDATED);assert(requested_uuid==0x1801);
    event(RBP_GATT_EVT_SERVICE_NOT_FOUND);assert(requested_uuid==0x1812);
    found(0x55,0xb4);event(RBP_GATT_EVT_PROC_DONE);assert(requested_uuid==0x2a4a);
    characteristic(0xa0,2);event(RBP_GATT_EVT_PROC_DONE);assert(requested_handle==0xa0);
    uint8_t info[]={0x11,0x01,0,2};value(info,sizeof info);
    assert(a.hid_info_valid&&a.hid_flags==2&&requested_uuid==0x2a4e);
    characteristic(0x80,6);event(RBP_GATT_EVT_PROC_DONE);
    assert(commands==0&&requested_handle==0x80&&a.state==ST_READ_PROTOCOL_MODE);
    uint8_t mode=0;value(&mode,1);
    assert(commands==1&&last_cmd==1&&a.state==ST_VERIFY_PROTOCOL_MODE);
    mode=1;value(&mode,1);assert(requested_uuid==0x2a4b);
    characteristic(0x5d,2);event(RBP_GATT_EVT_PROC_DONE);assert(requested_handle==0x5d);
    FILE*f=fopen("tests/fixtures/rc003-hardware-report-map.hex","r");assert(f);
    uint8_t map[86];unsigned b;for(unsigned i=0;i<86;i++){assert(fscanf(f,"%2x",&b)==1);map[i]=b;}fclose(f);
    value(map,86);assert(a.rc003_boot_layout&&requested_uuid==0x2a4d);
    /* Hardware enumerates more reports than the bounded runtime cache.
     * Preserve RC003's existing first-eight behavior, including proceeding
     * to descriptor discovery after surplus characteristics. */
    rc003_adapter_t before_reports=a;
    for(unsigned i=0;i<RC003_MAX_REPORT_CHARS+2;i++)characteristic(0x57+4*i,0x12);
    assert(!failures&&a.state==ST_ENUM_REPORT_CHARS&&a.char_count==RC003_MAX_REPORT_CHARS);
    event(RBP_GATT_EVT_PROC_DONE);
    assert(!failures&&a.state==ST_DISC_DESCS&&requested_handle==0x58);
    a=before_reports;
    characteristic(0x57,0x12);characteristic(0x63,0x12);event(RBP_GATT_EVT_PROC_DONE);
    descriptor(0x58,0x2902);descriptor(0x59,0x2908);
    descriptor(0x5c,0x2803);descriptor(0x62,0x2902); /* belongs to a different char */
    event(RBP_GATT_EVT_PROC_DONE);assert(requested_handle==0x59);
    uint8_t ref[]={1,1};value(ref,2);assert(requested_handle==0x58);
    event(RBP_GATT_EVT_WRITE_DONE);
    descriptor(0x64,0x2902);descriptor(0x65,0x2908);event(RBP_GATT_EVT_PROC_DONE);
    ref[0]=3;value(ref,2);assert(!a.chars[1].subscribed&&requested_uuid==0x180f);
    uint8_t boot[]={0,0,0x52,0x4a,0,0,0,0};
    notify(0x63,boot,8);assert(!failures&&!key_events); /* ghost while discovering */
    notify(0x57,boot,8);assert(!failures&&key_events==1);
    notify(0x57,boot,7);assert(!failures&&key_events==2); /* release, no teardown */
    found(0x2e,0x34);event(RBP_GATT_EVT_PROC_DONE);characteristic(0x30,2);event(RBP_GATT_EVT_PROC_DONE);
    event(RBP_GATT_EVT_PROC_DONE);assert(requested_handle==0x30); /* read-only battery */
    uint8_t level=97;value(&level,1);assert(battery==97&&requested_uuid==1);
    event(RBP_GATT_EVT_SERVICE_NOT_FOUND);assert(a.ready&&ready_count==1&&!failures);
    /* No CAPS reply is a voice failure, not a key-link failure. */
    a.state=ST_ATVV_PREHANDSHAKE;a.deadline_ms=100;a.retry=0;
    rc003_adapter_tick(&a,100);assert(a.ready&&!failures&&ready_count==2);
    /* ATVV contract: TX discovery, CTL CCCD first, AUDIO CCCD second,
     * then GET_CAPS; no characteristic-handle arithmetic for CCCDs. */
    a.ready=false;start_op(&a,ST_FIND_ATVV_SVC,100);begin_next(&a,100);
    found(0x300,0x308);event(RBP_GATT_EVT_PROC_DONE);assert(requested_uuid==2);
    characteristic(0x302,0x0c);event(RBP_GATT_EVT_PROC_DONE);assert(requested_uuid==4);
    characteristic(0x307,0x10);event(RBP_GATT_EVT_PROC_DONE);
    descriptor(0x308,0x2902);event(RBP_GATT_EVT_PROC_DONE);assert(requested_handle==0x308);
    event(RBP_GATT_EVT_WRITE_DONE);assert(requested_uuid==3);
    /* Synthetic standard On-request CAPS, not a captured RC003 response. */
    uint8_t caps[]={11,1,0,3,0,0,120,0,0};notify(0x307,caps,7);
    assert(!a.atvv.caps_valid&&!a.voice_ready); /* truncated wire data rejected */
    notify(0x307,caps,sizeof caps);
    assert(a.atvv.caps_valid&&!a.voice_ready); /* unsolicited CAPS before AUDIO CCCD */
    characteristic(0x304,0x10);event(RBP_GATT_EVT_PROC_DONE);
    descriptor(0x305,0x2902);event(RBP_GATT_EVT_PROC_DONE);assert(requested_handle==0x305);
    event(RBP_GATT_EVT_WRITE_DONE);assert(requested_handle==0x302&&last_cmd==0x0a);
    notify(0x307,caps,sizeof caps);
    assert(a.ready&&a.voice_ready&&ready_count==3&&!failures);
    assert(a.cache_safe&&a.cache_valid&&a.cache_peer_id==7);
    discovered=a;
    caps[1]=0xff;notify(0x307,caps,sizeof caps);assert(a.ready&&!failures); /* no runtime link reset */
    uint8_t voice_boot[]={0,0,0x3e,0,0,0,0,0},released[8]={0};
    unsigned sent_commands=commands;notify(0x57,voice_boot,8);
    assert(commands==sent_commands+1&&last_cmd==12);notify(0x57,released,8);assert(a.atvv.closing);
    /* Late notifications after detach must not repopulate a new session. */
    rc003_adapter_detach(&a);notify(0x57,boot,8);assert(!a.ready&&!failures);
    /* Report mode already selected: no write. A rejected mode switch is
     * detected by readback, never hidden by ignoring an unrelated error. */
    unsigned old_commands=commands;
    a.protocol_mode_handle=0x5b;a.svc_start=0x55;a.svc_end=0xb4;
    start_op(&a,ST_READ_PROTOCOL_MODE,100);mode=1;value(&mode,1);
    assert(commands==old_commands&&a.state==ST_FIND_MAP_CHAR);
    start_op(&a,ST_READ_PROTOCOL_MODE,100);mode=0;value(&mode,1);
    assert(commands==old_commands+1&&a.state==ST_VERIFY_PROTOCOL_MODE);
    value(&mode,1);assert(failures==1&&a.state==ST_FAILED);
    /* Busy cannot postpone the deadline forever. */
    submit_status=RBP_GATT_RETRY;rc003_adapter_start(&a,1000);
    rc003_adapter_tick(&a,3000);assert(failures==2);
    /* Bearer failure must bypass optional feature fallback in every phase. */
    const uint8_t phases[]={ST_MTU,ST_FIND_GATT_SVC,ST_READ_BATTERY,ST_FIND_ATVV_SVC,ST_ATVV_PREHANDSHAKE,ST_DONE};
    for(unsigned i=0;i<sizeof phases;i++) {
        a.state=phases[i];unsigned failed_before=failures,ready_before=ready_count;
        rbp_gatt_evt_t broken={0};broken.type=RBP_GATT_EVT_BEARER_FAILED;broken.status=13;
        rc003_adapter_on_gatt(&a,&broken);
        assert(a.state==ST_FAILED&&failures==failed_before+1&&ready_count==ready_before);
    }
    for(unsigned flags=0;flags<4;flags++) {
        rc003_adapter_init(&a,&ops,NULL,NULL);a.svc_start=0x55;a.svc_end=0xb4;
        start_op(&a,ST_READ_HID_INFO,0);info[3]=flags;value(info,sizeof info);
        assert(a.hid_info_valid&&a.hid_flags==flags&&a.state==ST_FIND_PROTOCOL_MODE);
    }
    start_op(&a,ST_READ_HID_INFO,0);info[3]=4;unsigned failed_before=failures;
    value(info,sizeof info);assert(failures==failed_before+1);
    start_op(&a,ST_READ_HID_INFO,0);value(info,3);assert(failures==failed_before+2);
    /* Bond-scoped cache: no discovery, stream reset, early real search,
     * cancellation, changed database, and failed ATT ownership. */
    submit_status=0;
    for(unsigned scenario=0;scenario<8;scenario++) {
        a=discovered;
        a.atvv.stream_active=true;a.pressed_bits=1;a.keys_by_report[0]=1;
        rc003_adapter_detach(&a);
        assert(!a.atvv.stream_active&&!a.pressed_bits&&!a.keys_by_report[0]&&!a.ready);
        if(scenario==7){a.service_changed_handle=0x22;a.service_changed_cccd=0x23;}
        if(scenario==1) {
            a.service_changed_handle=0x22;
            uint8_t changed[]={1,0,255,255};notify(0x22,changed,4);
            assert(!a.cache_valid);
        }
        rc003_adapter_start_bound(&a,200,scenario==2?8:7);
        event(RBP_GATT_EVT_MTU_UPDATED);
        if(scenario==7){assert(requested_handle==0x23);event(RBP_GATT_EVT_WRITE_DONE);}
        if(scenario==1 || scenario==2) {assert(!a.restoring&&requested_uuid==0x1801);continue;}
        assert(a.restoring && requested_handle==a.protocol_mode_handle);
        if(scenario==3) {
            rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_PROC_ERROR;e.status=0x12;
            rc003_adapter_on_gatt(&a,&e);assert(!a.cache_valid&&a.state==ST_FAILED);continue;
        }
        uint8_t mode=1;value(&mode,1);assert(requested_handle==a.bat_value_handle);
        uint8_t fresh_battery=96;value(&fresh_battery,1);assert(battery==96&&requested_handle==a.atvv_ctl_cccd);
        unsigned before=commands;
        uint8_t search=8;notify(a.atvv_ctl_handle,&search,1);
        assert(a.pending_search&&commands==before);
        if(scenario==4) {uint8_t stop[]={0,0};notify(a.atvv_ctl_handle,stop,2);assert(!a.pending_search);}
        if(scenario==5) {
            a.service_changed_handle=0x22;uint8_t changed[]={1,0,255,255};notify(0x22,changed,4);
            assert(a.state==ST_FAILED&&!a.cache_valid&&!a.pending_search);continue;
        }
        if(scenario==6) {
            rbp_gatt_evt_t e={0};e.type=RBP_GATT_EVT_BEARER_FAILED;e.status=0x16;
            rc003_adapter_on_gatt(&a,&e);assert(a.state==ST_FAILED&&a.cache_valid&&!a.pending_search);continue;
        }
        event(RBP_GATT_EVT_WRITE_DONE);assert(requested_handle==a.atvv_audio_cccd);
        event(RBP_GATT_EVT_WRITE_DONE);assert(last_cmd==0x0a);
        uint8_t reply[]={11,1,0,3,0,0,120,0,0};
        notify(a.atvv_ctl_handle,reply,sizeof reply);
        assert(a.ready&&a.voice_ready&&!a.restoring&&a.cache_valid);
        assert(a.atvv.open_pending==(scenario!=4));
    }
    /* Captured failure: a slow MTU response at 2s must not kill the link.
     * Check accepted requests are never replayed, and transport failure keeps
     * the real discovered cache across repeated detach/reconnect attempts. */
    a=discovered;rc003_adapter_detach(&a);submit_status=0;
    rc003_adapter_start_bound(&a,1000,7);
    unsigned accepted_calls=calls,old_failures=failures;
    rc003_adapter_tick(&a,3000);
    assert(a.state==ST_MTU&&a.restoring&&calls==accepted_calls&&failures==old_failures);
    rc003_adapter_tick(&a,6000);event(RBP_GATT_EVT_MTU_UPDATED);
    assert(a.restoring&&a.state==ST_READ_PROTOCOL_MODE);
    rc003_adapter_tick(&a,36000);
    assert(a.state==ST_FAILED&&a.cache_valid&&failures==old_failures+1);
    rc003_adapter_detach(&a);rc003_adapter_detach(&a);
    rc003_adapter_start_bound(&a,40000,7);assert(a.restoring);
    rbp_gatt_evt_t lost={0};lost.type=RBP_GATT_EVT_BEARER_FAILED;lost.status=0x16;
    rc003_adapter_on_gatt(&a,&lost);assert(a.cache_valid&&a.state==ST_FAILED);
    rc003_adapter_detach(&a);rc003_adapter_start_bound(&a,41000,7);
    lost.status=0x12;rc003_adapter_on_gatt(&a,&lost);
    assert(!a.cache_valid&&a.state==ST_FAILED);
    /* Still bound local queue starvation, including clock wrap. */
    a=discovered;rc003_adapter_detach(&a);submit_status=RBP_GATT_RETRY;
    rc003_adapter_start_bound(&a,0xfffffc00u,7);
    rc003_adapter_tick(&a,0xfffffc00u+1999u);assert(a.state==ST_MTU);
    rc003_adapter_tick(&a,0xfffffc00u+2000u);assert(a.state==ST_FAILED&&a.cache_valid);
    submit_status=0;
    a=discovered;rc003_adapter_detach(&a);
    rc003_adapter_start_bound(&a,0xfffffc00u,7);
    accepted_calls=calls;
    rc003_adapter_tick(&a,0xfffffc00u+29999u);
    assert(a.state==ST_MTU&&calls==accepted_calls);
    rc003_adapter_tick(&a,0xfffffc00u+30000u);
    assert(a.state==ST_FAILED&&a.cache_valid);
    /* Progress can renew a transaction budget, but not the total attempt. */
    a=discovered;rc003_adapter_detach(&a);
    rc003_adapter_start_bound(&a,1000,7);
    rc003_adapter_tick(&a,30000);event(RBP_GATT_EVT_MTU_UPDATED);
    rc003_adapter_tick(&a,59000);mode=1;value(&mode,1);
    assert(a.state==ST_READ_BATTERY);
    rc003_adapter_tick(&a,61000);assert(a.state==ST_FAILED&&a.cache_valid);
    /* Early CAPS cannot complete a locally queued GET_CAPS. The reply
     * budget starts on submission, not while the local stack is busy. */
    a=discovered;rc003_adapter_detach(&a);
    rc003_adapter_start_bound(&a,1000,7);event(RBP_GATT_EVT_MTU_UPDATED);
    mode=1;value(&mode,1);value(&level,1);
    assert(requested_handle==a.atvv_ctl_cccd);
    event(RBP_GATT_EVT_WRITE_DONE);assert(requested_handle==a.atvv_audio_cccd);
    submit_status=RBP_GATT_RETRY;event(RBP_GATT_EVT_WRITE_DONE);
    uint8_t good_caps[]={11,1,0,3,0,0,120,0,0};
    notify(a.atvv_ctl_handle,good_caps,sizeof good_caps);
    assert(!a.ready&&!a.caps_requested&&a.retry);
    submit_status=0;rc003_adapter_tick(&a,2900);
    assert(a.ready&&a.caps_requested);

    /* CAPS/SEARCH can arrive in any cached restore phase. They cannot
     * bypass subscription completion or GET_CAPS submission. */
    for(unsigned phase=0;phase<5;phase++) {
        a=discovered;rc003_adapter_detach(&a);
        rc003_adapter_start_bound(&a,1000,7);
        for(unsigned step=0;step<5;step++) {
            if(step==phase) {
                unsigned before=commands;
                notify(a.atvv_ctl_handle,good_caps,sizeof good_caps);
                uint8_t search=8;notify(a.atvv_ctl_handle,&search,1);
                assert(!a.ready&&commands==before&&a.pending_search);
            }
            if(step==0)event(RBP_GATT_EVT_MTU_UPDATED);
            else if(step==1){mode=1;value(&mode,1);}
            else if(step==2)value(&level,1);
            else event(RBP_GATT_EVT_WRITE_DONE);
            if(step<4)assert(!a.ready);
        }
        assert(a.ready&&a.caps_requested&&a.atvv.open_pending);
    }
    /* A physical On-request voice press before CAPS must start after READY;
     * a release (including malformed report release) must cancel it. */
    for(unsigned release=0;release<3;release++) {
        a=discovered;rc003_adapter_detach(&a);
        rc003_adapter_start_bound(&a,1000,7);
        notify(0x57,voice_boot,8);assert(a.voice_down_seen);
        if(release==1)notify(0x57,released,8);
        if(release==2)notify(0x57,released,7);
        event(RBP_GATT_EVT_MTU_UPDATED);mode=1;value(&mode,1);value(&level,1);
        event(RBP_GATT_EVT_WRITE_DONE);event(RBP_GATT_EVT_WRITE_DONE);
        notify(a.atvv_ctl_handle,good_caps,sizeof good_caps);
        assert(a.ready&&a.atvv.open_pending==(release==0));
    }
    /* MIC_OPEN deadline closes the uncertain bearer, preserving cache.
     * A late START must not reanimate the failed attempt. */
    a=discovered;a.atvv.now_ms=1000;a.now_ms=1000;
    assert(rc003_atvv_request_start(&a.atvv));
    old_failures=failures;rc003_adapter_tick(&a,2002);
    assert(a.atvv.open_pending&&failures==old_failures);
    rc003_adapter_tick(&a,1000+ATVV_OPEN_WAIT_MS);
    assert(a.state==ST_FAILED&&a.cache_valid&&failures==old_failures+1);
    uint8_t late_start[]={4,0,2,0};notify(a.atvv_ctl_handle,late_start,4);
    assert(!a.atvv.stream_active);
    /* RC003 HTT: wake key may be the only start intent received. Cover
     * both pre-READY and runtime presses, no duplicate OPEN, and release. */
    for(unsigned early=0;early<2;early++) {
        a=discovered;rc003_adapter_detach(&a);
        rc003_adapter_start_bound(&a,1000,7);
        if(early)notify(0x57,voice_boot,8);
        event(RBP_GATT_EVT_MTU_UPDATED);mode=1;value(&mode,1);value(&level,1);
        event(RBP_GATT_EVT_WRITE_DONE);event(RBP_GATT_EVT_WRITE_DONE);
        uint8_t htt_caps[]={11,1,0,3,3,0,120,0,0};
        notify(a.atvv_ctl_handle,htt_caps,sizeof htt_caps);
        assert(a.ready&&a.atvv.caps.interaction==3);
        if(!early)notify(0x57,voice_boot,8);
        assert(a.atvv.open_pending);
        unsigned before=commands;uint8_t search=8;
        notify(a.atvv_ctl_handle,&search,1);notify(0x57,voice_boot,8);
        assert(commands==before); /* same held press cannot restart audio */
        notify(0x57,released,8);assert(a.atvv.closing);
        uint8_t late[]={4,0,2,0};notify(a.atvv_ctl_handle,late,4);
        assert(a.atvv.stream_active&&a.atvv.closing&&commands==before+1);
        uint8_t stop[]={0,0};notify(a.atvv_ctl_handle,stop,2);
        assert(!a.atvv.stream_active);
    }
    for(unsigned interaction=1;interaction<=3;interaction+=2) {
        a=discovered;rc003_adapter_detach(&a);
        rc003_adapter_start_bound(&a,1000,7);
        notify(0x57,voice_boot,8);
        if(interaction==3)notify(0x57,released,8);
        event(RBP_GATT_EVT_MTU_UPDATED);mode=1;value(&mode,1);value(&level,1);
        event(RBP_GATT_EVT_WRITE_DONE);event(RBP_GATT_EVT_WRITE_DONE);
        uint8_t caps_mode[]={11,1,0,3,interaction,0,120,0,0};
        notify(a.atvv_ctl_handle,caps_mode,sizeof caps_mode);
        assert(a.ready&&!a.atvv.open_pending); /* PTT unchanged; HTT released */
    }
    /* Native HTT START wins a concurrent MIC_OPEN: BUSY must preserve
     * the native stream, and repeated HID/SEARCH must not send another OPEN. */
    a=discovered;a.atvv.caps.interaction=3;a.voice_down_seen=false;
    notify(0x57,voice_boot,8);assert(a.atvv.open_pending);
    uint8_t native_start[]={4,3,2,1};notify(a.atvv_ctl_handle,native_start,4);
    uint8_t busy[]={12,15,128};notify(a.atvv_ctl_handle,busy,3);
    assert(a.atvv.stream_active&&!a.atvv.failed&&!a.atvv.open_pending);
    unsigned native_commands=commands;uint8_t search=8;
    notify(a.atvv_ctl_handle,&search,1);notify(0x57,voice_boot,8);
    assert(commands==native_commands);
    notify(0x57,released,8);assert(a.atvv.closing);
    /* Failed/idle adapters must never run pending MIC timers or commands. */
    a.state=ST_FAILED;a.atvv.open_queued=true;a.atvv.open_pending=true;
    a.atvv.open_deadline_ms=100;accepted_calls=commands;
    rc003_adapter_tick(&a,200);assert(commands==accepted_calls&&a.atvv.open_pending);
    rc003_adapter_detach(&a);rc003_adapter_tick(&a,300);
    assert(commands==accepted_calls&&!a.atvv.open_pending);
    /* Exercise actual audio after successive cached reconnects, including
     * a dirty prior decoder. Discovery-derived profile survives; no old
     * seed, sequence, or stream state survives. No SYNC is sent here. */
    a=discovered;
    for(unsigned cycle=0;cycle<3;cycle++) {
        a.atvv.codec_config[0]=123;a.atvv.codec_config[2]=42;
        a.atvv.frame_no=99;a.atvv.frame_bytes=17;a.atvv.stream_active=true;
        rc003_adapter_detach(&a);rc003_adapter_detach(&a);
        assert(a.atvv.profile_init_without_sync&&!a.atvv.decoder_ready);
        rc003_adapter_start_bound(&a,1000,7);
        event(RBP_GATT_EVT_MTU_UPDATED);mode=1;value(&mode,1);value(&level,1);
        event(RBP_GATT_EVT_WRITE_DONE);event(RBP_GATT_EVT_WRITE_DONE);
        uint8_t caps_htt[]={11,1,0,3,3,0,120,0,0};
        notify(a.atvv_ctl_handle,caps_htt,sizeof caps_htt);
        voice_starts=voice_ends=voice_faults=voice_bytes=0;
        uint8_t start[]={4,cycle?3:0,2,cycle?6:0};
        notify(a.atvv_ctl_handle,start,4);
        assert(voice_starts==1&&a.atvv.decoder_ready&&!a.atvv.saw_sync);
        for(unsigned i=0;i<4;i++)assert(seed[i]==0);
        uint8_t encoded[120];for(unsigned i=0;i<120;i++)encoded[i]=(uint8_t)(i+cycle);
        notify(a.atvv_audio_handle,encoded,64);
        notify(a.atvv_audio_handle,encoded+64,56);
        assert(!voice_faults&&voice_bytes==120&&!memcmp(recorded,encoded,120));
        assert(a.atvv.frame_no==1&&!a.atvv.frame_bytes);
        uint8_t stop[]={0,0};notify(a.atvv_ctl_handle,stop,2);
        assert(voice_ends==1&&!a.atvv.stream_active&&a.cache_valid);
    }
    /* Another bond cannot inherit this verified-map exception. */
    rc003_adapter_detach(&a);rc003_adapter_start_bound(&a,2000,8);
    assert(!a.atvv.profile_init_without_sync&&!a.rc003_boot_layout);
    /* Host start queues once, does not emit from RPC, and can be cancelled
     * before submission. Disabled/not-ready and duplicate pending reject. */
    a=discovered;a.now_ms=1000;a.atvv.now_ms=1000;
    unsigned rpc_commands=commands;
    assert(rc003_adapter_mic_start(&a)==RBP_STATUS_OK);
    assert(commands==rpc_commands&&a.atvv.open_queued);
    assert(rc003_adapter_mic_start(&a)==RBP_STATUS_BUSY);
    assert(rc003_adapter_mic_stop(&a)&&!a.atvv.open_pending);
    assert(commands==rpc_commands);
    assert(rc003_adapter_mic_start(&a)==RBP_STATUS_OK);
    rc003_adapter_tick(&a,1001);assert(commands==rpc_commands+1);
    rc003_adapter_tick(&a,1002);assert(commands==rpc_commands+1);
    uint8_t rejected[]={12,15,2};
    notify(a.atvv_ctl_handle,rejected,3);
    assert(a.state==ST_FAILED&&a.cache_valid); /* rejection is visible, no hang */
    a=discovered;a.now_ms=1000;a.atvv.now_ms=1000;
    voice_wanted=false;assert(rc003_adapter_mic_start(&a)==RBP_STATUS_BAD_STATE);
    voice_wanted=true;assert(rc003_adapter_mic_start(&a)==RBP_STATUS_OK);
    rpc_commands=commands;voice_wanted=false;
    rc003_adapter_tick(&a,1001);
    assert(commands==rpc_commands&&!a.atvv.open_pending&&!a.atvv.open_queued);
    voice_wanted=true;
    puts("Reference adapter: discovery, bond-scoped cache, early search/cancel, invalidation and ATT failure passed");
}

void rbp_server_set_voice_caps(rbp_server_t*s,const rbp_audio_caps_t*c){(void)s;(void)c;}
