#include "../../product/faults.h"
#include "rc003_adapter.h"
#include "device_model.h"
#include "rbp/frame.h"
#include <string.h>
#include "../../debug/trace.h"

/* ---- discovery states ---- */
enum {
    ST_IDLE = 0,
    ST_MTU,
    ST_FIND_GATT_SVC, ST_FIND_CHANGED_CHAR, ST_DISC_CHANGED_DESC, ST_SUB_CHANGED,
    ST_FIND_HID_SVC,
    ST_FIND_MAP_CHAR,
    ST_READ_MAP,
    ST_ENUM_REPORT_CHARS,
    ST_DISC_DESCS,
    ST_READ_REPORT_REF,
    ST_SUBSCRIBE_REPORT,
    ST_FIND_BAT_SVC,
    ST_FIND_BAT_CHAR,
    ST_DISC_BAT_DESCS,
    ST_READ_BATTERY,
    ST_SUBSCRIBE_BATTERY,
    ST_FIND_ATVV_SVC,
    ST_FIND_ATVV_TX,
    ST_FIND_ATVV_CTL,
    ST_FIND_ATVV_AUDIO,
    ST_DISC_CTL_DESC, ST_DISC_AUDIO_DESC,
    ST_SUBSCRIBE_CTL,
    ST_SUBSCRIBE_AUDIO,
    ST_ATVV_PREHANDSHAKE,
    ST_DONE,
    ST_FAILED,
    ST_FIND_PROTOCOL_MODE, ST_SET_PROTOCOL_MODE, ST_READ_MAP_CONT,
    ST_READ_PROTOCOL_MODE, ST_VERIFY_PROTOCOL_MODE,
    ST_FIND_HID_INFO, ST_READ_HID_INFO,
    ST_UNICOM_SERVICE, ST_UNICOM_CHAR, ST_UNICOM_DESC, ST_UNICOM_SUB, ST_UNICOM_WAIT,
};

#define INIT_TIMEOUT_MS 2000u /* local submission / ATVV CAPS budget */
#define ATT_RESPONSE_TIMEOUT_MS 30000u /* Core Vol 3, Part F, 3.3.3 */
#define INIT_TOTAL_TIMEOUT_MS 60000u

/* Preserve the failed step in the product error event before detach resets
 * the adapter. No raw reports or BLE handles are part of the host interface. */
static void fail_init(rc003_adapter_t *a,const char *cause,bool invalidate_cache)
{
    if(invalidate_cache){a->cache_valid=false;a->cache_safe=false;}
    a->pending_search=false;
    char message[]="init failed: S=00 E=00 M=0000 N=00 I=00 T=0";
    a->failed_stage=a->state;
    bool timed_out=strstr(cause,"timeout")!=NULL;
    rbp_fault_record(RBP_FAULT_ADAPTER,a->state,a->last_att_error,timed_out);
    DT(DT_HID,DT_ERROR,3,a->state,a->last_att_error,a->map_len,a->char_count);
    const char fields[]="SEMNI";
    uint16_t values[]={a->state,a->last_att_error,a->map_len,a->char_count,(uint8_t)a->cur_char};
    for(unsigned i=0;i<5;i++) {
        char *p=strchr(message,fields[i])+2;
        unsigned width=i==2?4:2;
        for(unsigned j=0;j<width;j++)p[width-j-1]="0123456789ABCDEF"[(values[i]>>(4*j))&15];
    }
    if(timed_out)message[sizeof message-2]='1';
    a->state=ST_FAILED;
    rbp_server_adapter_failed(a->server,message);
}

/* Transport loss closes this attempt, not the bonded attribute database. */
static void fail_initialization(rc003_adapter_t *a,const char *cause)
{ fail_init(a,cause,true); }
static void fail_transport(rc003_adapter_t *a,const char *cause)
{ fail_init(a,cause,false); }

/* ATVV service UUID (128-bit, little-endian on air):
 * AB5E0001-5A21-4F05-BC7D-AF01F617B664 */
static const uint8_t k_atvv_svc_uuid[16] = {
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x01, 0x00, 0x5E, 0xAB,
};
static const uint8_t k_atvv_tx_uuid[16] = {
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x02, 0x00, 0x5E, 0xAB,
};
static const uint8_t k_atvv_ctl_uuid[16] = {
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x04, 0x00, 0x5E, 0xAB,
};
static const uint8_t k_atvv_audio_uuid[16] = {
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x03, 0x00, 0x5E, 0xAB,
};

/* Forward decls */
static void begin_next(rc003_adapter_t *a, uint32_t now_ms);
static void handle_op_failure(rc003_adapter_t *a);
static void finish_voice_ready(rc003_adapter_t *a) {
    if(a->state!=ST_ATVV_PREHANDSHAKE || !a->atvv.caps_valid || !a->atvv_present || !a->caps_requested)return;
    a->voice_ready=true;a->state=ST_DONE;a->ready=true;a->retry=0;
    uint8_t interaction=a->atvv.caps.interaction==0?RBP_VI_ON_REQUEST:a->atvv.caps.interaction==1?RBP_VI_PTT:RBP_VI_HTT;
    rbp_server_on_voice_state(a->server,RBP_VOICE_READY,interaction,a->atvv.sample_rate);
    rbp_server_on_link(a->server,RBP_LINK_READY,0,a->now_ms);
    a->cache_valid=a->cache_safe && a->cache_peer_id!=0;
    a->restoring=false;
    if(a->pending_search || (a->atvv.caps.interaction!=1 && a->voice_down_seen)) {
        a->pending_search=false;
        rc003_atvv_request_start(&a->atvv);
    }
}

static void key_state_send(rc003_adapter_t *a, uint64_t bits)
{
    rbp_keys_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = RBP_KEYS_KIND_PHYSICAL;
    rep.pressed_bits = bits;
    rep.captured_us = (uint64_t)a->now_ms * 1000u;
    rbp_server_on_keys(a->server, &rep);
}

/* map key id -> catalog slot (0xFFFF when absent) */
static uint16_t key_slot(uint16_t key_id)
{
    const rbp_device_profile_t *p = &RBP_PROFILE_RC003_VOICE;
    for (uint8_t i = 0; i < p->key_count; i++)
        if (p->keys[i].key_id == key_id) return i;
    return 0xFFFF;
}

static void recompute_keys(rc003_adapter_t *a)
{
    uint64_t keys=0,bits=0;
    for(uint8_t i=0;i<a->char_count;i++)keys|=a->keys_by_report[i];
    bool voice_now=(keys&((uint64_t)1<<RBP_KEY_VOICE))!=0;
    if(!voice_now && a->voice_down_seen)a->pending_search=false;
    /* RC003 HID voice hold is also meaningful in HTT after wake/reconnect.
     * ATVV 4.4 permits host MIC_OPEN in HTT; an already active native stream
     * is guarded by request_start, and remote BUSY preserves that stream.
     * PTT remains toggle-driven, not converted to a held-button model. */
    if(a->voice_ready && a->atvv.caps.interaction!=1) {
        if(voice_now && !a->voice_down_seen)rc003_atvv_request_start(&a->atvv);
        else if(!voice_now && a->voice_down_seen)rc003_atvv_request_stop(&a->atvv);
    }
    if(keys & ((uint64_t)1<<RBP_KEY_VOICE))a->voice_down_seen=true;
    else if(a->voice_down_seen && !a->voice_key_verified) {
        a->voice_key_verified=true;rbp_server_set_profile(a->server,&RBP_PROFILE_RC003_VOICE);
    }
    a->voice_down_seen=voice_now;
    for(uint16_t k=1;k<64;k++)if(keys & ((uint64_t)1<<k)) {
        if(k==RBP_KEY_VOICE && !a->voice_key_verified)continue;
        uint16_t slot=key_slot(k);if(slot<64)bits|=(uint64_t)1<<slot;
    }
    if(bits!=a->pressed_bits) {a->pressed_bits=bits;key_state_send(a,bits);}
}

#include "unicom_profile.inc"
#include "unicom_runtime.inc"

/* ---------------- sink callbacks for the ATVV engine ---------------- */

static bool atvv_write_tx(void *user, const uint8_t *data, uint16_t len)
{
    rc003_adapter_t *a = (rc003_adapter_t *)user;
    if (!a->atvv_tx_handle) return false;
    return a->gatt->write_command && a->gatt->write_command(a->gatt_user, a->atvv_tx_handle, data, len) == 0;
}

static void atvv_on_source_begin(void *user) {
    rc003_adapter_t *a=user;rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
    rbp_server_on_voice(a->server,&ev,a->now_ms);
}
static void atvv_on_started(void *user, const rbp_audio_format_t *format)
{
    rc003_adapter_t *a = (rc003_adapter_t *)user;
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start = *format;
    rbp_server_on_voice(a->server, &ev, a->now_ms);
}

static void atvv_on_encoded(void *user, const uint8_t *data, uint16_t len)
{
    rc003_adapter_t *a = (rc003_adapter_t *)user;
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_ENCODED;
    ev.u.encoded.data=data;ev.u.encoded.len=len;
    ev.u.encoded.unit_size=len;ev.u.encoded.samples=(uint32_t)len*2;
    rbp_server_on_voice(a->server, &ev, a->now_ms);
}

static void atvv_on_format(void *user, const rbp_audio_format_t *format)
{
    rc003_adapter_t *a = (rc003_adapter_t *)user;
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_FORMAT;
    ev.u.format = *format;
    rbp_server_on_voice(a->server, &ev, a->now_ms);
}

static void atvv_on_ended(void *user, uint8_t reason)
{
    rc003_adapter_t *a = (rc003_adapter_t *)user;
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_END;
    ev.u.end.reason = reason;
    rbp_server_on_voice(a->server, &ev, a->now_ms);
}

static bool atvv_want_voice(void *user) {return rbp_server_voice_wanted(((rc003_adapter_t*)user)->server);}
static void atvv_on_fault(void *user,uint8_t reason) {
    rc003_adapter_t *a=user;
    /* A timed-out MIC_OPEN has an uncertain remote outcome. Retire this
     * bearer so a late START cannot become a new/ghost capture; retain the
     * host's enable intent and the validated bonded database for reconnect. */
    if(reason==RBP_END_DEVICE_ERROR && !a->atvv.stream_active) {
        fail_transport(a,"microphone command timeout");return;
    }
    rbp_voice_evt_t ev;memset(&ev,0,sizeof ev);
    ev.type=RBP_VOICE_EVT_FAULT;ev.u.end.reason=reason;rbp_server_on_voice(a->server,&ev,a->now_ms);
}

/* ---------------- lifecycle ---------------- */

void rc003_adapter_init(rc003_adapter_t *a, const rbp_gatt_client_t *gatt, void *gatt_user,
                        rbp_server_t *server)
{
    memset(a, 0, sizeof(*a));
    a->gatt = gatt;
    a->gatt_user = gatt_user;
    a->server = server;
    static const rbp_codec_t codecs[]={{RBP_CODEC_IMA_HI,1}};
    static const rbp_audio_caps_t caps={codecs,ATVV_MAX_FRAME,1};
    rbp_server_set_voice_caps(server,&caps);
    rc003_atvv_sink_t sink;
    memset(&sink,0,sizeof sink);
    sink.user = a;
    sink.write_tx = atvv_write_tx;
    sink.on_started = atvv_on_started;
    sink.on_encoded = atvv_on_encoded;
    sink.on_source_begin = atvv_on_source_begin;
    sink.on_format = atvv_on_format;
    sink.on_ended = atvv_on_ended;
    sink.want_voice=atvv_want_voice;sink.on_fault=atvv_on_fault;
    rc003_atvv_init(&a->atvv, &sink);
}

uint8_t rc003_adapter_match(const char *name, uint8_t name_len, bool hid_uuid_in_adv)
{
    /* Observed on the user's Remote 2 Pro during hardware bring-up.
     * This alias admits a candidate only; GATT/HOGP/ATVV initialization still
     * validates compatibility. Name may arrive alone in a scan response. */
    static const char mi_voice_name[] =
        "\xe5\xb0\x8f\xe7\xb1\xb3\xe8\x93\x9d\xe7\x89\x99\xe8\xaf\xad"
        "\xe9\x9f\xb3\xe9\x81\xa5\xe6\x8e\xa7\xe5\x99\xa8";
    if(name && name_len==sizeof(mi_voice_name)-1 && !memcmp(name,mi_voice_name,sizeof(mi_voice_name)-1))return 1;
    /* A generic HID service or Xiaomi brand alone is not a match. */
    (void)hid_uuid_in_adv;
    if(name)for(unsigned i=0;i+5<=name_len;i++) {
        if(i && name[i-1]!=' ' && name[i-1]!='-' && name[i-1]!='_')continue;
        if((name[i]=='R'||name[i]=='r')&&(name[i+1]=='C'||name[i+1]=='c')&&
           !memcmp(name+i+2,"003",3) && (i+5==name_len || name[i+5]==' ' || name[i+5]=='-' || name[i+5]=='_'))return 1;
    }
    return 0;
}

static int start_op(rc003_adapter_t *a, uint8_t state, uint32_t now_ms)
{
    if(state==ST_FIND_GATT_SVC || state==ST_FIND_HID_SVC || state==ST_FIND_BAT_SVC || state==ST_FIND_ATVV_SVC) a->svc_start=a->svc_end=0;
    if(state==ST_DISC_CTL_DESC || state==ST_DISC_AUDIO_DESC || state==ST_DISC_CHANGED_DESC || state==ST_DISC_DESCS || state==ST_DISC_BAT_DESCS)a->desc_boundary=false;
    if(state==ST_FIND_ATVV_SVC && a->unicom.selected)state=ST_UNICOM_SERVICE;
    if(state==ST_UNICOM_SERVICE)a->svc_start=a->svc_end=0;
    if(state==ST_UNICOM_DESC)a->desc_boundary=false;
    a->state = state;
    if(state==ST_ATVV_PREHANDSHAKE)a->caps_requested=false;
    a->retry=0;
    DT(DT_HID,DT_INFO,1,state,a->svc_start,a->svc_end,now_ms);
    a->last_att_error=0;
    a->deadline_ms = now_ms + INIT_TIMEOUT_MS;
    return 0;
}

void rc003_adapter_start(rc003_adapter_t *a, uint32_t now_ms)
{
    rc003_adapter_start_bound(a,now_ms,0);
}

void rc003_adapter_start_bound(rc003_adapter_t *a, uint32_t now_ms, uint32_t peer_id)
{
    if(peer_id && a->cache_valid && a->cache_peer_id==peer_id) {
        DT(DT_HID,DT_INFO,13,peer_id,a->service_changed_handle,a->char_count,now_ms);
        a->restoring=true;a->now_ms=now_ms;a->init_deadline_ms=now_ms+INIT_TOTAL_TIMEOUT_MS;
        rbp_server_set_profile(a->server,a->voice_key_verified?&RBP_PROFILE_RC003_VOICE:&RBP_PROFILE_RC003);
        start_op(a,ST_MTU,now_ms);begin_next(a,now_ms);return;
    }
    const rbp_gatt_client_t *gatt=a->gatt;void *user=a->gatt_user;rbp_server_t *server=a->server;
    rc003_adapter_init(a,gatt,user,server);a->now_ms=now_ms;
    a->cache_peer_id=peer_id;
    a->init_deadline_ms=now_ms+INIT_TOTAL_TIMEOUT_MS;
    rbp_server_set_profile(server,&RBP_PROFILE_RC003);
    start_op(a,ST_MTU,now_ms);begin_next(a,now_ms);
}

void rc003_adapter_detach(rc003_adapter_t *a)
{
    if(a->cache_valid) {
        /* Retain only database/decoder metadata in place, not a second RAM copy.
         * Stream, held keys and procedure ownership never survive a link. */
        rc003_atvv_sink_t sink=a->atvv.sink;
        rc003_atvv_init(&a->atvv,&sink);
        /* Reinstall the profile derived from the retained, verified Report
         * Map. Decoder seed/sequence/stream state remain freshly reset. */
        a->atvv.profile_init_without_sync=a->rc003_boot_layout;
        a->state=ST_IDLE;a->retry=0;a->ready=false;a->voice_ready=false;
        a->restoring=false;a->pending_search=false;a->caps_requested=false;a->voice_down_seen=false;
        a->pressed_bits=0;memset(a->keys_by_report,0,sizeof a->keys_by_report);
        return;
    }
    const rbp_gatt_client_t *gatt=a->gatt;void *user=a->gatt_user;rbp_server_t *server=a->server;
    rc003_adapter_init(a,gatt,user,server);
}

/* ---------------- discovery pump ---------------- */

void rc003_adapter_tick(rc003_adapter_t *a, uint32_t now_ms)
{
    a->now_ms = now_ms;
    if(a->state==ST_IDLE || a->state==ST_FAILED)return;
    if(a->unicom.selected) {
        unicom_tick(a);
        if(a->state==ST_DONE || a->state==ST_UNICOM_WAIT || a->state==ST_FAILED)return;
    }
    if(!a->unicom.selected && a->atvv.open_pending && !rbp_server_voice_wanted(a->server))
        rc003_atvv_request_stop(&a->atvv); /* enable/session revoked before START */
    if(!a->unicom.selected && a->voice_ready)(void)rc003_atvv_tick(&a->atvv, now_ms);
    if(a->state==ST_IDLE || a->state==ST_FAILED)return; /* callback can detach */
    if(a->state!=ST_IDLE && a->state!=ST_DONE && a->state!=ST_FAILED &&
       (int32_t)(now_ms-a->init_deadline_ms)>=0){fail_transport(a,"timeout total");return;}
    if (a->state != ST_IDLE && a->state != ST_DONE && a->state != ST_FAILED &&
        (int32_t)(now_ms - a->deadline_ms) >= 0) {
        /* ATT may still be busy after an application deadline. Never start
         * a new procedure on top of it: reset the physical connection. */
        if(a->state==ST_ATVV_PREHANDSHAKE && !a->restoring)handle_op_failure(a);
        else fail_transport(a,"timeout");
        return;
    }
    if(a->retry)begin_next(a,now_ms);
}

static void begin_next(rc003_adapter_t *a, uint32_t now_ms)
{
    int rc=0;uint8_t starting_state=a->state;
    switch (a->state) {
    case ST_UNICOM_SERVICE:
        rc=a->gatt->disc_service_by_uuid16(a->gatt_user,0xfd00);break;
    case ST_UNICOM_CHAR:
        rc=a->gatt->read_chars_by_uuid16(a->gatt_user,a->svc_start,a->svc_end,0xfd02);break;
    case ST_UNICOM_DESC:
        rc=a->gatt->disc_char_descs(a->gatt_user,a->unicom.fd+1,a->svc_end);break;
    case ST_UNICOM_SUB: {
        uint8_t on[]={1,0};
        rc=a->gatt->write_value(a->gatt_user,a->unicom.fd_cccd,on,2);break;
    }
    case ST_UNICOM_WAIT:return;
    case ST_FIND_HID_INFO:
        rc=a->gatt->read_chars_by_uuid16(a->gatt_user,a->svc_start,a->svc_end,0x2a4a);break;
    case ST_READ_HID_INFO:
        rc=a->gatt->read_value(a->gatt_user,a->hid_info_handle);break;
    case ST_FIND_PROTOCOL_MODE:
        rc=a->gatt->read_chars_by_uuid16(a->gatt_user,a->svc_start,a->svc_end,RBP_UUID16_PROTOCOL_MODE);break;
    case ST_READ_PROTOCOL_MODE:case ST_VERIFY_PROTOCOL_MODE:
        rc=a->gatt->read_value(a->gatt_user,a->protocol_mode_handle);break;
    case ST_SET_PROTOCOL_MODE: {
        uint8_t report_mode=1;
        rc=a->gatt->write_command(a->gatt_user,a->protocol_mode_handle,&report_mode,1);
        if(!rc){start_op(a,ST_VERIFY_PROTOCOL_MODE,now_ms);begin_next(a,now_ms);return;}
        break;
    }
    case ST_READ_MAP_CONT:
        rc=a->gatt->read_long_value(a->gatt_user,a->map_value_handle,a->map_len);break;
    case ST_MTU:
        rc=a->gatt->exchange_mtu(a->gatt_user, RBP_ATT_MTU);
        break;
    case ST_FIND_GATT_SVC:rc=a->gatt->disc_service_by_uuid16(a->gatt_user,0x1801);break;
    case ST_FIND_CHANGED_CHAR:rc=a->gatt->read_chars_by_uuid16(a->gatt_user,a->svc_start,a->svc_end,0x2a05);break;
    case ST_DISC_CHANGED_DESC:rc=a->gatt->disc_char_descs(a->gatt_user,a->service_changed_handle+1,a->svc_end);break;
    case ST_SUB_CHANGED: {uint8_t on[2]={2,0};rc=a->gatt->write_value(a->gatt_user,a->service_changed_cccd,on,2);break;}
    case ST_DISC_CTL_DESC:rc=a->gatt->disc_char_descs(a->gatt_user,a->atvv_ctl_handle+1,a->svc_end);break;
    case ST_DISC_AUDIO_DESC:rc=a->gatt->disc_char_descs(a->gatt_user,a->atvv_audio_handle+1,a->svc_end);break;
    case ST_FIND_HID_SVC:
        rc=a->gatt->disc_service_by_uuid16(a->gatt_user, RBP_UUID16_HID_SERVICE);
        break;
    case ST_FIND_MAP_CHAR:
        rc=a->gatt->read_chars_by_uuid16(a->gatt_user, a->svc_start, a->svc_end,
                                      RBP_UUID16_HID_REPORT_MAP);
        break;
    case ST_READ_MAP:
        a->map_len = 0;a->map_long_started=false;
        rc=a->gatt->read_value(a->gatt_user, a->map_value_handle);
        break;
    case ST_ENUM_REPORT_CHARS:
        rc=a->gatt->read_chars_by_uuid16(a->gatt_user, a->svc_start, a->svc_end,
                                      RBP_UUID16_HID_REPORT);
        break;
    case ST_DISC_DESCS: {
        uint16_t start = a->chars[a->cur_char].value_handle + 1;
        uint16_t end = (uint16_t)(a->cur_char + 1 < a->char_count
                                      ? a->chars[a->cur_char + 1].value_handle - 1
                                      : a->svc_end);
        if (start <= end) {
            rc=a->gatt->disc_char_descs(a->gatt_user, start, end);
        } else {
            /* no descriptors: record and move on via failure path */
            a->chars[a->cur_char].ref_handle = 0;
            a->chars[a->cur_char].cccd_handle = 0;
            if (a->cur_char + 1 < a->char_count) {
                a->cur_char++;
                start_op(a, ST_DISC_DESCS, now_ms);
                begin_next(a, now_ms);
                return;
            }
            start_op(a, ST_FIND_BAT_SVC, now_ms);
            begin_next(a, now_ms);
            return;
        }
        break;
    }
    case ST_READ_REPORT_REF: {
        rc003_report_char_t *c = &a->chars[a->cur_char];
        rc=a->gatt->read_value(a->gatt_user, c->ref_handle);
        break;
    }
    case ST_SUBSCRIBE_REPORT: {
        rc003_report_char_t *c = &a->chars[a->cur_char];
        bool mapped=a->unicom.selected;
        for(unsigned i=0;i<a->map.input_count;i++)if(a->map.inputs[i].report_id==c->report_id)mapped=true;
        if (!c->cccd_handle || c->report_type!=1 || !mapped) {
            /* skip to next char */
            a->cur_char++;
            if (a->cur_char >= a->char_count) {
                start_op(a, ST_FIND_BAT_SVC, now_ms);
                begin_next(a, now_ms);
                return;
            }
            start_op(a, ST_DISC_DESCS, now_ms);
            begin_next(a, now_ms);
            return;
        }
        uint8_t on[2] = { 0x01, 0x00 };
        rc=a->gatt->write_value(a->gatt_user, c->cccd_handle, on, 2);
        break;
    }
    case ST_FIND_BAT_SVC:
        if(a->unicom.selected && !unicom_reports(a)){unicom_bad(a,"Unicom report references");return;}
        for(unsigned i=0;i<a->map.input_count;i++) {
            bool found=false;
            for(unsigned j=0;j<a->char_count;j++)if(a->chars[j].subscribed && a->chars[j].report_id==a->map.inputs[i].report_id)found=true;
            if(!found) {a->state=ST_FAILED;rbp_server_adapter_failed(a->server,"HID input report not subscribed");return;}
        }
        if(!a->map.input_count) {a->state=ST_FAILED;rbp_server_adapter_failed(a->server,"HID input map empty");return;}
        rc=a->gatt->disc_service_by_uuid16(a->gatt_user, RBP_UUID16_BATTERY_SERVICE);
        break;
    case ST_FIND_BAT_CHAR:
        rc=a->gatt->read_chars_by_uuid16(a->gatt_user, a->svc_start, a->svc_end,
                                      RBP_UUID16_BATTERY_LEVEL);
        break;
    case ST_DISC_BAT_DESCS:
        rc=a->gatt->disc_char_descs(a->gatt_user, (uint16_t)(a->bat_value_handle + 1), a->svc_end);
        break;
    case ST_READ_BATTERY:
        rc=a->gatt->read_value(a->gatt_user, a->bat_value_handle);
        break;
    case ST_SUBSCRIBE_BATTERY: {
        uint8_t on[2] = { 0x01, 0x00 };
        rc=a->gatt->write_value(a->gatt_user, a->bat_cccd_handle, on, 2);
        break;
    }
    case ST_FIND_ATVV_SVC:
        rc=a->gatt->disc_service_by_uuid128(a->gatt_user, k_atvv_svc_uuid);
        break;
    case ST_FIND_ATVV_TX:
        rc=a->gatt->read_chars_by_uuid128(a->gatt_user, a->svc_start, a->svc_end,
                                       k_atvv_tx_uuid);
        break;
    case ST_FIND_ATVV_CTL:
        rc=a->gatt->read_chars_by_uuid128(a->gatt_user, a->svc_start, a->svc_end,
                                       k_atvv_ctl_uuid);
        break;
    case ST_FIND_ATVV_AUDIO:
        rc=a->gatt->read_chars_by_uuid128(a->gatt_user, a->svc_start, a->svc_end,
                                       k_atvv_audio_uuid);
        break;
    case ST_SUBSCRIBE_CTL: {
        /* RC003 quirk: CTL CCCD must be enabled before AUDIO CCCD */
        uint8_t on[2] = { 0x01, 0x00 };
        rc=a->gatt->write_value(a->gatt_user, a->atvv_ctl_cccd, on, 2);
        break;
    }
    case ST_SUBSCRIBE_AUDIO: {
        uint8_t on[2] = { 0x01, 0x00 };
        rc=a->gatt->write_value(a->gatt_user, a->atvv_audio_cccd, on, 2);
        break;
    }
    case ST_ATVV_PREHANDSHAKE: {
        /* RC003 needs one GET_CAPS after subscribe before audio flows */
        uint8_t cmd[6];
        (void)atvv_build_get_caps(cmd);
        rc=a->gatt->write_command(a->gatt_user,a->atvv_tx_handle,cmd,6);
        if(!rc) {
            a->caps_requested=true;
            a->deadline_ms=now_ms+INIT_TIMEOUT_MS;
            finish_voice_ready(a); /* only after actual GET_CAPS submission */
        }
        break;
    }
    default:
        break;
    }
    if(a->state==starting_state) {
        if(rc==RBP_GATT_RETRY){a->retry=1;return;}
        a->retry=0;
        if(rc==0 && a->state!=ST_ATVV_PREHANDSHAKE)
            a->deadline_ms=now_ms+ATT_RESPONSE_TIMEOUT_MS;
        if(rc<0){DT(DT_HID,DT_ERROR,4,a->state,(uint32_t)rc,0,0);handle_op_failure(a);return;}
    }
}

static void handle_op_failure(rc003_adapter_t *a)
{
    if(a->restoring){fail_initialization(a,"cached attributes failed");return;}
    switch (a->state) {
    case ST_FIND_PROTOCOL_MODE:
        /* Optional characteristic absent. Other errors must not silently
         * switch the device into an unknown keyboard interpretation. */
        fail_initialization(a,"protocol mode discovery");break;
    case ST_FIND_GATT_SVC:case ST_FIND_CHANGED_CHAR:case ST_DISC_CHANGED_DESC:case ST_SUB_CHANGED:
        a->service_changed_handle=0;start_op(a,ST_FIND_HID_SVC,a->now_ms);begin_next(a,a->now_ms);break;
    case ST_FIND_BAT_SVC:
    case ST_FIND_BAT_CHAR:
    case ST_DISC_BAT_DESCS:
    case ST_READ_BATTERY:
    case ST_SUBSCRIBE_BATTERY:
        /* battery is optional: continue with ATVV discovery */
        a->retry = 0;
        a->bat_value_handle = 0;
        start_op(a, ST_FIND_ATVV_SVC, a->now_ms);
        begin_next(a, a->now_ms);
        break;
    case ST_FIND_ATVV_SVC:
        /* no voice: keys still usable */
        a->atvv_present = false;
        a->retry = 0;
        a->state = ST_DONE;
        a->ready = true;
        rbp_server_on_voice_state(a->server, RBP_VOICE_UNSUPPORTED, RBP_VI_UNAVAILABLE, 0);
        rbp_server_on_link(a->server, RBP_LINK_READY, 0, a->now_ms);
        break;
    case ST_FIND_ATVV_TX:
    case ST_FIND_ATVV_CTL:
    case ST_FIND_ATVV_AUDIO:
    case ST_DISC_CTL_DESC:case ST_DISC_AUDIO_DESC:
    case ST_SUBSCRIBE_CTL:
    case ST_SUBSCRIBE_AUDIO:
    case ST_ATVV_PREHANDSHAKE:
        /* voice unavailable, keys remain */
        a->retry = 0;
        a->atvv_present=false;
        a->state = ST_DONE;
        a->ready = true;
        rbp_server_on_voice_state(a->server, RBP_VOICE_FAILED, RBP_VI_UNAVAILABLE, 0);
        rbp_server_on_link(a->server, RBP_LINK_READY, 0, a->now_ms);
        break;
    default:
        fail_initialization(a,"failed");
        break;
    }
}

static void advance(void)
{
    /* placeholder; real flow advanced inline in events */
}

/* ---------------- GATT event handling ---------------- */

void rc003_adapter_on_gatt(rc003_adapter_t *a, const rbp_gatt_evt_t *evt)
{
    (void)advance;
    DT(DT_HID,DT_DETAIL,2,a->state,evt->type,evt->status,((uint32_t)evt->offset<<16)|evt->len);
    if(evt->value && evt->len && evt->type!=RBP_GATT_EVT_NOTIFY)DT_BLOB(DT_HID,10,evt->value,evt->len);
    /* Bonded servers can indicate changes before our encrypted-start callback.
     * Invalidate even while detached; never lose that indication at ST_IDLE. */
    if(evt->type==RBP_GATT_EVT_NOTIFY && a->service_changed_handle && evt->handle==a->service_changed_handle) {
        DT(DT_HID,DT_INFO,14,a->cache_peer_id,evt->handle,a->state,evt->len);
        a->cache_valid=false;a->cache_safe=false;
        if(a->state!=ST_IDLE && a->state!=ST_FAILED)fail_initialization(a,"GATT database changed");
        return;
    }
    if(a->state==ST_IDLE || a->state==ST_FAILED)return;
    if(evt->type==RBP_GATT_EVT_BEARER_FAILED) {
        a->last_att_error=evt->status;
        if(evt->status==0x12)fail_initialization(a,"GATT database out of sync");
        else fail_transport(a,"ATT bearer failed");
        return;
    }
    if(evt->type==RBP_GATT_EVT_CHARS_FOUND || evt->type==RBP_GATT_EVT_DESC_FOUND ||
       evt->type==RBP_GATT_EVT_READ_RSP || evt->type==RBP_GATT_EVT_SERVICE_FOUND)
        a->deadline_ms=a->now_ms+ATT_RESPONSE_TIMEOUT_MS;
    if(evt->type==RBP_GATT_EVT_PROC_ERROR)a->last_att_error=evt->status;
    if(evt->type==RBP_GATT_EVT_PROC_ERROR && evt->status==0x12) {
        fail_initialization(a,"GATT database out of sync");return;
    }
    if(a->unicom.selected && evt->type==RBP_GATT_EVT_NOTIFY) {unicom_notify(a,evt);return;}
    if(a->unicom.selected && a->state==ST_DONE && a->unicom.in_flight) {
        if(evt->type==RBP_GATT_EVT_WRITE_DONE){a->unicom.in_flight=0;return;}
        if(evt->type==RBP_GATT_EVT_PROC_ERROR){unicom_bad(a,"Unicom FB write failed");return;}
    }
    a->atvv.now_ms=a->now_ms;
    switch (a->state) {
    case ST_UNICOM_SERVICE:
        if(evt->type==RBP_GATT_EVT_SERVICE_FOUND) {
            if(a->svc_start){unicom_bad(a,"duplicate FD00");return;}
            a->svc_start=evt->svc_start;a->svc_end=evt->svc_end;
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE && a->svc_start) {
            start_op(a,ST_UNICOM_CHAR,a->now_ms);begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE || evt->type==RBP_GATT_EVT_SERVICE_NOT_FOUND || evt->type==RBP_GATT_EVT_PROC_ERROR)
            unicom_bad(a,"FD00 unavailable");
        break;
    case ST_UNICOM_CHAR:
        if(evt->type==RBP_GATT_EVT_CHARS_FOUND) {
            if(evt->len<3 || !(evt->value[0]&0x10) || a->unicom.fd){unicom_bad(a,"FD02 properties");return;}
            a->unicom.fd=evt->value[1]|((uint16_t)evt->value[2]<<8);
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE && a->unicom.fd) {
            start_op(a,ST_UNICOM_DESC,a->now_ms);begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE || evt->type==RBP_GATT_EVT_PROC_ERROR)unicom_bad(a,"FD02 unavailable");
        break;
    case ST_UNICOM_DESC:
        if(evt->type==RBP_GATT_EVT_DESC_FOUND) {
            if(evt->uuid16==0x2803)a->desc_boundary=true;
            if(!a->desc_boundary && evt->uuid16==0x2902)a->unicom.fd_cccd=evt->desc_handle;
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE && a->unicom.fd_cccd) {
            start_op(a,ST_UNICOM_SUB,a->now_ms);begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE || evt->type==RBP_GATT_EVT_PROC_ERROR)unicom_bad(a,"FD02 CCCD unavailable");
        break;
    case ST_UNICOM_SUB:
        if(evt->type==RBP_GATT_EVT_WRITE_DONE) {
            start_op(a,ST_UNICOM_WAIT,a->now_ms);
            if(a->unicom.hello)unicom_ready(a);
        } else if(evt->type==RBP_GATT_EVT_PROC_ERROR)unicom_bad(a,"FD02 subscribe failed");
        break;
    case ST_MTU:
        if (evt->type == RBP_GATT_EVT_MTU_UPDATED || evt->type == RBP_GATT_EVT_PROC_ERROR) {
            start_op(a, a->restoring?(a->service_changed_cccd?ST_SUB_CHANGED:
                a->protocol_mode_handle?ST_READ_PROTOCOL_MODE:(a->bat_value_handle?ST_READ_BATTERY:ST_SUBSCRIBE_CTL)):ST_FIND_GATT_SVC, a->now_ms);
            begin_next(a, a->now_ms);
        }
        break;

    case ST_FIND_PROTOCOL_MODE:
        if(evt->type==RBP_GATT_EVT_CHARS_FOUND && evt->len>=3) {
            if((evt->value[0]&6)!=6){fail_initialization(a,"protocol mode properties");return;}
            a->protocol_mode_handle=evt->value[1]|(evt->value[2]<<8);
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE) {
            start_op(a,a->protocol_mode_handle?ST_READ_PROTOCOL_MODE:ST_FIND_MAP_CHAR,a->now_ms);
            begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;

    case ST_READ_PROTOCOL_MODE:case ST_VERIFY_PROTOCOL_MODE:
        if(evt->type==RBP_GATT_EVT_READ_RSP) {
            /* BlueZ reads before writing. Verify a needed mode change as
             * WriteNoRsp SUCCESS only means accepted by the local stack. */
            if(evt->len!=1 || !evt->value || evt->value[0]>1 ||
               (a->state==ST_VERIFY_PROTOCOL_MODE && evt->value[0]!=1)) {
                fail_initialization(a,"protocol mode value");return;
            }
            start_op(a,evt->value[0]==1?(a->restoring?(a->bat_value_handle?ST_READ_BATTERY:ST_SUBSCRIBE_CTL):ST_FIND_MAP_CHAR):ST_SET_PROTOCOL_MODE,a->now_ms);
            begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;

    case ST_FIND_GATT_SVC:
        if(evt->type==RBP_GATT_EVT_SERVICE_NOT_FOUND)a->cache_safe=true;
        if(evt->type==RBP_GATT_EVT_SERVICE_FOUND) {a->svc_start=evt->svc_start;a->svc_end=evt->svc_end;}
        else if(evt->type==RBP_GATT_EVT_PROC_DONE && a->svc_start) {start_op(a,ST_FIND_CHANGED_CHAR,a->now_ms);begin_next(a,a->now_ms);}
        else if(evt->type==RBP_GATT_EVT_PROC_DONE || evt->type==RBP_GATT_EVT_SERVICE_NOT_FOUND || evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;
    case ST_FIND_CHANGED_CHAR:
        if(evt->type==RBP_GATT_EVT_CHARS_FOUND && evt->len>=3 && (evt->value[0]&0x20))a->service_changed_handle=evt->value[1]|(evt->value[2]<<8);
        else if(evt->type==RBP_GATT_EVT_PROC_DONE) {if(!a->service_changed_handle){a->cache_safe=true;handle_op_failure(a);break;}start_op(a,ST_DISC_CHANGED_DESC,a->now_ms);begin_next(a,a->now_ms);}
        else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;
    case ST_DISC_CHANGED_DESC:case ST_DISC_CTL_DESC:case ST_DISC_AUDIO_DESC:
        if(evt->type==RBP_GATT_EVT_DESC_FOUND) {
            if(evt->uuid16==0x2803)a->desc_boundary=true;
            if(!a->desc_boundary && evt->uuid16==RBP_UUID16_CCCD) {
                if(a->state==ST_DISC_CHANGED_DESC)a->service_changed_cccd=evt->desc_handle;
                else if(a->state==ST_DISC_CTL_DESC)a->atvv_ctl_cccd=evt->desc_handle;
                else a->atvv_audio_cccd=evt->desc_handle;
            }
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE) {
            uint16_t handle=a->state==ST_DISC_CHANGED_DESC?a->service_changed_cccd:a->state==ST_DISC_CTL_DESC?a->atvv_ctl_cccd:a->atvv_audio_cccd;
            if(!handle){handle_op_failure(a);break;}
            uint8_t next=a->state==ST_DISC_CHANGED_DESC?ST_SUB_CHANGED:a->state==ST_DISC_CTL_DESC?ST_SUBSCRIBE_CTL:ST_SUBSCRIBE_AUDIO;
            start_op(a,next,a->now_ms);begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;
    case ST_SUB_CHANGED:
        if(evt->type==RBP_GATT_EVT_WRITE_DONE) {a->cache_safe=true;start_op(a,a->restoring?(a->protocol_mode_handle?ST_READ_PROTOCOL_MODE:(a->bat_value_handle?ST_READ_BATTERY:ST_SUBSCRIBE_CTL)):ST_FIND_HID_SVC,a->now_ms);begin_next(a,a->now_ms);}
        else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;
    case ST_FIND_HID_SVC:
        if (evt->type == RBP_GATT_EVT_SERVICE_FOUND) {
            a->svc_start = evt->svc_start;
            a->svc_end = evt->svc_end;
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->svc_start) { handle_op_failure(a); return; }
            start_op(a, ST_FIND_HID_INFO, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_SERVICE_NOT_FOUND || evt->type==RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_FIND_HID_INFO:
        if(evt->type==RBP_GATT_EVT_CHARS_FOUND && evt->len>=3) {
            if(a->hid_info_handle || !(evt->value[0]&2)){handle_op_failure(a);return;}
            a->hid_info_handle=evt->value[1]|((uint16_t)evt->value[2]<<8);
        } else if(evt->type==RBP_GATT_EVT_PROC_DONE) {
            if(!a->hid_info_handle){handle_op_failure(a);return;}
            start_op(a,ST_READ_HID_INFO,a->now_ms);begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;
    case ST_READ_HID_INFO:
        if(evt->type==RBP_GATT_EVT_READ_RSP) {
            if(!evt->proc_complete || !evt->value || evt->len!=4 || (evt->value[3]&~3u)){handle_op_failure(a);return;}
            a->hid_flags=evt->value[3];a->hid_info_valid=true;
            DT(DT_HID,DT_INFO,12,a->hid_flags,evt->value[0]|((uint16_t)evt->value[1]<<8),evt->value[2],0);
            start_op(a,ST_FIND_PROTOCOL_MODE,a->now_ms);begin_next(a,a->now_ms);
        } else if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;
    case ST_FIND_MAP_CHAR:
        if (evt->type == RBP_GATT_EVT_CHARS_FOUND && evt->len >= 3) {
            /* declaration: [props, value_handle, uuid...] */
            a->map_value_handle = (uint16_t)(evt->value[1] | (evt->value[2] << 8));
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->map_value_handle) { handle_op_failure(a); return; }
            start_op(a, ST_READ_MAP, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_READ_MAP:case ST_READ_MAP_CONT:
        if (evt->type == RBP_GATT_EVT_READ_RSP && (evt->value || !evt->len)) {
            uint16_t space = (uint16_t)(HOGP_MAX_MAP_SIZE - a->map_len);
            if(evt->len>space || evt->offset!=a->map_len){handle_op_failure(a);return;}
            uint16_t n = evt->len;
            if(n)memcpy(a->map_buf + a->map_len, evt->value, n);
            a->map_len += n;
            if (evt->proc_complete) {
                a->unicom.selected=a->map_len==sizeof unicom_map && !memcmp(a->map_buf,unicom_map,sizeof unicom_map);
                if(a->unicom.selected) {
                    memset(&a->map,0,sizeof a->map);
                    static const uint8_t ids[]={1,3,0xfc,0xf8,0xf9,4};
                    a->map.input_count=sizeof ids;
                    for(unsigned i=0;i<sizeof ids;i++)a->map.inputs[i].report_id=ids[i];
                }
                if (!a->unicom.selected && !hogp_report_map_parse(a->map_buf, a->map_len, &a->map)) {
                    handle_op_failure(a);
                    return;
                }
                /* Independent RC003 receiver documents boot-shaped payloads
                 * despite this exact 86-byte Map. Scope compatibility to it. */
                a->rc003_boot_layout=a->map_len==86 && rbp_crc32c(a->map_buf,86)==0x6bd7daad;
#ifndef RBP_SIMULATOR
                /* Broad scan admission must not publish an arbitrary keyboard
                 * as RC003. Only the two measured layouts are product profiles.
                 * The synthetic simulator exercises additional generic HID fields. */
                if(!a->rc003_boot_layout && !a->unicom.selected) {
                    fail_initialization(a,"unsupported HID profile");return;
                }
#endif
                a->atvv.profile_init_without_sync=a->rc003_boot_layout;
                start_op(a, ST_ENUM_REPORT_CHARS, a->now_ms);
                begin_next(a, a->now_ms);
            } else if(!a->map_long_started) {
                a->map_long_started=true;
                start_op(a,ST_READ_MAP_CONT,a->now_ms);begin_next(a,a->now_ms);
            }
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_ENUM_REPORT_CHARS:
        /* RC003 historically keeps the first eight reports and ignores the
         * surplus. Only Unicom requires the measured exact report topology. */
        if(a->unicom.selected && evt->type==RBP_GATT_EVT_CHARS_FOUND && a->char_count>=RC003_MAX_REPORT_CHARS){fail_initialization(a,"too many reports");return;}
        if (evt->type == RBP_GATT_EVT_CHARS_FOUND && evt->len >= 3 &&
            a->char_count < RC003_MAX_REPORT_CHARS) {
            rc003_report_char_t *c = &a->chars[a->char_count++];
            memset(c, 0, sizeof(*c));
            c->properties=evt->value[0];
            c->value_handle = (uint16_t)(evt->value[1] | (evt->value[2] << 8));
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->char_count) { handle_op_failure(a); return; }
            a->cur_char = 0;
            start_op(a, ST_DISC_DESCS, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_DISC_DESCS:
        if (evt->type == RBP_GATT_EVT_DESC_FOUND) {
            rc003_report_char_t *c = &a->chars[a->cur_char];
            if(evt->uuid16==0x2803)a->desc_boundary=true;
            if(a->desc_boundary)break;
            if (evt->uuid16 == RBP_UUID16_REPORT_REFERENCE) c->ref_handle = evt->desc_handle;
            if (evt->uuid16 == RBP_UUID16_CCCD) c->cccd_handle = evt->desc_handle;
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            rc003_report_char_t *c = &a->chars[a->cur_char];
            if (c->ref_handle) {
                start_op(a, ST_READ_REPORT_REF, a->now_ms);
                begin_next(a, a->now_ms);
            } else if (a->cur_char + 1 < a->char_count) {
                a->cur_char++;
                start_op(a, ST_DISC_DESCS, a->now_ms);
                begin_next(a, a->now_ms);
            } else {
                start_op(a, ST_FIND_BAT_SVC, a->now_ms);
                begin_next(a, a->now_ms);
            }
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_READ_REPORT_REF: {
        rc003_report_char_t *c = &a->chars[a->cur_char];
        if (evt->type == RBP_GATT_EVT_READ_RSP && evt->len == 2) {
            c->report_id = evt->value[0];
            c->report_type = evt->value[1];
            start_op(a, ST_SUBSCRIBE_REPORT, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR || evt->type==RBP_GATT_EVT_READ_RSP) {
            handle_op_failure(a);
        }
        break;
    }

    case ST_SUBSCRIBE_REPORT: {
        if(evt->type==RBP_GATT_EVT_PROC_ERROR) {handle_op_failure(a);break;}
        if (evt->type == RBP_GATT_EVT_WRITE_DONE) {
            a->chars[a->cur_char].subscribed=true;
            if (a->cur_char + 1 < a->char_count) {
                a->cur_char++;
                start_op(a, ST_DISC_DESCS, a->now_ms);
                begin_next(a, a->now_ms);
            } else {
                start_op(a, ST_FIND_BAT_SVC, a->now_ms);
                begin_next(a, a->now_ms);
            }
        }
        break;
    }

    case ST_FIND_BAT_SVC:
        if (evt->type == RBP_GATT_EVT_SERVICE_FOUND) {
            a->svc_start = evt->svc_start;
            a->svc_end = evt->svc_end;
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->svc_start) { handle_op_failure(a); return; }
            start_op(a, ST_FIND_BAT_CHAR, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_SERVICE_NOT_FOUND ||
                   evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_FIND_BAT_CHAR:
        if (evt->type == RBP_GATT_EVT_CHARS_FOUND && evt->len >= 3) {
            a->bat_value_handle = (uint16_t)(evt->value[1] | (evt->value[2] << 8));
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->bat_value_handle) { handle_op_failure(a); return; }
            start_op(a, ST_DISC_BAT_DESCS, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_DISC_BAT_DESCS:
        if(evt->type==RBP_GATT_EVT_DESC_FOUND && evt->uuid16==0x2803)a->desc_boundary=true;
        if (evt->type == RBP_GATT_EVT_DESC_FOUND && !a->desc_boundary && evt->uuid16 == RBP_UUID16_CCCD) {
            a->bat_cccd_handle = evt->desc_handle;
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (a->bat_cccd_handle) {
                start_op(a, ST_READ_BATTERY, a->now_ms);
                begin_next(a, a->now_ms);
            } else {start_op(a,ST_READ_BATTERY,a->now_ms);begin_next(a,a->now_ms);}
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            start_op(a, ST_FIND_ATVV_SVC, a->now_ms);
            begin_next(a, a->now_ms);
        }
        break;

    case ST_READ_BATTERY:
        if(a->restoring && (evt->type==RBP_GATT_EVT_PROC_ERROR ||
           (evt->type==RBP_GATT_EVT_READ_RSP && (!evt->value || evt->len!=1 || evt->value[0]>100)))) {
            handle_op_failure(a);return;
        }
        if (evt->type == RBP_GATT_EVT_READ_RSP && evt->len == 1 && evt->value[0]<=100) {
            rbp_server_on_battery(a->server, evt->value[0], 0);
            start_op(a, a->restoring?ST_SUBSCRIBE_CTL:(a->bat_cccd_handle?ST_SUBSCRIBE_BATTERY:ST_FIND_ATVV_SVC), a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR || evt->type==RBP_GATT_EVT_READ_RSP) {
            start_op(a, ST_FIND_ATVV_SVC, a->now_ms);
            begin_next(a, a->now_ms);
        }
        break;

    case ST_SUBSCRIBE_BATTERY:
        if (evt->type == RBP_GATT_EVT_WRITE_DONE || evt->type == RBP_GATT_EVT_PROC_ERROR) {
            start_op(a, ST_FIND_ATVV_SVC, a->now_ms);
            begin_next(a, a->now_ms);
        }
        break;

    case ST_FIND_ATVV_SVC:
        if (evt->type == RBP_GATT_EVT_SERVICE_FOUND) {
            a->svc_start = evt->svc_start;
            a->svc_end = evt->svc_end;
            a->atvv_present = true;
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->atvv_present) { handle_op_failure(a); return; }
            start_op(a, ST_FIND_ATVV_TX, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_SERVICE_NOT_FOUND ||
                   evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_FIND_ATVV_TX:
        if (evt->type == RBP_GATT_EVT_CHARS_FOUND && evt->len >= 3 && (evt->value[0]&4)) {
            a->atvv_tx_handle = (uint16_t)(evt->value[1] | (evt->value[2] << 8));
        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->atvv_tx_handle) { handle_op_failure(a); return; }
            start_op(a, ST_FIND_ATVV_CTL, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_FIND_ATVV_CTL:
        if (evt->type == RBP_GATT_EVT_CHARS_FOUND && evt->len >= 3 && (evt->value[0]&0x10)) {
            a->atvv_ctl_handle = (uint16_t)(evt->value[1] | (evt->value[2] << 8));

        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->atvv_ctl_handle) { handle_op_failure(a); return; }
            start_op(a, ST_DISC_CTL_DESC, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_SUBSCRIBE_CTL:
        if (evt->type == RBP_GATT_EVT_WRITE_DONE) {
            start_op(a, a->restoring?ST_SUBSCRIBE_AUDIO:ST_FIND_ATVV_AUDIO, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_FIND_ATVV_AUDIO:
        if (evt->type == RBP_GATT_EVT_CHARS_FOUND && evt->len >= 3 && (evt->value[0]&0x10)) {
            a->atvv_audio_handle = (uint16_t)(evt->value[1] | (evt->value[2] << 8));

        } else if (evt->type == RBP_GATT_EVT_PROC_DONE) {
            if (!a->atvv_audio_handle) { handle_op_failure(a); return; }
            start_op(a, ST_DISC_AUDIO_DESC, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_SUBSCRIBE_AUDIO:
        if (evt->type == RBP_GATT_EVT_WRITE_DONE) {
            start_op(a, ST_ATVV_PREHANDSHAKE, a->now_ms);
            begin_next(a, a->now_ms);
        } else if (evt->type == RBP_GATT_EVT_PROC_ERROR) {
            handle_op_failure(a);
        }
        break;

    case ST_ATVV_PREHANDSHAKE:
        if(evt->type==RBP_GATT_EVT_PROC_ERROR)handle_op_failure(a);
        break;

    default:
        /* runtime notifications (idle/done) */
        break;
    }

    /* runtime notifications delivered in any state */
    if (evt->type == RBP_GATT_EVT_NOTIFY) {
        if(a->service_changed_handle && evt->handle==a->service_changed_handle) {
            rbp_server_adapter_failed(a->server,"GATT database changed; rediscover on reconnect");return;
        }
        /* report char? */
        for (uint8_t i = 0; i < a->char_count; i++) {
            if (a->chars[i].value_handle == evt->handle) {
                DT(DT_HID,DT_DETAIL,5,evt->handle,a->chars[i].report_id,evt->len,a->chars[i].subscribed);
                if(a->chars[i].report_type!=1 || !a->chars[i].subscribed)return;
                uint64_t keys;
                bool valid;
                if(a->rc003_boot_layout && a->chars[i].report_id==1 && evt->len==8)
                    valid=hogp_decode_boot_keyboard(evt->value,evt->len,&keys);
                else valid=hogp_decode_report(&a->map,a->chars[i].report_id,evt->value,evt->len,&keys);
                if(!valid) {
                    rbp_fault_record(RBP_FAULT_ADAPTER,256,a->chars[i].report_id,evt->len);
                    DT(DT_HID,DT_ERROR,6,evt->handle,a->chars[i].report_id,evt->len,a->map.input_count);
                    DT_BLOB(DT_HID,10,evt->value,evt->len);
                    /* One malformed notification is not a failed ATT link.
                     * Release that report's keys, preserve other reports. */
                    if(a->voice_down_seen)rc003_atvv_request_stop(&a->atvv);
                    a->keys_by_report[i]=0;recompute_keys(a);return;
                }
                a->keys_by_report[i]=keys;recompute_keys(a);
                return;
            }
        }
        if (a->atvv_present && evt->handle == a->atvv_ctl_handle) {
            if(a->restoring && evt->value && evt->len==2 && evt->value[0]==0x00)a->pending_search=false;
            /* A genuine remote search request may precede CAPS on a bonded
             * reconnect. Defer this request only, never invent a held key. */
            if(a->restoring && !a->voice_ready && evt->len==1 && evt->value && evt->value[0]==0x08) {
                a->pending_search=true;return;
            }
            bool valid=rc003_atvv_on_control(&a->atvv, evt->value, evt->len);
            if(!valid && evt->len && evt->value[0]==0x0b) {
                if(a->state==ST_ATVV_PREHANDSHAKE)handle_op_failure(a);
                return;
            }
            finish_voice_ready(a);
        } else if (a->atvv_present && evt->handle == a->atvv_audio_handle) {
            rc003_atvv_on_audio(&a->atvv, evt->value, evt->len);
        } else if (evt->handle == a->bat_value_handle && evt->len >= 1) {
            if(evt->len==1 && evt->value[0]<=100)rbp_server_on_battery(a->server, evt->value[0], 0);
        }
    }
}


bool rc003_adapter_mic_stop(rc003_adapter_t *a)
{
    if(a->unicom.selected){bool active=a->unicom.active;unicom_stop(a);return active;}
    if (!a->ready || !a->atvv_tx_handle || (!a->atvv.stream_active && !a->atvv.open_pending)) return false;
    return rc003_atvv_request_stop(&a->atvv);
}

uint16_t rc003_adapter_mic_start(rc003_adapter_t *a)
{
    if(a->unicom.selected)return RBP_STATUS_VOICE_UNAVAILABLE; /* physical key only */
    if(!a->ready || !a->voice_ready || !a->atvv.caps_valid || !a->atvv_tx_handle)
        return RBP_STATUS_VOICE_UNAVAILABLE;
    if(!rbp_server_voice_wanted(a->server))return RBP_STATUS_BAD_STATE;
    if(a->atvv.stream_active || a->atvv.open_pending || a->atvv.closing || a->atvv.close_queued)
        return RBP_STATUS_BUSY;
    a->atvv.open_pending=true;a->atvv.open_queued=true;a->atvv.closing=false;
    a->atvv.open_deadline_ms=a->now_ms+ATVV_OPEN_WAIT_MS;
    return RBP_STATUS_OK; /* tick submits after the response is queued */
}
