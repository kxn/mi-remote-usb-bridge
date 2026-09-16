#include "../product/faults.h"
#include "wch_central.h"
#include "../debug/trace.h"
#include "wch_gatt.h"
#include "board.h"
#include "CH58xBLE_LIB.h"
#include <string.h>
static uint8_t gap_result(uint16_t stage,uint8_t status,uint32_t context) {
    if(status!=SUCCESS && !(stage==11 && status==0xfe))
        rbp_fault_record(RBP_FAULT_GAP,stage,status,context);
    return status;
}
uint8_t wch_central_task_id;
extern uint32_t wch_now_ms;
#define WC_START_EVT 1u
#define WC_POLL_EVT 2u
#define MAX_CAND 8
static rbp_server_t *g_srv;
static rc003_adapter_t *g_adapter;
static uint16_t conn_handle=GAP_CONNHANDLE_INIT;
enum { WC_IDLE, WC_SCAN, WC_CONNECTING, WC_CONNECTED, WC_RESOLVE,
       WC_SCAN_STOPPING, WC_CONNECT_CANCEL, WC_DISCONNECTING };
enum { NEXT_NONE, NEXT_PAIR, NEXT_RECONNECT };
#define CANCEL_TIMEOUT_MS 5000u
static uint8_t wc_state,backoff;
static uint8_t next_link;
static bool radio_faulted, security_blocked, passkey_pending;
/* Eight task-context flags share a byte; no ISR accesses these fields. */
static struct {
    bool radio_ready:1, pair_pending:1, bond_ready:1, adapter_started:1;
    bool recovering:1, delete_report:1, erase_requested:1;
} central_flags;
#define radio_ready central_flags.radio_ready
#define pair_pending central_flags.pair_pending
#define bond_ready central_flags.bond_ready
#define adapter_started central_flags.adapter_started
#define recovering central_flags.recovering
#define delete_report central_flags.delete_report
#define erase_requested central_flags.erase_requested
static uint32_t deadline,reconnect_at,recovery_deadline;
static uint8_t target_addr[6],target_type,identity[6],identity_type,peer_irk[16];
static bool have_irk;
#ifdef RBP_DEBUG
/* Public SDK diagnostics only. Never put key material in the trace. */
static void trace_address(uint16_t code,const uint8_t *addr,uint8_t type,uint32_t c,uint32_t d) {
    uint32_t lo=(uint32_t)addr[0]|((uint32_t)addr[1]<<8)|((uint32_t)addr[2]<<16)|((uint32_t)addr[3]<<24);
    DT(DT_BLE,DT_INFO,code,lo,(uint32_t)addr[4]|((uint32_t)addr[5]<<8)|((uint32_t)type<<16),c,d);
}
static void trace_local_identity(void) {
    uint8_t addr[6]={0},live[16]={0},saved[16]={0};
    uint8_t as=gap_result(8,GAPRole_GetParameter(GAPROLE_BD_ADDR,addr),GAPROLE_BD_ADDR);
    uint8_t ls=gap_result(8,GAPRole_GetParameter(GAPROLE_IRK,live),GAPROLE_IRK);
    uint8_t ns=gap_result(12,tmos_snv_read(BLE_NVID_IRK,sizeof saved,saved),BLE_NVID_IRK);
    trace_address(13,addr,ADDRTYPE_PUBLIC,as,0);
    bool nonzero=false;for(unsigned i=0;i<sizeof live;i++)nonzero|=live[i]!=0;
    DT(DT_BLE,DT_INFO,14,ls,ns,ls==SUCCESS && ns==SUCCESS && !memcmp(live,saved,sizeof live),nonzero);
    memset(live,0,sizeof live);memset(saved,0,sizeof saved);
}
static void trace_direct_target(const gapDirectDeviceInfoEvent_t *info) {
    uint8_t addr[6]={0},irk[16]={0};
    uint8_t match=255,status=gap_result(8,GAPRole_GetParameter(GAPROLE_BD_ADDR,addr),GAPROLE_BD_ADDR);
    if(info->directAddrType==ADDRTYPE_PUBLIC && status==SUCCESS)
        match=!memcmp(addr,info->directAddr,6);
    else if(info->directAddrType!=ADDRTYPE_PUBLIC && (info->directAddr[5]&0xc0)==0x40) {
        status=gap_result(8,GAPRole_GetParameter(GAPROLE_IRK,irk),GAPROLE_IRK);
        if(status==SUCCESS)match=GAP_ResolvePrivateAddr(irk,(uint8_t*)info->directAddr)==SUCCESS;
    }
    trace_address(15,info->directAddr,info->directAddrType,match,(uint8_t)info->rssi);
    memset(irk,0,sizeof irk);
}
#endif
/* Product already retains public candidate names; radio retains addresses only. */
typedef struct {uint8_t addr[6],type;} candidate_t;
static candidate_t candidates[MAX_CAND];static uint8_t candidate_count;
static void reconnect_schedule(void) {
    static const uint16_t delay[]={1000,2000,5000};
    uint8_t step=backoff;
    reconnect_at=wch_now_ms+delay[step];
    backoff=step<2?step+1:2;
}
static void rb_disconnect(void *user);
static void rb_connect_peer(void *user);
static void set_state(uint8_t state,uint32_t timeout) {
    DT(DT_BLE,DT_INFO,8,wc_state,state,next_link,timeout);
    wc_state=state;deadline=wch_now_ms+timeout;
}
static void radio_fault(const char *why,uint8_t status) {
    (void)status; /* still used by the opt-in diagnostic build */
    DT(DT_BLE,DT_ERROR,9,wc_state,status,conn_handle,next_link);
    next_link=NEXT_NONE;
    if(radio_faulted)return;
    radio_faulted=true;wch_gatt_quiesce();rc003_adapter_detach(g_adapter);
    rbp_server_on_link_message(g_srv,why);
    rbp_server_on_link(g_srv,RBP_LINK_ERROR,0,wch_now_ms);
}
static void disconnected(void) {
    rbp_server_on_link(g_srv,radio_faulted||security_blocked?RBP_LINK_ERROR:
        board_storage_state()==1?RBP_LINK_DISCONNECTED:RBP_LINK_UNBOUND,0,wch_now_ms);
    reconnect_schedule();
}
static void stop_scan(void) {
    if(wc_state==WC_SCAN_STOPPING)return;
    set_state(WC_SCAN_STOPPING,CANCEL_TIMEOUT_MS);
    uint8_t status=gap_result(4,GAPRole_CentralCancelDiscovery(),0);
    DT(DT_BLE,DT_INFO,11,1,status,0,0);
    /* Even a rejected cancel can race the already queued natural completion.
     * Retain ownership until that event or the cancellation deadline. */
    if(status!=SUCCESS)rbp_server_on_link_message(g_srv,"scan cancel rejected; awaiting completion");
}
static void fail_pair(uint16_t status) {
    if(!pair_pending)return;
    DT(DT_STORE,DT_ERROR,7,status,board_storage_state(),bond_ready,g_adapter->ready);
    pair_pending=false;recovering=board_storage_state()==3;delete_report=false;
    erase_requested=false;recovery_deadline=0;
    rbp_server_on_pair_done(g_srv,status,false,NULL,0);
    rb_disconnect(NULL);
}
/* Keep actionable security failures in the normal product state, not only
 * in optional traces. This contains status/phase only, never key material. */
static void fail_pair_sdk(const char *phase,uint8_t code) {
    if(!pair_pending)return;
    char message[64];static const char hex[]="0123456789ABCDEF";
    size_t n=strlen(phase);if(n>40)n=40;
    memcpy(message,phase,n);memcpy(message+n," (SDK 0x",8);n+=8;
    message[n++]=hex[code>>4];message[n++]=hex[code&15];message[n++]=')';message[n]=0;
    fail_pair(RBP_STATUS_PAIRING_FAILED);
    rbp_server_on_link_message(g_srv,message);
    rbp_server_on_link(g_srv,RBP_LINK_ERROR,0,wch_now_ms);
}
static bool read_identity(void) {
    gapBondRec_t rec;memset(&rec,0xff,sizeof rec);
    uint8_t count=255;
    uint8_t cs=gap_result(2,GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT,&count),GAPBOND_BOND_COUNT);
    uint8_t rs=gap_result(12,tmos_snv_read(mainRecordNvID(0),sizeof rec,&rec),mainRecordNvID(0));
    DT(DT_STORE,DT_INFO,4,cs,count,rs,rec.publicAddrType);
    /* This record contains addresses/flags only, never LTK/IRK material. */
    if(rs==SUCCESS)DT_BLOB(DT_STORE,10,(const uint8_t*)&rec,sizeof rec);
    if(cs!=SUCCESS || count!=1 || rs!=SUCCESS)return false;
    bool zero=true,ff=true;
    for(unsigned i=0;i<6;i++){if(rec.publicAddr[i])zero=false;if(rec.publicAddr[i]!=255)ff=false;}
    if(rec.publicAddrType>1 || zero || ff) {
        DT(DT_STORE,DT_ERROR,5,1,rec.publicAddrType,zero,ff);return false;
    }
    memcpy(identity,rec.publicAddr,6);identity_type=rec.publicAddrType;
    memset(peer_irk,0,sizeof peer_irk);have_irk=false;
    uint8_t ir=gap_result(12,tmos_snv_read(devIRKNvID(0),sizeof peer_irk,peer_irk),devIRKNvID(0));
    if(ir==SUCCESS) {
        zero=true;ff=true;for(unsigned i=0;i<16;i++){if(peer_irk[i])zero=false;if(peer_irk[i]!=255)ff=false;}
        have_irk=!zero && !ff;
    }
    DT(DT_STORE,DT_INFO,6,ir,have_irk,identity_type,rec.stateFlags);
    return true;
}

static void finish_pair(void) {
    if(!read_identity()) {fail_pair(RBP_STATUS_STORAGE_FAILED);return;}
    rbp_peer_record_t rec;memset(&rec,0,sizeof rec);rec.peer_id=board_peer_counter();rec.auto_reconnect=true;
    strcpy(rec.name,"BLE Remote");
    board_addr_stage(identity,identity_type);
    board_hid_flags_stage(g_adapter->hid_info_valid?g_adapter->hid_flags:255);
    pair_pending=false;
    rbp_server_on_pair_done(g_srv,RBP_STATUS_OK,true,&rec,0);
    DT(DT_STORE,DT_INFO,8,board_storage_state(),rec.peer_id,0,0);
    if(board_storage_state()!=1) {
        recovering=true;delete_report=false;erase_requested=false;recovery_deadline=0;
        rb_disconnect(NULL);
    }
}
static void begin_link(const uint8_t *addr,uint8_t type) {
    if(wc_state!=WC_IDLE || radio_faulted || recovering)return;
    if(addr!=target_addr)memcpy(target_addr,addr,6);
    target_type=type;next_link=NEXT_NONE;
    /* INITIATE makes the pinned SDK start encryption for a known bond.
     * Bond failure is configured to terminate, never erase/re-pair. */
    uint8_t mode=GAPBOND_PAIRING_MODE_INITIATE;
    uint8_t status=gap_result(3,GAPBondMgr_SetParameter(GAPBOND_CENT_PAIRING_MODE,1,&mode),GAPBOND_CENT_PAIRING_MODE);
    if(status!=SUCCESS){radio_fault("security setup failed",status);fail_pair_sdk("security setup failed",status);return;}
    set_state(WC_CONNECTING,20000);adapter_started=false;bond_ready=false;passkey_pending=false;
    rbp_server_on_link(g_srv,RBP_LINK_CONNECTING,0,wch_now_ms);
    status=gap_result(5,GAPRole_CentralEstablishLink(FALSE,FALSE,type,target_addr),0);
    DT(DT_BLE,DT_INFO,11,2,status,type,0);
    if(status!=SUCCESS) {
        set_state(WC_IDLE,0);if(pair_pending)fail_pair_sdk("connection request failed",status);
        else rbp_server_on_link(g_srv,RBP_LINK_DISCONNECTED,0,wch_now_ms);
        reconnect_schedule();
    }
}
static void resolve_advertisement(const uint8_t *addr,uint8_t type,uint8_t event_type) {
    if(wc_state!=WC_RESOLVE || radio_faulted)return;
    if(event_type!=GAP_ADRPT_ADV_IND && event_type!=GAP_ADRPT_ADV_DIRECT_IND)return;
    bool match=(type==identity_type && !memcmp(addr,identity,6));
    uint8_t resolved=255;
    if(!match && have_irk && (type==ADDRTYPE_STATIC || type==ADDRTYPE_PRIVATE_RESOLVE) && (addr[5]&0xc0)==0x40) {
        resolved=GAP_ResolvePrivateAddr(peer_irk,(uint8_t*)addr);match=resolved==SUCCESS;
    }
    uint32_t lo=(uint32_t)addr[0]|((uint32_t)addr[1]<<8)|((uint32_t)addr[2]<<16)|((uint32_t)addr[3]<<24);
    (void)lo;(void)event_type;
    DT(DT_BLE,DT_DETAIL,7,lo,(uint32_t)addr[4]|((uint32_t)addr[5]<<8)|((uint32_t)type<<16)|((uint32_t)event_type<<24),match,resolved);
    if(match) {
        memcpy(target_addr,addr,6);target_type=type;next_link=NEXT_RECONNECT;
        stop_scan();
    }
}
/* Validate advertising UTF-8 and copy only whole characters. Invalid names are hints only. */
static uint8_t candidate_name(char *out,const uint8_t *p,unsigned n) {
    unsigned i=0,w=0;
    while(i<n) {
        uint32_t cp=p[i];unsigned k=1;
        if(cp>=0xc2 && cp<=0xdf){cp&=31;k=2;}
        else if(cp>=0xe0 && cp<=0xef){cp&=15;k=3;}
        else if(cp>=0xf0 && cp<=0xf4){cp&=7;k=4;}
        else if(cp>=0x80 || cp<0x20)return 0;
        if(i+k>n)return 0;
        for(unsigned j=1;j<k;j++){if((p[i+j]&0xc0)!=0x80)return 0;cp=(cp<<6)|(p[i+j]&63);}
        if((k==3&&cp<0x800)||(k==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
        if(i+k<sizeof(((rbp_candidate_t*)0)->name)){memcpy(out+w,p+i,k);w+=k;}
        i+=k;
    }
    out[w]=0;return w;
}
static void on_device(gapDeviceInfoEvent_t *info) {
    if(wc_state==WC_RESOLVE) {resolve_advertisement(info->addr,info->addrType,info->eventType);return;}
    if(wc_state!=WC_SCAN)return;
    const uint8_t *name=NULL;uint8_t nlen=0;bool hid=false,voice_hint=false;
    for(unsigned pos=0;pos<info->dataLen;) {
        unsigned n=info->pEvtData[pos];if(!n)break;if(pos+n+1>info->dataLen)return;
        const uint8_t *d=info->pEvtData+pos+2;uint8_t type=info->pEvtData[pos+1];
        if(type==8 || type==9) {name=d;nlen=n-1;}
        if(type==2 || type==3)for(unsigned j=0;j+1<n-1;j+=2) {
            if(d[j]==0x12 && d[j+1]==0x18)hid=true;
            if(d[j]==0x00 && d[j+1]==0xfd)voice_hint=true;
        }
        pos+=n+1;
    }
    uint8_t support=rc003_adapter_match((const char*)name,nlen,hid);
    DT(DT_BLE,DT_DETAIL,2,support,nlen,hid,(uint8_t)info->rssi);
    if(nlen)DT_BLOB(DT_BLE,10,name,nlen);
    unsigned i;for(i=0;i<candidate_count;i++)if(candidates[i].type==info->addrType && !memcmp(candidates[i].addr,info->addr,6))break;
    bool fresh=i==candidate_count;
    rbp_candidate_t c;memset(&c,0,sizeof c);
    if(name)c.name_len=candidate_name(c.name,name,nlen);
    /* GATT services need not be listed in advertisements. A valid local name
     * (including one arriving only in SCAN_RSP) admits an unverified candidate;
     * only post-connect profile validation establishes compatibility. */
    bool discoverable=info->eventType==GAP_ADRPT_ADV_IND || info->eventType==GAP_ADRPT_SCAN_RSP;
    if(fresh && (!discoverable || (!support && !hid && !voice_hint && !c.name_len)))return;
    if(i==candidate_count){if(i==MAX_CAND)return;candidate_count++;memset(&candidates[i],0,sizeof candidates[i]);memcpy(candidates[i].addr,info->addr,6);candidates[i].type=info->addrType;}
    c.candidate_id=i+1;c.support=support;
    c.signal=info->rssi>-60?3:info->rssi>-75?2:1;
    if(!c.name_len && fresh){strcpy(c.name,support?"Xiaomi RC003":hid?"BLE HID candidate":"BLE voice candidate");c.name_len=strlen(c.name);}
    if(!c.name_len)c.name[0]=0;
    rbp_server_on_scan_candidate(g_srv,&c);
}
static void wc_rssi_cb(uint16_t h,int8_t r){(void)h;(void)r;}
static void wc_data_len_cb(uint16_t h,uint16_t t,uint16_t r){(void)h;(void)t;(void)r;DT(DT_BLE,DT_INFO,5,h,t,r,0);}
static void wc_event_cb(gapRoleEvent_t *e) {
    DT(DT_BLE,DT_INFO,1,e->gap.opcode,e->gap.hdr.status,wc_state,0);
    if(e->gap.hdr.status!=SUCCESS && e->gap.hdr.status!=bleGAPUserCanceled)
        rbp_fault_record(RBP_FAULT_GAP,64,e->gap.hdr.status,e->gap.opcode);
    switch(e->gap.opcode) {
    case GAP_DEVICE_INIT_DONE_EVENT:
        radio_ready=e->gap.hdr.status==SUCCESS;
        if(!radio_ready)radio_fault("BLE initialization failed",e->gap.hdr.status);
        else {
            /* Fixed public central identity. Peer RPA resolution is separate
             * and is performed by resolve_advertisement with the saved IRK. */
            uint8_t status=gap_result(10,GAP_ConfigDeviceAddr(ADDRTYPE_PUBLIC,NULL),0);
            DT(DT_BLE,DT_INFO,11,8,status,ADDRTYPE_PUBLIC,0);
            if(status!=SUCCESS){radio_ready=false;radio_fault("local address configuration failed",status);}
        }
        break;
    case GAP_LINK_ESTABLISHED_EVENT:
#ifdef RBP_DEBUG
        if(e->gap.hdr.status==SUCCESS)
            DT(DT_BLE,DT_INFO,16,e->linkCmpl.connectionHandle,e->linkCmpl.connInterval,e->linkCmpl.connLatency,e->linkCmpl.connTimeout);
#endif
        if(e->gap.hdr.status==SUCCESS) {
            if(conn_handle!=GAP_CONNHANDLE_INIT)return; /* duplicate cannot replace owner */
            bool wanted=wc_state==WC_CONNECTING && !recovering && !radio_faulted && !security_blocked;
            conn_handle=e->linkCmpl.connectionHandle;
            /* Product pairing has its own 60s total deadline. Do not cut a
             * 30s passkey prompt short with the old 20s connection timer. */
            set_state(WC_CONNECTED,pair_pending?60000:30000);adapter_started=false;
            if(!wanted){rb_disconnect(NULL);break;}
            wch_gatt_set_conn(conn_handle);
            if(!pair_pending) {
                /* LinkEst has already requested encryption using the saved LTK.
                 * Disallow subsequent new authentication on a restore intent. */
                uint8_t no_pair=GAPBOND_PAIRING_MODE_NO_PAIRING;
                uint8_t status=gap_result(3,GAPBondMgr_SetParameter(GAPBOND_CENT_PAIRING_MODE,1,&no_pair),GAPBOND_CENT_PAIRING_MODE);
                if(status!=SUCCESS){radio_fault("restore security policy failed",status);rb_disconnect(NULL);break;}
            }
            rbp_server_on_link(g_srv,RBP_LINK_INITIALIZING,0,wch_now_ms);
        } else if(wc_state==WC_CONNECTING || wc_state==WC_CONNECT_CANCEL) {
            set_state(WC_IDLE,0);fail_pair_sdk("connection completion failed",e->gap.hdr.status);disconnected();
        }
        break;
    case GAP_LINK_TERMINATED_EVENT:
        if(conn_handle==GAP_CONNHANDLE_INIT || e->linkTerminate.connectionHandle!=conn_handle)return;
        DT(DT_BLE,DT_INFO,3,e->linkTerminate.reason,conn_handle,0,0);
        rbp_fault_record(RBP_FAULT_GAP,65,e->linkTerminate.reason,0);
        conn_handle=GAP_CONNHANDLE_INIT;wch_gatt_set_conn(conn_handle);set_state(WC_IDLE,0);adapter_started=false;passkey_pending=false;
        rc003_adapter_detach(g_adapter);fail_pair(RBP_STATUS_LINK_LOST);
        disconnected();break;
    case GAP_LINK_PARAM_UPDATE_EVENT:
        if(e->linkUpdate.status!=SUCCESS)rbp_fault_record(RBP_FAULT_GAP,66,e->linkUpdate.status,0);
        DT(DT_BLE,DT_INFO,17,e->linkUpdate.connectionHandle,e->linkUpdate.connInterval,
           e->linkUpdate.connLatency,e->linkUpdate.connTimeout);
        DT(DT_BLE,DT_INFO,18,e->gap.hdr.status,e->linkUpdate.status,0,0);
        break;
    case GAP_DEVICE_INFO_EVENT:on_device(&e->deviceInfo);break;
    case GAP_DIRECT_DEVICE_INFO_EVENT:
#ifdef RBP_DEBUG
        if(wc_state==WC_RESOLVE)trace_direct_target(&e->deviceDirectInfo);
#endif
        resolve_advertisement(e->deviceDirectInfo.addr,e->deviceDirectInfo.addrType,e->deviceDirectInfo.eventType);break;
    case GAP_DEVICE_DISCOVERY_EVENT:
        if(wc_state==WC_SCAN_STOPPING) {
            uint8_t next=next_link;next_link=NEXT_NONE;set_state(WC_IDLE,0);
            /* WCH GAP_DeviceDiscoveryCancel emits bleGAPUserCanceled (0x30),
             * while a natural completion emits SUCCESS. Both end ownership. */
            if(e->gap.hdr.status!=SUCCESS && e->gap.hdr.status!=bleGAPUserCanceled){radio_fault("scan completion failed",e->gap.hdr.status);fail_pair_sdk("scan completion failed",e->gap.hdr.status);}
            else if(next && !radio_faulted && !recovering)begin_link(target_addr,target_type);
            else disconnected();
        } else if(wc_state==WC_SCAN) {
            set_state(WC_IDLE,0);rbp_server_on_scan_done(g_srv,RBP_FIND_DONE_EXPIRED,wch_now_ms);
        } else if(wc_state==WC_RESOLVE){set_state(WC_IDLE,0);disconnected();}
        break;
    }
}
static gapCentralRoleCB_t wc_role_cb={wc_rssi_cb,wc_event_cb,wc_data_len_cb};
static uint32_t display_passkey;
/* Pinned WCH archive: SM_PasskeyUpdate returns 0xFE when the selected
 * authentication method does not take a passkey. GAPBondMgr_PasscodeRsp
 * explicitly exempts both SUCCESS and 0xFE from GAP_TerminateAuth.
 * Preserve that policy here; final pairing callbacks still decide success.
 * This is NOT blePending (0x16), nor a generic exception for other APIs. */
static bool passcode_response_failed(uint8_t status) {
    return status!=SUCCESS && status!=0xfe;
}
static void wc_passcode_cb(uint8_t *addr,uint16_t h,uint8_t inputs,uint8_t outputs) {
    (void)addr;if(h!=conn_handle || wc_state!=WC_CONNECTED || !pair_pending)return;
    if(inputs){passkey_pending=true;rbp_server_on_pair_prompt(g_srv,RBP_PROMPT_ENTER_PASSKEY,0,30000);}
    else if(outputs){
        display_passkey=tmos_rand()%1000000;
        rbp_server_on_pair_prompt(g_srv,RBP_PROMPT_DISPLAY_PASSKEY,display_passkey,30000);
        uint8_t status=gap_result(11,GAPBondMgr_PasscodeRsp(h,SUCCESS,display_passkey),0);
        if(passcode_response_failed(status))fail_pair_sdk("display passkey response failed",status);
    }
    else {uint8_t status=gap_result(11,GAPBondMgr_PasscodeRsp(h,SUCCESS,0),0);
        if(passcode_response_failed(status))fail_pair_sdk("just works response failed",status);}
}
static void wc_pair_state_cb(uint16_t h,uint8_t state,uint8_t status) {
    DT(DT_BLE,DT_INFO,4,h,state,status,0);
    if(status!=SUCCESS)rbp_fault_record(RBP_FAULT_GAP,32+state,status,0);
    /* The Role can invoke a security callback before its link callback. An
     * unrequested new pairing must cancel the pending restore as well. */
    if(state==GAPBOND_PAIRING_STATE_STARTED && !pair_pending &&
       (wc_state==WC_CONNECTING || (wc_state==WC_CONNECTED && h==conn_handle))) {
        security_blocked=true;rbp_server_on_link_message(g_srv,"new pairing requires user action");rb_disconnect(NULL);return;
    }
    if(h!=conn_handle || wc_state!=WC_CONNECTED)return;
    if(state==GAPBOND_PAIRING_STATE_STARTED)rbp_server_on_link(g_srv,RBP_LINK_PAIRING,0,wch_now_ms);
    if(state==GAPBOND_PAIRING_STATE_COMPLETE && status!=SUCCESS) {
        if(pair_pending)fail_pair_sdk("pair authentication failed",status);else {
            security_blocked=true;rbp_server_on_link_message(g_srv,"bond authentication failed; user pairing required");rb_disconnect(NULL);
        }
    }
    if(state==GAPBOND_PAIRING_STATE_BONDED && status!=SUCCESS && !pair_pending) {
        security_blocked=true;rbp_server_on_link_message(g_srv,"bond encryption failed; user pairing required");rb_disconnect(NULL);
    }
    if(state==GAPBOND_PAIRING_STATE_BOND_SAVED && pair_pending) {
        if(status==SUCCESS)bond_ready=true;else fail_pair(RBP_STATUS_STORAGE_FAILED);
    }
}
static gapBondCBs_t wc_bond_cb={wc_passcode_cb,wc_pair_state_cb,NULL};
static uint16_t rb_start_find(void *u,uint16_t duration) {
    (void)u;if(!radio_ready || radio_faulted)return RBP_STATUS_DEVICE_ERROR;
    if(recovering || wc_state!=WC_IDLE)return RBP_STATUS_BUSY;
    candidate_count=0;
    uint32_t ticks=(uint32_t)duration*8/5;
    if(ticks>65535)ticks=65535;
    uint8_t status=gap_result(1,GAP_SetParamValue(TGAP_DISC_SCAN,(uint16_t)ticks),TGAP_DISC_SCAN);
    if(status==SUCCESS)status=gap_result(1,GAP_SetParamValue(TGAP_DISC_SCAN_INT,36),TGAP_DISC_SCAN_INT);
    if(status==SUCCESS)status=gap_result(1,GAP_SetParamValue(TGAP_DISC_SCAN_WIND,18),TGAP_DISC_SCAN_WIND);
    if(status==SUCCESS)status=gap_result(7,GAPRole_CentralStartDiscovery(DEVDISC_MODE_ALL,TRUE,FALSE),0);
    DT(DT_BLE,DT_INFO,11,3,status,duration,0);
    if(status==SUCCESS){set_state(WC_SCAN,(ticks*5/8)+CANCEL_TIMEOUT_MS);return RBP_STATUS_OK;}
    rbp_server_on_link_message(g_srv,"discovery start rejected");
    return status==bleNoResources?RBP_STATUS_RESOURCE_LIMIT:RBP_STATUS_DEVICE_ERROR;
}
static void rb_stop_find(void *u){(void)u;if(wc_state==WC_SCAN){next_link=NEXT_NONE;stop_scan();}}
static void rb_pair_begin(void *u,uint32_t id) {
    (void)u;uint8_t count=255;
    uint16_t status=RBP_STATUS_OK;
    if(!radio_ready || radio_faulted)status=RBP_STATUS_DEVICE_ERROR;
    else if(recovering || (wc_state!=WC_IDLE && wc_state!=WC_SCAN_STOPPING))status=RBP_STATUS_BUSY;
    else if(!id || id>candidate_count)status=RBP_STATUS_NOT_FOUND;
    else if(gap_result(2,GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT,&count),GAPBOND_BOND_COUNT)!=SUCCESS)status=RBP_STATUS_STORAGE_FAILED;
    else if(count)status=RBP_STATUS_BUSY;
    else if(!board_begin_pair())status=RBP_STATUS_STORAGE_FAILED;
    if(status!=RBP_STATUS_OK){rbp_server_on_pair_done(g_srv,status,false,NULL,0);return;}
    pair_pending=true;security_blocked=false;candidate_t *c=&candidates[id-1];
    memcpy(target_addr,c->addr,6);target_type=c->type;
    if(wc_state==WC_SCAN_STOPPING)next_link=NEXT_PAIR;
    else begin_link(target_addr,target_type);
}
static void rb_pair_reply(void *u,bool accept,bool has_passkey,uint32_t passkey) {
    (void)u;if(!pair_pending || wc_state!=WC_CONNECTED || !passkey_pending)return;
    passkey_pending=false;
    uint8_t status=gap_result(11,GAPBondMgr_PasscodeRsp(conn_handle,accept?SUCCESS:SMP_PAIRING_FAILED_CONFIRM_VALUE,has_passkey?passkey:display_passkey),0);
    if(passcode_response_failed(status))fail_pair_sdk("user passkey response failed",status);
}
static void rb_pair_cancel(void *u){(void)u;fail_pair(RBP_STATUS_CANCELLED);}
static void rb_disconnect(void *u) {
    (void)u;
    next_link=NEXT_NONE;passkey_pending=false;
    if(wc_state==WC_SCAN || wc_state==WC_RESOLVE)stop_scan();
    else if(wc_state==WC_CONNECTING) {
        set_state(WC_CONNECT_CANCEL,CANCEL_TIMEOUT_MS);
        /* WCH Central/APP/central.c uses INVALID_CONNHANDLE for initiation
         * cancellation; GAP_CONNHANDLE_INIT belongs to a different API. */
        uint8_t status=gap_result(9,GAPRole_TerminateLink(INVALID_CONNHANDLE),0);
        DT(DT_BLE,DT_INFO,11,4,status,INVALID_CONNHANDLE,0);
        if(status!=SUCCESS)rbp_server_on_link_message(g_srv,"initiation cancel rejected; awaiting terminal event");
    }
    else if(wc_state==WC_CONNECTED) {
        wch_gatt_quiesce();rc003_adapter_detach(g_adapter);set_state(WC_DISCONNECTING,CANCEL_TIMEOUT_MS);
        uint8_t status=gap_result(9,GAPRole_TerminateLink(conn_handle),0);
        DT(DT_BLE,DT_INFO,11,5,status,conn_handle,0);
        if(status!=SUCCESS)rbp_server_on_link_message(g_srv,"disconnect rejected; awaiting terminal event");
    }
}
static void rb_forget_peer(void *u) {
    (void)u;
    if(!board_begin_forget()){rbp_server_on_forget_done(g_srv,RBP_STATUS_STORAGE_FAILED,true);return;}
    recovering=true;delete_report=true;erase_requested=false;recovery_deadline=0;
    rb_disconnect(NULL);
}
static void rb_connect_peer(void *u) {
    (void)u;if(!radio_ready || radio_faulted || security_blocked || recovering || wc_state!=WC_IDLE)return;
    uint8_t saved[6],type;
    if(!board_addr_load(saved,&type) || !read_identity() || type!=identity_type || memcmp(saved,identity,6)) {
        security_blocked=true;rbp_server_on_link_message(g_srv,"bond identity unavailable/mismatch");rbp_server_on_link(g_srv,RBP_LINK_ERROR,0,wch_now_ms);return;
    }
    /* Do not enable controller privacy as a side effect of knowing a peer IRK.
     * Core Vol 6 Part B 6.4 forbids an RPA initiator from responding to a
     * directed advertisement with public/static TargetA. RC003 advertises our
     * public identity. Keep our identity public throughout pairing/reconnect;
     * resolve the remote's RPA in the host using its independently saved IRK. */
#ifdef RBP_DEBUG
    trace_local_identity();
#endif
    uint8_t status=SUCCESS;
    /* HOGP 5.2.3: remain listening for device wakeup. USB power allows a
     * larger window than the minimum recommendation (60ms / 30ms). */
    uint8_t flags=board_hid_flags_load();
    uint16_t interval=flags<=3 && (flags&2)?48:96;
    status=gap_result(1,GAP_SetParamValue(TGAP_DISC_SCAN,0),TGAP_DISC_SCAN);
    if(status==SUCCESS)status=gap_result(1,GAP_SetParamValue(TGAP_DISC_SCAN_INT,interval),TGAP_DISC_SCAN_INT);
    if(status==SUCCESS)status=gap_result(1,GAP_SetParamValue(TGAP_DISC_SCAN_WIND,48),TGAP_DISC_SCAN_WIND);
    if(status==SUCCESS)status=gap_result(7,GAPRole_CentralStartDiscovery(DEVDISC_MODE_ALL,FALSE,FALSE),0);
    DT(DT_BLE,DT_INFO,11,7,status,interval,48);
    DT(DT_BLE,DT_INFO,12,flags,have_irk,identity_type,0);
    if(status==SUCCESS){set_state(WC_RESOLVE,0);rbp_server_on_link(g_srv,RBP_LINK_CONNECTING,0,wch_now_ms);}
    else {disconnected();rbp_server_on_link_message(g_srv,"reconnect scan rejected");}
}
static void rb_voice_stop(void *u){(void)u;rc003_adapter_mic_stop(g_adapter);}
#ifdef RBP_EXPERIMENTAL_HOST_VOICE_START
static uint16_t rb_voice_start(void *u){(void)u;return rc003_adapter_mic_start(g_adapter);}
#else
#define rb_voice_start NULL
#endif
static const rbp_radio_backend_t backend={NULL,rb_start_find,rb_stop_find,rb_pair_begin,rb_pair_reply,rb_pair_cancel,rb_forget_peer,rb_connect_peer,rb_disconnect,rb_voice_stop,rb_voice_start};
const rbp_radio_backend_t *wch_central_backend(void){return &backend;}
static void start_encrypted_adapter(void) {
    if(wc_state==WC_CONNECTED && !adapter_started && !radio_faulted && !security_blocked &&
       linkDB_State(conn_handle,LINK_ENCRYPTED)) {
        adapter_started=true;
        rc003_adapter_start_bound(g_adapter,wch_now_ms,pair_pending?0:board_peer_counter());
    }
}
static void poll(void) {
    if(!radio_ready){
        if(!radio_faulted && (int32_t)(wch_now_ms-deadline)>=0)radio_fault("BLE initialization event timed out",bleTimeout);
        return;
    }
    /* Terminal-event waits run even while a storage rollback is pending. */
    if(!radio_faulted && (wc_state==WC_SCAN_STOPPING || wc_state==WC_CONNECT_CANCEL || wc_state==WC_DISCONNECTING) &&
       (int32_t)(wch_now_ms-deadline)>=0) {
        radio_fault("radio cancellation completion timed out",bleTimeout);
        fail_pair(RBP_STATUS_TIMEOUT);
    }
    if(recovering) {
        if(radio_faulted) {
            recovering=false;if(delete_report)rbp_server_on_forget_done(g_srv,RBP_STATUS_STORAGE_FAILED,true);
            return; /* durable journal retained; never erase on uncertain radio state */
        }
        if(wc_state!=WC_IDLE)return;
        if(!recovery_deadline)recovery_deadline=wch_now_ms+5000;
        if((int32_t)(wch_now_ms-recovery_deadline)>=0) {
            recovering=false;if(delete_report)rbp_server_on_forget_done(g_srv,RBP_STATUS_STORAGE_FAILED,true);
            rbp_server_on_link_message(g_srv,"bond transaction recovery failed");rbp_server_on_link(g_srv,RBP_LINK_ERROR,0,wch_now_ms);return;
        }
        if(!erase_requested) {if(gap_result(3,GAPBondMgr_SetParameter(GAPBOND_ERASE_ALLBONDS,0,NULL),GAPBOND_ERASE_ALLBONDS)!=SUCCESS)return;erase_requested=true;return;}
        uint8_t count=255;if(gap_result(2,GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT,&count),GAPBOND_BOND_COUNT)!=SUCCESS || count)return;
        if(delete_report)rbp_server_on_forget_done(g_srv,RBP_STATUS_OK,false);
        else if(board_store.clear(NULL)) {rbp_peer_record_t empty={0};rbp_server_on_peer_changed(g_srv,&empty);rbp_server_on_link(g_srv,RBP_LINK_UNBOUND,0,wch_now_ms);}
        else {rbp_server_on_link_message(g_srv,"EEPROM recovery commit failed");rbp_server_on_link(g_srv,RBP_LINK_ERROR,0,wch_now_ms);}
        recovering=false;security_blocked=false;return;
    }
    if(radio_faulted)return;
    if(wc_state==WC_CONNECTED) {
        start_encrypted_adapter();
        if(pair_pending && bond_ready && g_adapter->ready)finish_pair();
        if((!adapter_started || (pair_pending && !bond_ready)) && (int32_t)(wch_now_ms-deadline)>=0) {
            /* A local deadline is not evidence that the saved key is invalid.
             * Retire this link through GAP, then retry the existing bond with
             * normal backoff. Explicit authentication failures still block. */
            if(!pair_pending)rbp_server_on_link_message(g_srv,"bond encryption timed out; retrying after disconnect");
            fail_pair(RBP_STATUS_TIMEOUT);rb_disconnect(NULL);
        }
        if(g_adapter->ready)backoff=0;
    } else if(wc_state==WC_SCAN && (int32_t)(wch_now_ms-deadline)>=0) {
        rbp_server_on_scan_done(g_srv,RBP_FIND_DONE_EXPIRED,wch_now_ms);rb_disconnect(NULL);
    } else if(wc_state==WC_CONNECTING && (int32_t)(wch_now_ms-deadline)>=0) {
        fail_pair(RBP_STATUS_TIMEOUT);rb_disconnect(NULL);
    } else if(wc_state==WC_IDLE && !security_blocked && rbp_server_should_reconnect(g_srv) && (int32_t)(wch_now_ms-reconnect_at)>=0)rb_connect_peer(NULL);
}
static uint16_t process(uint8_t task,uint16_t events) {
    if(events&SYS_EVENT_MSG) {
#ifdef RBP_DEBUG
        uint32_t rx_begin=TMOS_GetSystemClock();
#endif
        uint8_t *p=tmos_msg_receive(task);
        if(p) {
            if(((tmos_event_hdr_t*)p)->event==GATT_MSG_EVENT){
                gattMsgEvent_t *m=(gattMsgEvent_t*)p;
                /* A persisted notification can arrive before the periodic poll.
                 * Initialize its cached routes after encryption, before delivery.
                 * Indications (Service Changed) still invalidate the idle cache first. */
                if(m->connHandle==conn_handle && m->method==ATT_HANDLE_VALUE_NOTI)start_encrypted_adapter();
                wch_gatt_on_msg(p);GATT_bm_free(&m->msg,m->method);
            }
            tmos_msg_deallocate(p);
#ifdef RBP_DEBUG
            uint32_t rx_done=TMOS_GetSystemClock();
#endif
            rbp_server_flush_output(g_srv);
#ifdef RBP_DEBUG
            uint32_t flush_done=TMOS_GetSystemClock();
            /* Log slow work at state level; normal per-message timing is detail. */
            DT(DT_SYS,(flush_done-rx_begin)>=8?DT_INFO:DT_DETAIL,3,
               rx_done-rx_begin,flush_done-rx_done,0,0);
#endif
        }
        /* Match WCH Central: consume one message and release its SDK buffers
         * immediately. TMOS keeps SYS_EVENT_MSG asserted while more remain.
         * Do not use the scarce SDK queue as a USB pacing buffer. */
        /* A busy RX queue must not starve a co-pending deadline poll. */
        events^=SYS_EVENT_MSG;
    }
    if(events&WC_START_EVT) {
        if(!radio_faulted){uint8_t status=gap_result(6,GAPRole_CentralStartDevice(task,&wc_bond_cb,&wc_role_cb),0);if(status!=SUCCESS)radio_fault("BLE start rejected",status);}
        tmos_start_task(task,WC_POLL_EVT,32);events^=WC_START_EVT;
    }
    if(events&WC_POLL_EVT) {poll();tmos_start_task(task,WC_POLL_EVT,32);events^=WC_POLL_EVT;}return events;
}
void wch_central_init(rbp_server_t *srv,rc003_adapter_t *adapter) {
    g_srv=srv;g_adapter=adapter;wch_gatt_set_adapter(adapter);wch_central_task_id=TMOS_ProcessEventRegister(process);
    deadline=wch_now_ms+CANCEL_TIMEOUT_MS;
    if(wch_central_task_id==INVALID_TASK_ID)radio_fault("TMOS task registration failed",FAILURE);
    const uint16_t params[][2]={{TGAP_CONN_EST_INT_MIN,12},{TGAP_CONN_EST_INT_MAX,24},
        {TGAP_CONN_EST_SUPERV_TIMEOUT,300},{TGAP_CONN_EST_SCAN_INT,96},{TGAP_CONN_EST_SCAN_WIND,48}};
    for(unsigned i=0;i<sizeof params/sizeof params[0];i++) {
        uint8_t status=gap_result(1,GAP_SetParamValue(params[i][0],params[i][1]),params[i][0]);
        if(status!=SUCCESS)radio_fault("GAP configuration failed",status);
    }
    const uint16_t ids[]={GAPBOND_CENT_PAIRING_MODE,GAPBOND_CENT_MITM_PROTECTION,
        GAPBOND_CENT_IO_CAPABILITIES,GAPBOND_CENT_BONDING_ENABLED,GAPBOND_BOND_AUTO,
        GAPBOND_ERASE_AUTO,GAPBOND_AUTO_SYNC_RL,GAPBOND_BOND_FAIL_ACTION};
    uint8_t values[]={GAPBOND_PAIRING_MODE_NO_PAIRING,FALSE,GAPBOND_IO_CAP_KEYBOARD_DISPLAY,
        TRUE,TRUE,FALSE,FALSE,GAPBOND_FAIL_TERMINATE_LINK};
    for(unsigned i=0;i<sizeof values;i++) {
        uint8_t status=gap_result(3,GAPBondMgr_SetParameter(ids[i],1,&values[i]),ids[i]);
        if(status!=SUCCESS)radio_fault("bond configuration failed",status);
    }
    uint8_t st=board_storage_state();recovering=st==2 || st==3;recovery_deadline=0;
    if(st==255)radio_fault("invalid EEPROM record; recovery required",NV_OPER_FAILED);
    uint8_t status=GATT_InitClient();if(status!=SUCCESS)radio_fault("GATT initialization failed",status);
    GATT_RegisterForInd(wch_central_task_id);
    if(!radio_faulted)tmos_set_event(wch_central_task_id,WC_START_EVT);
}
