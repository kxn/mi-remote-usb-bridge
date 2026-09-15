#include "sim_remote.h"
#include "rbp/frame.h"
#include "rbp/tlv.h"
#include "rc003_atvv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- fake GATT database ---------------- */
/* handles (fixed layout, mirrors the observed RC003 shape) */
#define H_HID_SVC        0x0010
#define H_MAP_DECL       0x0012
#define H_MAP_VAL        0x0013
#define H_INFO_DECL      0x0014
#define H_INFO_VAL       0x0015
#define H_PROTO_MODE     0x0015
#define H_HID_INFO       0x0017
#define H_KB_DECL        0x0018
#define H_KB_VAL         0x0019
#define H_KB_CCCD        0x001A
#define H_KB_REF         0x001B
#define H_CON_DECL       0x001C
#define H_CON_VAL        0x001D
#define H_CON_CCCD       0x001E
#define H_CON_REF        0x001F
#define H_GHOST_DECL     0x0020
#define H_GHOST_VAL      0x0021
#define H_GHOST_CCCD     0x0022
#define H_GHOST_REF      0x0023
#define H_BAT_SVC        0x0030
#define H_BAT_DECL       0x0031
#define H_BAT_VAL        0x0032
#define H_BAT_CCCD       0x0033
#define H_ATVV_TX_DECL   0x0051
#define H_ATVV_TX_VAL    0x0052
#define H_ATVV_CTL_DECL  0x0053
#define H_ATVV_CTL_VAL   0x0054
#define H_ATVV_CTL_CCCD  0x0055
#define H_ATVV_AUD_DECL  0x0056
#define H_ATVV_AUD_VAL   0x0057
#define H_ATVV_AUD_CCCD  0x0058

/* Report Map: keyboard (id 1, page 0x07, 8 bytes) + consumer
 * (id 2, page 0x0C, 3 bytes bit-packed).  Only the items the HOGP parser
 * consumes are encoded faithfully. */
static const uint8_t k_report_map[] = {
    0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,
    0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
    0x75,8,0x95,1,0x81,1,
    0x19,0,0x2a,0xff,0,0x15,0,0x26,0xff,0,0x75,8,0x95,6,0x81,0,0xc0,
    0x05,0x0c,0x09,1,0xa1,1,0x85,2,0x15,0,0x25,1,
    0x09,0x41,0x09,0x42,0x09,0x43,0x09,0x44,0x09,0x45,0x09,0x30,0x09,0xcf,0x09,0xe9,
    0x09,0xea,0x0a,0xa2,1,0x0a,0xb8,1,0x09,0x6a,0x09,0x6b,0x09,0x6c,0x0a,0x8a,1,0x0a,0x92,1,
    0x0a,0x94,1,0x09,0x40,0x0a,0x23,2,0x0a,0x24,2,0x0a,0x25,2,0x0a,0x26,2,0x0a,0x27,2,0x0a,0x2a,2,
    0x75,1,0x95,24,0x81,2,0xc0,
};

static const uint8_t k_atvv_base[14] = {
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A,
};

static void emit(sim_t *s, const char *line)
{
    if (s->on_event) s->on_event(s->event_user, line);
}

typedef struct { rbp_gatt_evt_t evt; uint8_t value[512]; } queued_evt_t;
static queued_evt_t event_q[64];static unsigned event_head,event_count;
static void enqueue_event(sim_t *s,const rbp_gatt_evt_t *e) {
    (void)s;if(event_count==64 || e->len>512) {fprintf(stderr,"sim GATT queue overflow\n");abort();}
    queued_evt_t *q=&event_q[(event_head+event_count++)%64];q->evt=*e;
    if(e->value && e->len) {memcpy(q->value,e->value,e->len);q->evt.value=q->value;}
}
static void drain_events(sim_t *s) {
    unsigned budget=128;
    while(event_count && budget--) {queued_evt_t q=event_q[event_head];
        if(q.evt.value)q.evt.value=q.value;
        event_head=(event_head+1)%64;event_count--;
        rc003_adapter_on_gatt(&s->adapter,&q.evt);
    }
}

/* ---------------- gatt client implementation (adapter side) ---------------- */

typedef struct {
    sim_t *sim;
} gatt_user_t;

static int g_exchange_mtu(void *user, uint16_t mtu)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = RBP_GATT_EVT_MTU_UPDATED;
    evt.mtu = mtu < 158 ? mtu : 158; /* sim ATT server caps at 158 */
    enqueue_event(s,&evt);
    return 0;
}

static int g_disc_service_by_uuid16(void *user, uint16_t uuid16)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    if (uuid16 == 0x1812) {
        evt.type = RBP_GATT_EVT_SERVICE_FOUND;
        evt.svc_start = H_HID_SVC;
        evt.svc_end = H_GHOST_REF;
    } else if (uuid16 == 0x180F) {
        evt.type = RBP_GATT_EVT_SERVICE_FOUND;
        evt.svc_start = H_BAT_SVC;
        evt.svc_end = H_BAT_CCCD;
    } else {
        evt.type = RBP_GATT_EVT_SERVICE_NOT_FOUND;
    }
    enqueue_event(s,&evt);
    if(evt.type==RBP_GATT_EVT_SERVICE_NOT_FOUND)return 0;
    evt.type = RBP_GATT_EVT_PROC_DONE;
    enqueue_event(s,&evt);
    return 0;
}

static int g_disc_service_by_uuid128(void *user, const uint8_t *uuid)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    if (memcmp(uuid, k_atvv_base, 12) == 0 && uuid[12] == 0x01 &&
        uuid[13] == 0x00 && uuid[14] == 0x5E && uuid[15] == 0xAB) {
        evt.type = RBP_GATT_EVT_SERVICE_FOUND;
        evt.svc_start = H_ATVV_TX_DECL - 1;
        evt.svc_end = H_ATVV_AUD_CCCD;
    } else {
        evt.type = RBP_GATT_EVT_SERVICE_NOT_FOUND;
    }
    enqueue_event(s,&evt);
    if(evt.type==RBP_GATT_EVT_SERVICE_NOT_FOUND)return 0;
    evt.type = RBP_GATT_EVT_PROC_DONE;
    enqueue_event(s,&evt);
    return 0;
}

/* discover characteristic(s) with the given UUID inside a range */
static int g_read_chars_by_uuid16(void *user, uint16_t start, uint16_t end, uint16_t uuid16)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));

    struct { uint16_t decl; uint16_t val; uint16_t uuid; } list[] = {
        { H_MAP_DECL, H_MAP_VAL, 0x2A4B },
        { H_INFO_DECL, H_INFO_VAL, 0x2A4A },
        { H_KB_DECL, H_KB_VAL, 0x2A4D },   /* keyboard report */
        { H_CON_DECL, H_CON_VAL, 0x2A4D }, /* consumer report */
        { H_GHOST_DECL, H_GHOST_VAL, 0x2A4D }, /* input ID absent from Report Map */
        { H_BAT_DECL, H_BAT_VAL, 0x2A19 },
    };
    bool found = false;
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (list[i].uuid != uuid16) continue;
        if (list[i].decl < start || list[i].decl > end) continue;
        uint8_t val[3] = { 0x12, (uint8_t)(list[i].val & 0xFF), (uint8_t)(list[i].val >> 8) };
        evt.type = RBP_GATT_EVT_CHARS_FOUND;
        evt.handle = list[i].decl;
        evt.uuid16 = uuid16;
        evt.value = val;
        evt.len = 3;
        evt.proc_complete = true;
        enqueue_event(s,&evt);
        found = true;
    }
    if (!found) {
        evt.type = RBP_GATT_EVT_PROC_DONE; /* WCH translates discovery ATTR_NOT_FOUND to completion */
        enqueue_event(s,&evt);
        return 0;
    }
    evt.type = RBP_GATT_EVT_PROC_DONE;
    evt.value = NULL;
    evt.len = 0;
    enqueue_event(s,&evt);
    return 0;
}

static int g_read_chars_by_uuid128(void *user, uint16_t start, uint16_t end, const uint8_t *uuid)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    uint16_t decl = 0;
    if (memcmp(uuid, k_atvv_base, 12) == 0 && uuid[14] == 0x5E &&
        uuid[15] == 0xAB) {
        if (uuid[12] == 0x02 && uuid[13] == 0x00) decl = H_ATVV_TX_DECL;
        else if (uuid[12] == 0x04 && uuid[13] == 0x00) decl = H_ATVV_CTL_DECL;
        else if (uuid[12] == 0x03 && uuid[13] == 0x00) decl = H_ATVV_AUD_DECL;
    }
    if (!decl || decl < start || decl > end) {
        evt.type = RBP_GATT_EVT_PROC_DONE;
        enqueue_event(s,&evt);
        return 0;
    }
    uint16_t val = (uint16_t)(decl + 1);
    uint8_t pv[19] = { decl==H_ATVV_TX_DECL?4:0x10, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    memcpy(pv+3,uuid,16);
    evt.type = RBP_GATT_EVT_CHARS_FOUND;
    evt.handle = decl;
    evt.value = pv;
    evt.len = sizeof pv;
    evt.proc_complete = true;
    enqueue_event(s,&evt);
    evt.type = RBP_GATT_EVT_PROC_DONE;
    enqueue_event(s,&evt);
    return 0;
}

static int g_disc_char_descs(void *user, uint16_t start, uint16_t end)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = RBP_GATT_EVT_DESC_FOUND;
    struct { uint16_t handle; uint16_t uuid; } descs[] = {
        { H_KB_CCCD, 0x2902 }, { H_KB_REF, 0x2908 },
        { H_CON_CCCD, 0x2902 }, { H_CON_REF, 0x2908 },
        { H_GHOST_CCCD, 0x2902 }, { H_GHOST_REF, 0x2908 },
        { H_BAT_CCCD, 0x2902 },
        { H_ATVV_CTL_CCCD, 0x2902 }, { H_ATVV_AUD_DECL, 0x2803 }, { H_ATVV_AUD_CCCD, 0x2902 },
    };
    for (size_t i = 0; i < sizeof(descs) / sizeof(descs[0]); i++) {
        if (descs[i].handle < start || descs[i].handle > end) continue;
        evt.desc_handle = descs[i].handle;
        evt.uuid16 = descs[i].uuid;
        enqueue_event(s,&evt);
    }
    evt.type = RBP_GATT_EVT_PROC_DONE;
    enqueue_event(s,&evt);
    return 0;
}

static int g_read_value(void *user, uint16_t handle)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = RBP_GATT_EVT_READ_RSP;
    evt.handle = handle;
    uint8_t ref[2];
    switch (handle) {
    case H_INFO_VAL: {
        static const uint8_t info[]={0x11,0x01,0,2};
        evt.value=info;evt.len=sizeof info;evt.proc_complete=true;break;
    }
    case H_MAP_VAL:
        evt.value = k_report_map;
        evt.len = sizeof(k_report_map);
        evt.proc_complete = true; /* short enough to fit one read here */
        break;
    case H_KB_REF:
        ref[0] = 0x01; ref[1] = 0x01;
        evt.value = ref;
        evt.len = 2;
        evt.proc_complete = true;
        break;
    case H_CON_REF:
        ref[0] = 0x02; ref[1] = 0x01;
        evt.value = ref;
        evt.len = 2;
        evt.proc_complete = true;
        break;
    case H_GHOST_REF:
        ref[0]=3;ref[1]=1;evt.value=ref;evt.len=2;evt.proc_complete=true;break;
    case H_BAT_VAL: {
        static uint8_t lvl;
        lvl = s->battery_level;
        evt.value = &lvl;
        evt.len = 1;
        evt.proc_complete = true;
        break;
    }
    default:
        evt.type = RBP_GATT_EVT_PROC_ERROR;
        evt.status = 0x02;
        enqueue_event(s,&evt);
        return 0;
    }
    enqueue_event(s,&evt);
    return 0;
}

static int g_read_long_value(void *user, uint16_t handle, uint16_t offset)
{
    (void)user;
    (void)handle;
    (void)offset;
    return 0; /* map fits one read in the sim */
}

static int g_write_common(void *user, uint16_t handle, const uint8_t *val, uint16_t len, bool command)
{
    gatt_user_t *u = (gatt_user_t *)user;
    sim_t *s = u->sim;
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));

    if (handle == H_GHOST_CCCD) {
        /* Regression: subscribing an unmapped input must not be necessary
         * for READY. Reject it to catch accidental ghost subscriptions. */
        return -1;
    } else if (handle == H_KB_CCCD) {
        s->subscribed_kb = val && len >= 2 && val[0] == 1;
    } else if (handle == H_CON_CCCD) {
        s->subscribed_consumer = val && len >= 2 && val[0] == 1;
    } else if (handle == H_BAT_CCCD) {
        s->subscribed_bat = val && len >= 2 && val[0] == 1;
    } else if (handle == H_ATVV_CTL_CCCD) {
        s->subscribed_ctl = val && len >= 2 && val[0] == 1;
        if (s->subscribed_ctl) emit(s, "remote:ctl_subscribed");
    } else if (handle == H_ATVV_AUD_CCCD) {
        s->subscribed_audio = val && len >= 2 && val[0] == 1;
        if (s->subscribed_audio) emit(s, "remote:audio_subscribed");
    } else if (handle == H_ATVV_TX_VAL) {
        /* remote control-plane input */
        if (len >= 1 && val[0] == 0x0A) {
            /* GET_CAPS -> CAPS_RESP (16 kHz, HTT, frame 120) */
            uint8_t resp[9] = { 0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78, 0x00, 0x00 };
            sim_remote_notify(s, H_ATVV_CTL_VAL, resp, sizeof(resp));
        } else if (len >= 2 && val[0] == 0x0D) {
            /* MIC_CLOSE -> stop after a moment */
            if (s->atvv_stream_active) {
                s->atvv_stop_ms = s->now_ms + 20; /* remote tail */
            }
        } else if (len >= 2 && val[0] == 0x0E) {
            /* MIC_EXTEND accepted silently */
        } else if (len >= 2 && val[0] == 0x0C) {
            if(!s->atvv_stream_active) {
                s->atvv_stream_active=true;s->atvv_stream_id=0;
                s->atvv_frame_counter=0;s->atvv_frames_since_sync=0;
                s->atvv_phase=0;s->atvv_phase_pred=0;s->atvv_phase_step=0;
                s->atvv_next_frame_ms=s->now_ms+5;s->atvv_stop_ms=s->now_ms+3000;
                uint8_t start[4]={4,0,2,0},sync[7]={10,2,0,0,0,0,0};
                sim_remote_notify(s,H_ATVV_CTL_VAL,start,4);
                sim_remote_notify(s,H_ATVV_CTL_VAL,sync,7);
            } else {
                uint8_t busy[3]={12,15,128};
                sim_remote_notify(s,H_ATVV_CTL_VAL,busy,3);
            }
        }
    }
    if(command)return 0;
    evt.type = RBP_GATT_EVT_WRITE_DONE;
    evt.handle = handle;
    enqueue_event(s,&evt);
    return 0;
}

static int g_write_value(void *u,uint16_t h,const uint8_t*v,uint16_t n){return g_write_common(u,h,v,n,false);}
static int g_write_command(void *u,uint16_t h,const uint8_t*v,uint16_t n){return g_write_common(u,h,v,n,true);}

static const rbp_gatt_client_t k_gatt = {
    NULL,
    g_exchange_mtu,
    g_disc_service_by_uuid16,
    g_disc_service_by_uuid128,
    g_read_chars_by_uuid16,
    g_read_chars_by_uuid128,
    g_disc_char_descs,
    g_read_value,
    g_read_long_value,
    g_write_value,
    g_write_command,
};

void sim_att_inject(sim_t *s, const uint8_t *pdu, uint16_t len)
{
    (void)s;
    (void)pdu;
    (void)len;
    /* semantic gatt layer: not used */
}

/* ---------------- radio backend mock ---------------- */

static rbp_server_t *g_server;
static sim_t *g_sim;

static uint16_t rb_start_find(void *user, uint16_t duration_ms)
{
    (void)user;
    sim_t *s = g_sim;
    if (s->remote_powered && s->remote_connectable) {
        rbp_candidate_t c;
        memset(&c, 0, sizeof(c));
        c.candidate_id = 1;
        c.support = 1;
        c.signal = 3;
        const char *name = "Xiaomi RC003";
        size_t n = strlen(name);
        memcpy(c.name, name, n);
        c.name_len = (uint8_t)n;
        rbp_server_on_scan_candidate(g_server, &c);
        (void)duration_ms;
    }
    return RBP_STATUS_OK;
}

static void rb_stop_find(void *user)
{
    (void)user;
}

static void rb_pair_begin(void *user, uint32_t candidate_id)
{
    (void)user;
    (void)candidate_id;
    sim_t *s = g_sim;
    extern uint32_t sim_allocate_peer_id(void);
    s->pair_peer_id=sim_allocate_peer_id();
    if(!s->pair_peer_id) {rbp_server_on_pair_done(g_server,RBP_STATUS_STORAGE_FAILED,false,NULL,0);return;}
    rbp_server_on_link(g_server, RBP_LINK_CONNECTING, 0, s->now_ms);
    s->pair_in_progress = true;
    s->pair_done_ms = 0;
    if (s->pairing_should_fail) {
        s->pair_result = RBP_STATUS_PAIRING_FAILED;
        s->pair_done_ms = s->now_ms + 100;
        return;
    }
    rbp_server_on_link(g_server, RBP_LINK_PAIRING, 0, s->now_ms);
    if (s->pairing_requires_passkey) {
        rbp_server_on_pair_prompt(g_server, RBP_PROMPT_ENTER_PASSKEY, 0, 30000);
        /* wait for pair_reply */
        return;
    }
    s->pair_result = RBP_STATUS_OK;
    s->pair_done_ms = s->now_ms + 100;
}

static void rb_pair_reply(void *user, bool accept, bool has_passkey, uint32_t passkey)
{
    (void)user;
    sim_t *s = g_sim;
    if (!s->pair_in_progress) return;
    if (!accept || (has_passkey && passkey != s->pairing_passkey)) {
        s->pair_result = RBP_STATUS_PAIRING_FAILED;
    } else {
        s->pair_result = RBP_STATUS_OK;
    }
    s->pair_done_ms = s->now_ms + 50;
}

static void rb_pair_cancel(void *user)
{
    (void)user;
    sim_t *s = g_sim;
    s->pair_in_progress = false;
    s->pair_initializing=false;
    s->pair_done_ms = 0;
    s->atvv_stop_ms = 0;
}

static void rb_forget_peer(void *user)
{
    (void)user;
    sim_t *s = g_sim;
    s->remote_bonded = false;
    if (s->link_up) {
        s->link_up = false;
        s->mic_on = false;
        s->atvv_stream_active = false;
    }
    rbp_server_on_forget_done(g_server, RBP_STATUS_OK, false);
}

static void rb_connect_peer(void *user)
{
    (void)user;
    sim_t *s = g_sim;
    if (!s->remote_bonded) return;
    rbp_server_on_link(g_server, RBP_LINK_CONNECTING, 0, s->now_ms);
    rbp_server_on_link(g_server, RBP_LINK_INITIALIZING, 0, s->now_ms + 50);
    s->link_up = true;
    rc003_adapter_start(&s->adapter, s->now_ms + 60);
}

static void rb_disconnect(void *user)
{
    (void)user;
    sim_t *s = g_sim;
    s->link_up = false;
    s->mic_on = false;
    s->atvv_stream_active = false;
    s->subscribed_kb = s->subscribed_consumer = s->subscribed_bat = false;
    s->subscribed_ctl = s->subscribed_audio = false;
    rc003_adapter_detach(&s->adapter);
    rbp_server_on_link(g_server, RBP_LINK_DISCONNECTED, 0, s->now_ms);
}

static uint16_t rb_voice_request_start(void *user){(void)user;return rc003_adapter_mic_start(&g_sim->adapter);}
static void rb_voice_request_stop(void *user)
{
    (void)user;
    sim_t *s = g_sim;
    rc003_adapter_mic_stop(&s->adapter);
    if (s->atvv_stream_active && s->subscribed_ctl) {
        uint8_t stop[2] = { 0x00, 0x00 }; /* AUDIO_STOP reason 0 (mic close) */
        sim_remote_notify(s, H_ATVV_CTL_VAL, stop, sizeof(stop));
        s->atvv_stream_active = false;
        s->mic_on = false;
    }
}

/* ---------------- notifications ---------------- */

void sim_remote_notify(sim_t *s, uint16_t value_handle, const uint8_t *data, uint16_t len)
{
    rbp_gatt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = RBP_GATT_EVT_NOTIFY;
    evt.handle = value_handle;
    evt.value = data;
    evt.len = len;
    enqueue_event(s,&evt);
}

/* ---------------- voice generation ---------------- */

/* tiny IMA encoder for the tone generator (mirrors decoder rules) */
static const int16_t k_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t k_idx_tbl[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

static uint8_t enc_nibble(int16_t *pred, uint8_t *sidx, int16_t sample)
{
    int32_t diff = sample - *pred;
    int sign = diff < 0;
    if (sign) diff = -diff;
    uint8_t nib = 0;
    int32_t step = k_steps[*sidx];
    if (diff >= step) { nib = 4; diff -= step; }
    if (diff >= step >> 1) { nib |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) nib |= 1;
    nib = (uint8_t)(nib | (sign ? 8 : 0));
    int32_t d = step >> 3;
    if (nib & 4) d += step;
    if (nib & 2) d += step >> 1;
    if (nib & 1) d += step >> 2;
    int32_t p = *pred + (sign ? -d : d);
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    *pred = (int16_t)p;
    int idx = *sidx + k_idx_tbl[nib & 7];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    *sidx = (uint8_t)idx;
    return nib;
}

/* frame the tone into 120-byte ADPCM frames with a 3-byte report-style prefix
 * of zeros; includes periodic AUDIO_SYNC via the control plane */
static void atvv_send_frame(sim_t *s)
{
    uint8_t frame[120];
    memset(frame, 0, sizeof(frame));
    int16_t pred = s->atvv_phase_pred;
    uint8_t sidx = s->atvv_phase_step;
    for (int i = 0; i < 240; i += 2) {
        s->atvv_phase += 400; /* ~244 Hz at 16 kHz */
        int16_t sample = (int16_t)(s->atvv_phase >> 4);
        uint8_t hi = enc_nibble(&pred, &sidx, sample);
        s->atvv_phase += 400;
        s->atvv_phase &= 0x7FFF;
        int32_t x = s->atvv_phase;
        x -= (x >> 8) * 3; /* pseudo-sine */
        int16_t sample2 = (int16_t)(x >> 4);
        uint8_t lo = enc_nibble(&pred, &sidx, sample2);
        frame[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    s->atvv_phase_pred = pred;
    s->atvv_phase_step = sidx;
    sim_remote_notify(s, H_ATVV_AUD_VAL, frame, sizeof(frame));
}

/* ---------------- main tick ---------------- */

void sim_tick(sim_t *s, uint32_t now_ms)
{
    s->now_ms = now_ms;

    /* pairing completion */
    if (s->pair_in_progress && s->pair_done_ms && now_ms >= s->pair_done_ms) {
        s->pair_in_progress = false;
        s->pair_done_ms = 0;
        if (s->pair_result == RBP_STATUS_OK) {
            s->remote_bonded = true;
            s->pair_initializing=true;
            s->link_up = true;
            rbp_server_on_link(g_server,RBP_LINK_INITIALIZING,0,now_ms);
            rc003_adapter_start(&s->adapter, now_ms);
        } else {
            rbp_server_on_pair_done(g_server, s->pair_result, false, NULL, 0);
            s->link_up = false;
        }
    }

    /* battery notification */
    if (s->subscribed_bat && s->battery_pending) {
        s->battery_pending = false;
        sim_remote_notify(s, H_BAT_VAL, &s->battery_level, 1);
    }

    /* ATVV stream */
    if (s->atvv_stream_active && s->subscribed_audio) {
        while (now_ms >= s->atvv_next_frame_ms) {
            if (s->atvv_frames_since_sync == 0 && s->atvv_send_sync && !s->atvv_drop_sync) {
                uint8_t sync[7];
                sync[0] = 0x0A;
                sync[1] = 0x02;
                sync[2] = (uint8_t)(s->atvv_frame_counter >> 8);
                sync[3] = (uint8_t)(s->atvv_frame_counter & 0xFF);
                sync[4] = (uint8_t)(s->atvv_phase_pred >> 8);
                sync[5] = (uint8_t)(s->atvv_phase_pred & 0xFF);
                sync[6] = s->atvv_phase_step;
                sim_remote_notify(s, H_ATVV_CTL_VAL, sync, sizeof(sync));
            }
            atvv_send_frame(s);
            s->atvv_frame_counter++;
            s->atvv_frames_since_sync++;
            if (s->atvv_frames_since_sync >= 20) s->atvv_frames_since_sync = 0;
            s->atvv_next_frame_ms += 15; /* 120 bytes per 15 ms = 16 kbit*4 */
            if (s->atvv_stop_ms && now_ms >= s->atvv_stop_ms) {
                uint8_t stop[2] = { 0x00, 0x02 }; /* HTT release */
                sim_remote_notify(s, H_ATVV_CTL_VAL, stop, sizeof(stop));
                s->atvv_stream_active = false;
                s->mic_on = false;
                s->atvv_stop_ms = 0;
                emit(s, "remote:atvv_stopped");
                break;
            }
        }
    }

    /* link drop injection */
    if (s->drop_link_ms && now_ms >= s->drop_link_ms) {
        s->drop_link_ms = 0;
        rb_disconnect(NULL);
    }

    s->adapter.now_ms=now_ms;drain_events(s);
    rc003_adapter_tick(&s->adapter, now_ms);
    drain_events(s);
    if(s->pair_initializing && s->adapter.ready) {
        rbp_peer_record_t rec={0};rec.peer_id=s->pair_peer_id;strcpy(rec.name,"Xiaomi RC003");rec.auto_reconnect=true;
        s->pair_initializing=false;rbp_server_on_pair_done(g_server,RBP_STATUS_OK,true,&rec,0);
    }
    if(!s->link_up && s->remote_bonded && s->remote_powered && s->remote_connectable && rbp_server_should_reconnect(g_server))rb_connect_peer(NULL);
}

/* ---------------- commands ---------------- */

static uint16_t key_code(const char *name)
{
    if (!strcmp(name, "up")) return 0x52;
    if (!strcmp(name, "down")) return 0x51;
    if (!strcmp(name, "left")) return 0x50;
    if (!strcmp(name, "right")) return 0x4F;
    if (!strcmp(name, "ok")) return 0x28;
    if (!strcmp(name, "back")) return 0xF1;
    if (!strcmp(name, "home")) return 0x4A;
    if (!strcmp(name, "menu")) return 0x65;
    if (!strcmp(name, "power")) return 0x66;
    if (!strcmp(name, "volup")) return 0x80;
    if (!strcmp(name, "voldown")) return 0x81;
    if (!strcmp(name, "voice")) return 0x3E;
    if (!strcmp(name, "tv")) return 0x35;
    return 0;
}

bool sim_command(sim_t *s, const char *line)
{
    char cmd[32] = { 0 };
    char arg[32] = { 0 };
    if (sscanf(line, "%31s %31s", cmd, arg) < 1) return false;

    if (!strcmp(cmd, "press") || !strcmp(cmd, "release")) {
        uint16_t kc = key_code(arg);
        if (!kc) return false;
        uint8_t rep[8];
        memset(rep, 0, sizeof(rep));
        /* HOGP Report characteristic excludes report ID. */
        if (!strcmp(cmd, "press")) {
            rep[2] = (uint8_t)kc;
            s->kb_report_state = kc;
        } else {
            s->kb_report_state = 0;
        }
        if (s->subscribed_kb) sim_remote_notify(s, H_KB_VAL, rep, sizeof(rep));
        return true;
    }
    if (!strcmp(cmd, "consumer")) {
        /* bit-packed consumer report: arg = decimal bit */
        uint32_t bit = (uint32_t)atoi(arg);
        uint8_t rep[3];
        memset(rep, 0, sizeof(rep));
        if (bit < 24) {
            s->consumer_state[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        memcpy(rep, s->consumer_state, 3);
        if (s->subscribed_consumer) sim_remote_notify(s, H_CON_VAL, rep, sizeof(rep));
        return true;
    }
    if (!strcmp(cmd, "consumer_clear")) {
        memset(s->consumer_state, 0, sizeof(s->consumer_state));
        uint8_t rep[3] = { 0, 0, 0 };
        if (s->subscribed_consumer) sim_remote_notify(s, H_CON_VAL, rep, sizeof(rep));
        return true;
    }
    if (!strcmp(cmd, "mic_on")) {
        if (!s->atvv_stream_active) {
            s->atvv_stream_active = true;
            s->atvv_stream_id = (uint8_t)((s->atvv_stream_id % 0x80) + 1);
            s->atvv_frame_counter = 0;
            s->atvv_frames_since_sync = 0;
            s->atvv_phase = 0;
            s->atvv_phase_pred = 0;
            s->atvv_phase_step = 0;
            s->atvv_next_frame_ms = s->now_ms + 5;
            /* HTT model: a press speaks a short tone then releases itself;
             * an explicit client VOICE_STOP usually beats it */
            s->atvv_stop_ms = s->now_ms + 3000;
            if (s->subscribed_ctl) {
                uint8_t start[4] = { 0x04, 0x03, 0x02, s->atvv_stream_id };
                sim_remote_notify(s, H_ATVV_CTL_VAL, start, sizeof(start));
                uint8_t initial_sync[7]={0x0a,2,0,0,0,0,0};
                sim_remote_notify(s,H_ATVV_CTL_VAL,initial_sync,sizeof initial_sync);
            }
            emit(s, "remote:atvv_started");
        }
        return true;
    }
    if (!strcmp(cmd, "mic_off")) {
        if (s->atvv_stream_active) {
            s->atvv_stop_ms = s->now_ms + 10;
        }
        return true;
    }
    if (!strcmp(cmd, "battery")) {
        s->battery_level = (uint8_t)atoi(arg);
        s->battery_pending = s->subscribed_bat;
        return true;
    }
    if (!strcmp(cmd, "drop_link")) {
        s->drop_link_ms = s->now_ms + 10;
        return true;
    }
    if (!strcmp(cmd, "pair_fail")) {
        s->pairing_should_fail = true;
        return true;
    }
    if (!strcmp(cmd, "pair_ok")) {
        s->pairing_should_fail = false;
        s->pairing_requires_passkey = false;
        return true;
    }
    if (!strcmp(cmd, "pair_passkey")) {
        s->pairing_requires_passkey = true;
        s->pairing_passkey = (uint32_t)atoi(arg);
        return true;
    }
    if (!strcmp(cmd, "atvv_no_sync")) {
        s->atvv_send_sync = !strcmp(arg, "on");
        return true;
    }
    return false;
}

/* ---------------- init ---------------- */

static const rbp_radio_backend_t k_radio_backend = {
    NULL,
    rb_start_find,
    rb_stop_find,
    rb_pair_begin,
    rb_pair_reply,
    rb_pair_cancel,
    rb_forget_peer,
    rb_connect_peer,
    rb_disconnect,
    rb_voice_request_stop, rb_voice_request_start,
};

const rbp_radio_backend_t *sim_radio_backend(void)
{
    return &k_radio_backend;
}

void sim_init(sim_t *s, rbp_server_t *server)
{
    memset(s, 0, sizeof(*s));
    g_server = server;
    g_sim = s;
    s->server = server;
    s->remote_powered = true;
    s->remote_connectable = true;
    s->battery_level = 80;
    s->atvv_send_sync = true;
    s->atvv_sample_rate = 16000;

    static gatt_user_t gu; /* static: lifetime = process */
    gu.sim = s;
    rc003_adapter_init(&s->adapter, &k_gatt, &gu, server);
}
