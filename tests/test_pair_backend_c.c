/* Real Central; SDK acceptance and completion are independent. */
#include "../firmware/wch/wch_central.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t wch_now_ms;
static rc003_adapter_t adapter;
static gapBondRec_t record;
static uint8_t nv_status, count_value, store_state, flags_value, cancel_status,
    terminate_status, start_status, privacy_status, config_status, pair_mode;
static unsigned completed, terminated, staged, initiations, scans, cancels,
    errors, initializing, detached, started, erased;
static uint16_t result, last_terminate, scan_ticks, scan_interval, scan_window;
static bool irk_present, encrypted, auto_connect;
static uint8_t last_addr[6], last_type;
static unsigned queued, freed, pumps, scheduled;
static unsigned address_configured;
static unsigned scan_reports;static rbp_candidate_t scan_last;
static uint8_t address_status, auto_sync_rl;
static tmos_event_hdr_t rx_message;
static char last_link_message[80];
bool board_addr_load(uint8_t *a, uint8_t *t) {
  memcpy(a, record.publicAddr, 6);
  *t = record.publicAddrType;
  return true;
}
uint8_t board_hid_flags_load(void) { return flags_value; }
void board_hid_flags_stage(uint8_t f) { flags_value = f; }
uint8_t board_storage_state(void) { return store_state; }
uint32_t board_peer_counter(void) { return 7; }
bool board_begin_pair(void) {
  store_state = 3;
  return true;
}
bool board_begin_forget(void) {
  store_state = 2;
  return true;
}
void board_addr_stage(const uint8_t *a, uint8_t t) {
  assert(!memcmp(a, record.publicAddr, 6) && t == record.publicAddrType);
  staged++;
}
static bool clear_store(void *u) {
  (void)u;
  store_state = 1;
  return true;
}
const rbp_store_t board_store = {NULL, NULL, NULL, clear_store};
bStatus_t tmos_snv_read(tmosSnvId_t id, tmosSnvLen_t len, void *out) {
  if (id == mainRecordNvID(0)) {
    assert(len == sizeof record);
    if (!nv_status)
      memcpy(out, &record, len);
    return nv_status;
  }
  assert(id == devIRKNvID(0));
  if (irk_present) {
    memset(out, 0x55, len);
    return SUCCESS;
  }
  return NV_OPER_FAILED;
}
bStatus_t GAPBondMgr_GetParameter(uint16_t id, void *out) {
  assert(id == GAPBOND_BOND_COUNT);
  *(uint8_t *)out = count_value;
  return SUCCESS;
}
bStatus_t GAPBondMgr_SetParameter(uint16_t id, uint8_t len, void *v) {
  (void)len;
  if (config_status)
    return config_status;
  if (id == GAPBOND_CENT_PAIRING_MODE)
    pair_mode = *(uint8_t *)v;
  if (id == GAPBOND_AUTO_SYNC_RL)
    auto_sync_rl = *(uint8_t *)v;
  if (id == GAPBOND_ERASE_ALLBONDS) {
    assert(wc_state == WC_IDLE && !radio_faulted);
    erased++;
    count_value = 0;
  }
  return SUCCESS;
}
bStatus_t GAPRole_SetPrivacyMode(uint8_t t, uint8_t *a, uint8_t m) {
  assert(wc_state == WC_IDLE && m == 1 && t == record.publicAddrType &&
         !memcmp(a, record.publicAddr, 6));
  assert(!"public central must not configure controller privacy");
  return privacy_status;
}
bStatus_t GAP_ConfigDeviceAddr(uint8_t type, uint8_t *addr) {
  assert(type == ADDRTYPE_PUBLIC && addr == NULL && wc_state == WC_IDLE);
  address_configured++;
  return address_status;
}
bStatus_t GAPRole_CentralEstablishLink(uint8_t h, uint8_t w, uint8_t t,
                                       uint8_t *a) {
  assert(!h && !w && wc_state == WC_CONNECTING);
  initiations++;
  memcpy(last_addr, a, 6);
  last_type = t;
  return start_status;
}
bStatus_t GAPRole_CentralStartDiscovery(uint8_t m, uint8_t a, uint8_t w) {
  assert(m == DEVDISC_MODE_ALL && !w && wc_state == WC_IDLE);
  (void)a;
  scans++;
  return start_status;
}
bStatus_t GAP_SetParamValue(uint16_t p, uint16_t v) {
  if (p == TGAP_DISC_SCAN)
    scan_ticks = v;
  if (p == TGAP_DISC_SCAN_INT)
    scan_interval = v;
  if (p == TGAP_DISC_SCAN_WIND)
    scan_window = v;
  return config_status;
}
bStatus_t GAP_ResolvePrivateAddr(uint8_t *k, uint8_t *a) {
  (void)k;
  return a[0] == 0x55 ? SUCCESS : FAILURE;
}
bStatus_t GAPRole_TerminateLink(uint16_t h) {
  if (h == GAP_CONNHANDLE_INIT)
    return bleIncorrectMode;
  assert(h == INVALID_CONNHANDLE || h == conn_handle);
  terminated++;
  last_terminate = h;
  return terminate_status;
}
bStatus_t GAPRole_CentralCancelDiscovery(void) {
  assert(wc_state == WC_SCAN_STOPPING);
  cancels++;
  return cancel_status;
}
bStatus_t GAPBondMgr_PasscodeRsp(uint16_t h, uint8_t s, uint32_t k) {
  (void)s;
  (void)k;
  assert(h == conn_handle);
  return config_status;
}
uint8_t linkDB_State(uint16_t h, uint8_t s) {
  assert(h == conn_handle && s == LINK_ENCRYPTED);
  return encrypted;
}
void wch_gatt_quiesce(void) {}
void wch_gatt_set_conn(uint16_t h) { (void)h; }
uint16_t rc003_adapter_mic_start(rc003_adapter_t *a){(void)a;return RBP_STATUS_OK;}
void rc003_adapter_detach(rc003_adapter_t *a) {
  a->ready = false;
  detached++;
}
void rc003_adapter_start_bound(rc003_adapter_t *a, uint32_t n, uint32_t peer_id) {
  (void)peer_id;
  (void)a;
  (void)n;
  started++;
}
void rbp_server_on_link(rbp_server_t *s, rbp_link_state_t st, uint32_t id,
                        uint32_t n) {
  (void)s;
  (void)id;
  (void)n;
  if (st == RBP_LINK_ERROR)
    errors++;
  if (st == RBP_LINK_INITIALIZING)
    initializing++;
}
void rbp_server_on_link_message(rbp_server_t *s, const char *m) {
  (void)s;
  assert(m && *m);
  snprintf(last_link_message,sizeof last_link_message,"%s",m);
}
void rbp_server_on_scan_done(rbp_server_t *s, uint8_t r, uint32_t n) {
  (void)s;
  (void)r;
  (void)n;
}
void rbp_server_on_forget_done(rbp_server_t *s, uint16_t st, bool u) {
  (void)s;
  (void)st;
  (void)u;
}
void rbp_server_on_peer_changed(rbp_server_t *s, const rbp_peer_record_t *p) {
  (void)s;
  (void)p;
}
bool rbp_server_should_reconnect(const rbp_server_t *s) {
  (void)s;
  return auto_connect;
}
void rbp_server_on_pair_done(rbp_server_t *s, uint16_t st, bool commit,
                             const rbp_peer_record_t *p, uint32_t id) {
  (void)s;
  (void)id;
  completed++;
  result = st;
  if (st == RBP_STATUS_OK) {
    assert(commit && p && p->peer_id == 7 && p->auto_reconnect);
    store_state = 1;
  }
}
uint8_t *tmos_msg_receive(uint8_t t) {
  (void)t;
  if (!queued)
    return NULL;
  queued--;
  return (uint8_t *)&rx_message;
}
bStatus_t tmos_msg_deallocate(uint8_t *p) {
  assert(p == (uint8_t *)&rx_message);
  freed++;
  return SUCCESS;
}
BOOL tmos_start_task(uint8_t t, tmosEvents e, tmosTimer n) {
  (void)t;
  assert(e == WC_POLL_EVT && n == 32);
  scheduled++;
  return TRUE;
}
void rbp_server_flush_output(rbp_server_t *s) {
  (void)s;
  pumps++;
}
static void reset(void) {
  memset(&central_flags, 0, sizeof central_flags);
  memset(&adapter, 0, sizeof adapter);
  g_adapter = &adapter;
  g_srv = NULL;
  conn_handle = GAP_CONNHANDLE_INIT;
  wc_state = WC_IDLE;
  backoff = next_link = 0;
  radio_faulted = security_blocked = passkey_pending = false;
  radio_ready = true;
  wch_now_ms = 100;
  deadline = reconnect_at = recovery_deadline = 0;
  memset(&record, 0, sizeof record);
  record.publicAddr[0] = 0x40;
  record.publicAddr[5] = 0xc0;
  count_value = 1;
  store_state = 1;
  flags_value = 255;
  nv_status = 0;
  irk_present = true;
  cancel_status = terminate_status = start_status = privacy_status =
      config_status = 0;
  completed = terminated = staged = initiations = scans = cancels = errors =
      initializing = detached = started = erased = 0;
  encrypted = auto_connect = false;
  queued = freed = pumps = scheduled = 0;
  address_configured = address_status = 0;
  auto_sync_rl = 255;
}
static void scan_done(void) {
  gapRoleEvent_t e = {0};
  e.gap.opcode = GAP_DEVICE_DISCOVERY_EVENT;
  e.gap.hdr.status = bleGAPUserCanceled;
  wc_event_cb(&e);
}
static void link_event(uint8_t s, uint16_t h) {
  gapRoleEvent_t e = {0};
  e.gap.opcode = GAP_LINK_ESTABLISHED_EVENT;
  e.gap.hdr.status = s;
  e.linkCmpl.connectionHandle = h;
  wc_event_cb(&e);
}
static void terminated_event(uint16_t h) {
  gapRoleEvent_t e = {0};
  e.gap.opcode = GAP_LINK_TERMINATED_EVENT;
  e.linkTerminate.connectionHandle = h;
  wc_event_cb(&e);
}
static void expire(void) {
  wch_now_ms = deadline;
  poll();
}
static void match(void) {
  resolve_advertisement(record.publicAddr, record.publicAddrType,
                        GAP_ADRPT_ADV_IND);
}
int main(void) {
  reset();wc_state=WC_SCAN;candidate_count=0;scan_reports=0;
  uint8_t adv[]={3,3,0x12,0x18};gapDeviceInfoEvent_t observed={0};
  observed.pEvtData=adv;observed.dataLen=sizeof adv;observed.rssi=-50;
  on_device(&observed);assert(scan_reports==1 && candidate_count==1 && scan_last.support==0);
  uint8_t name[]={4,9,'A','B','C'};observed.pEvtData=name;observed.dataLen=sizeof name;
  on_device(&observed);assert(scan_reports==2 && scan_last.candidate_id==1 && !strcmp(scan_last.name,"ABC"));
  /* Names without advertised HIDS must remain discoverable, including names
   * delivered only in the scan response. Duplicate Xiaomi/other reports do
   * not consume another slot. */
  observed.addr[0]=1;observed.eventType=GAP_ADRPT_SCAN_RSP;
  on_device(&observed);assert(scan_reports==3 && candidate_count==2 && scan_last.support==0);
  on_device(&observed);assert(scan_reports==4 && candidate_count==2);
  observed.addr[0]=2;observed.eventType=GAP_ADRPT_ADV_NONCONN_IND;
  on_device(&observed);assert(scan_reports==4 && candidate_count==2);
  uint8_t fd[]={3,3,0,0xfd};observed.pEvtData=fd;observed.dataLen=sizeof fd;observed.eventType=GAP_ADRPT_ADV_IND;
  on_device(&observed);assert(scan_reports==5 && candidate_count==3 && scan_last.support==0);
  uint8_t invalid_name[]={3,9,0xc0,0x80};observed.addr[0]=3;observed.pEvtData=invalid_name;observed.dataLen=sizeof invalid_name;
  on_device(&observed);assert(scan_reports==5 && candidate_count==3);
  char text[49];memset(text,0x7e,sizeof text);uint8_t longname[60];memset(longname,'A',sizeof longname);
  assert(candidate_name(text,longname,sizeof longname)==47 && text[47]==0 && text[48]==0x7e);
  uint8_t badutf[]={0xc0,0x80};assert(!candidate_name(text,badutf,sizeof badutf));

#ifdef RBP_EXPERIMENTAL_HOST_VOICE_START
  assert(wch_central_backend()->voice_request_start != NULL);
#else
  assert(wch_central_backend()->voice_request_start == NULL);
#endif
  /* Match the pinned SDK's nonfatal passcode return, across all three
   * response paths. It must not claim pairing success or replay a request. */
  for(unsigned path=0;path<3;path++) {
    for(unsigned bad=0;bad<2;bad++) {
      reset();wc_state=WC_CONNECTED;conn_handle=1;pair_pending=true;
      config_status=bad?bleIncorrectMode:0xfe;
      if(path==2){passkey_pending=true;rb_pair_reply(NULL,true,true,123456);}
      else wc_passcode_cb(NULL,1,0,path==1);
      assert(pair_pending==!bad);
      assert(completed==(bad?1u:0u));
      if(!bad) {
        assert(wc_state==WC_CONNECTED&&terminated==0);
        config_status=SUCCESS;
        wc_pair_state_cb(1,GAPBOND_PAIRING_STATE_COMPLETE,SMP_PAIRING_FAILED_AUTH_REQ);
        assert(!pair_pending&&result==RBP_STATUS_PAIRING_FAILED);
      }
    }
  }
  reset();wc_state=WC_CONNECTED;conn_handle=1;pair_pending=true;
  wc_pair_state_cb(1,GAPBOND_PAIRING_STATE_COMPLETE,SMP_PAIRING_FAILED_AUTH_REQ);
  assert(result==RBP_STATUS_PAIRING_FAILED&&!pair_pending);
  assert(strcmp(last_link_message,"pair authentication failed (SDK 0x03)")==0);
  reset();
  wc_state = WC_CONNECTED;
  conn_handle = 1;
  pair_pending = true;
  adapter.ready = true;
  adapter.hid_info_valid = true;
  adapter.hid_flags = 2;
  wc_pair_state_cb(1, GAPBOND_PAIRING_STATE_BONDED, SUCCESS);
  assert(!bond_ready);
  wc_pair_state_cb(1, GAPBOND_PAIRING_STATE_BOND_SAVED, SUCCESS);
  assert(bond_ready);
  finish_pair();
  assert(completed == 1 && result == RBP_STATUS_OK && staged == 1 &&
         !terminated && flags_value == 2);
  pair_pending = true;
  store_state = 3;
  nv_status = NV_OPER_FAILED;
  finish_pair();
  assert(completed == 2 && result == RBP_STATUS_STORAGE_FAILED &&
         terminated == 1 && recovering && wc_state == WC_DISCONNECTING);
  fail_pair(RBP_STATUS_STORAGE_FAILED);
  assert(completed == 2);
  poll();
  assert(!erased);
  terminated_event(1);
  poll();
  assert(erased == 1);
  reset();
  record.publicAddrType = 255;
  assert(!read_identity());
  record.publicAddrType = 0;
  memset(record.publicAddr, 0, 6);
  assert(!read_identity());
  memset(record.publicAddr, 255, 6);
  assert(!read_identity());
  reset();
  count_value = 0;
  assert(!read_identity());
  reset();
  irk_present = false;
  assert(read_identity() && !have_irk);
  reset();
  rb_connect_peer(NULL);
  assert(scans == 1 && !initiations && wc_state == WC_RESOLVE &&
         scan_ticks == 0 && scan_interval == 96 && scan_window == 48);
  wch_now_ms += 600000;
  poll();
  assert(wc_state == WC_RESOLVE && scans == 1);
  resolve_advertisement(identity, identity_type, GAP_ADRPT_SCAN_RSP);
  assert(!cancels);
  uint8_t unknown[6] = {1, 2, 3, 4, 5, 6};
  resolve_advertisement(unknown, 0, 0);
  assert(!cancels);
  match();
  assert(wc_state == WC_SCAN_STOPPING && cancels == 1 && !initiations);
  scan_done();
  assert(wc_state == WC_CONNECTING && initiations == 1 &&
         pair_mode == GAPBOND_PAIRING_MODE_INITIATE);
  link_event(SUCCESS, 1);
  assert(wc_state == WC_CONNECTED && initializing == 1 &&
         pair_mode == GAPBOND_PAIRING_MODE_NO_PAIRING);
  poll();
  assert(!started);
  encrypted = true;
  poll();
  assert(started == 1);
  terminated_event(9);
  assert(wc_state == WC_CONNECTED && !detached);
  terminated_event(1);
  assert(wc_state == WC_IDLE && detached == 1);
  terminated_event(1);
  assert(detached == 1);
  reset();
  rb_connect_peer(NULL);match();scan_done();link_event(SUCCESS,1);
  rb_disconnect(NULL);
  assert(wc_state==WC_DISCONNECTING && detached==1);
  rb_disconnect(NULL);assert(detached==1); /* repeated request owns no work */
  terminated_event(1);assert(wc_state==WC_IDLE && detached==2);
  reset();
  rb_connect_peer(NULL);
  uint8_t rpa[6] = {0x55, 2, 3, 4, 5, 0x40};
  resolve_advertisement(rpa, ADDRTYPE_PRIVATE_RESOLVE, 0);
  scan_done();
  assert(initiations == 1 && last_type == ADDRTYPE_PRIVATE_RESOLVE &&
         !memcmp(last_addr, rpa, 6));
  reset();
  rb_connect_peer(NULL);
  gapRoleEvent_t direct = {0};
  direct.gap.opcode = GAP_DIRECT_DEVICE_INFO_EVENT;
  memcpy(direct.deviceDirectInfo.addr, identity, 6);
  direct.deviceDirectInfo.eventType = GAP_ADRPT_ADV_DIRECT_IND;
  wc_event_cb(&direct);
  assert(wc_state == WC_SCAN_STOPPING);
  rb_disconnect(NULL);
  scan_done();
  assert(wc_state == WC_IDLE && !initiations);
  reset();
  count_value = 0;
  rb_start_find(NULL, 1000);
  candidate_count = 1;
  memcpy(candidates[0].addr, record.publicAddr, 6);
  candidates[0].type = 0;
  rb_stop_find(NULL);
  rb_pair_begin(NULL, 1);
  assert(pair_pending && next_link == NEXT_PAIR && !initiations);
  scan_done();
  assert(initiations == 1 && wc_state == WC_CONNECTING);
  reset();
  count_value = 0;
  rb_start_find(NULL, 1000);
  candidate_count = 1;
  memcpy(candidates[0].addr, record.publicAddr, 6);
  rb_stop_find(NULL);
  rb_pair_begin(NULL, 1);
  rb_pair_cancel(NULL);
  scan_done();
  assert(!initiations && completed == 1 && result == RBP_STATUS_CANCELLED);
  for (unsigned reject = 0; reject < 2; reject++) {
    reset();
    rb_connect_peer(NULL);
    cancel_status = reject ? bleIncorrectMode : SUCCESS;
    match();
    assert(wc_state == WC_SCAN_STOPPING);
    scan_done();
    assert(wc_state == WC_CONNECTING && initiations == 1);
    terminate_status = reject ? bleIncorrectMode : SUCCESS;
    expire();
    assert(wc_state == WC_CONNECT_CANCEL &&
           last_terminate == INVALID_CONNHANDLE);
    rb_disconnect(NULL);
    poll();
    assert(terminated == 1);
    link_event(SUCCESS, 1);
    assert(wc_state == WC_DISCONNECTING && !initializing &&
           last_terminate == 1);
    terminated_event(1);
    assert(wc_state == WC_IDLE);
  }
  reset();
  rb_connect_peer(NULL);
  match();
  scan_done();
  expire();
  link_event(bleTimeout, 0);
  assert(wc_state == WC_IDLE && !radio_faulted);
  link_event(bleTimeout, 0);
  assert(wc_state == WC_IDLE);
  reset();
  rb_connect_peer(NULL);
  match();
  expire();
  assert(radio_faulted && !initiations);
  unsigned prior = errors;
  poll();
  assert(errors == prior);
  scan_done();
  assert(wc_state == WC_IDLE);
  rb_connect_peer(NULL);
  assert(scans == 1);
  reset();
  rb_connect_peer(NULL);
  match();
  scan_done();
  expire();
  expire();
  assert(radio_faulted && terminated == 1);
  link_event(SUCCESS, 1);
  assert(wc_state == WC_DISCONNECTING && !initializing);
  terminated_event(1);
  assert(radio_faulted);
  reset();
  rb_connect_peer(NULL);
  match();
  scan_done();
  wc_pair_state_cb(1, GAPBOND_PAIRING_STATE_STARTED, SUCCESS);
  assert(security_blocked && wc_state == WC_CONNECT_CANCEL);
  link_event(SUCCESS, 1);
  assert(!initializing && wc_state == WC_DISCONNECTING);
  reset();
  rb_connect_peer(NULL);
  match();
  scan_done();
  link_event(SUCCESS, 1);
  wc_pair_state_cb(1, GAPBOND_PAIRING_STATE_BONDED, FAILURE);
  assert(security_blocked && wc_state == WC_DISCONNECTING);
  terminated_event(1);
  auto_connect = true;
  wch_now_ms += 10000;
  poll();
  assert(scans == 1 && !erased);
  reset();
  /* Missing encryption completion must not permanently disable reconnect.
   * Wait for the real termination, respect backoff, and reuse the bond. */
  auto_connect = true;
  rb_connect_peer(NULL);
  match();
  scan_done();
  link_event(SUCCESS, 1);
  expire();
  assert(!security_blocked && wc_state == WC_DISCONNECTING && terminated == 1);
  poll();
  assert(scans == 1 && terminated == 1 && !erased);
  terminated_event(1);
  assert(wc_state == WC_IDLE && !security_blocked);
  wch_now_ms = reconnect_at - 1;
  poll();
  assert(scans == 1);
  wch_now_ms++;
  poll();
  assert(scans == 2 && wc_state == WC_RESOLVE);
  match();
  scan_done();
  link_event(SUCCESS, 2);
  encrypted = true;
  poll();
  assert(started == 1 && adapter_started && !security_blocked && !erased);
  start_encrypted_adapter();
  assert(started == 1); /* notification/poll race cannot start twice */
  reset();
  wc_state=WC_CONNECTED;conn_handle=1;
  start_encrypted_adapter();assert(!started && !adapter_started);
  encrypted=true;security_blocked=true;
  start_encrypted_adapter();assert(!started && !adapter_started);
  security_blocked=false;
  start_encrypted_adapter();assert(started==1 && adapter_started);
  reset();
  privacy_status = bleIncorrectMode;
  rb_connect_peer(NULL);
  assert(!radio_faulted && scans == 1 && !initiations);
  reset();
  start_status = bleNoResources;
  rb_connect_peer(NULL);
  assert(wc_state == WC_IDLE && scans == 1);
  reset();
  flags_value = 2;
  rb_connect_peer(NULL);
  assert(scan_interval == 48 && scan_window == 48);
  reset();
  queued = 5;
  for (unsigned i = 0; i < 5; i++) {
    assert(process(0, SYS_EVENT_MSG | WC_POLL_EVT) == 0);
    assert(queued == 4 - i && freed == i + 1 && pumps == i + 1);
    assert(scheduled == i + 1); /* bounded RX does not starve deadline poll */
  }
  assert(process(0, SYS_EVENT_MSG) == 0 && freed == 5 && pumps == 5);
  reset();
  count_value = 0;
  assert(rb_start_find(NULL, 1000) == RBP_STATUS_OK);
  gapRoleEvent_t natural = {0};
  natural.gap.opcode = GAP_DEVICE_DISCOVERY_EVENT;
  wc_event_cb(&natural);
  assert(wc_state == WC_IDLE);
  reset();
  start_status = bleNoResources;
  assert(rb_start_find(NULL, 1000) == RBP_STATUS_RESOURCE_LIMIT &&
         wc_state == WC_IDLE);
  reset();
  wc_state = WC_CONNECTED;
  conn_handle = 1;
  rb_forget_peer(NULL);
  wch_now_ms += 4000;
  poll();
  assert(!erased && !recovery_deadline);
  terminated_event(1);
  poll();
  assert(erased == 1 && recovery_deadline == wch_now_ms + 5000 &&
         !radio_faulted);
  reset();
  wc_state = WC_CONNECTED;
  conn_handle = 1;
  rb_forget_peer(NULL);
  expire();
  assert(radio_faulted && !erased && !recovering);
  reset();
  wch_now_ms = UINT32_MAX - 1000;
  rb_connect_peer(NULL);
  match();
  wch_now_ms += 4999;
  poll();
  assert(!radio_faulted);
  wch_now_ms++;
  poll();
  assert(radio_faulted);
  reset();
  radio_ready = false;
  wch_central_init(NULL, &adapter);
  assert(!radio_faulted && pair_mode == GAPBOND_PAIRING_MODE_NO_PAIRING);
  assert(auto_sync_rl == FALSE);
  process(0, WC_START_EVT);
  expire();
  assert(radio_faulted);
  reset();
  radio_ready = false;
  config_status = INVALIDPARAMETER;
  wch_central_init(NULL, &adapter);
  assert(radio_faulted && !scans && !initiations);
  reset();
  radio_ready = false;
  gapRoleEvent_t initialized = {0};
  initialized.gap.opcode = GAP_DEVICE_INIT_DONE_EVENT;
  wc_event_cb(&initialized);
  assert(radio_ready && !radio_faulted && address_configured == 1);
  reset();
  radio_ready = false;
  address_status = bleIncorrectMode;
  wc_event_cb(&initialized);
  assert(!radio_ready && radio_faulted && address_configured == 1);
  rb_connect_peer(NULL);
  assert(!scans && !initiations);
  puts("Central: async ownership, takeover, cancellation "
       "races/rejections/deadlines, stale handles, security, privacy, "
       "persistence, startup and immediate RX release passed");
}
bStatus_t GAPRole_CentralStartDevice(uint8_t t, gapBondCBs_t *b,
                                     gapCentralRoleCB_t *c) {
  (void)t;
  (void)b;
  (void)c;
  return start_status;
}
bStatus_t GATT_InitClient(void) { return config_status; }
void GATT_RegisterForInd(uint8_t t) { (void)t; }
void GATT_bm_free(gattMsg_t *m, uint8_t o) {
  (void)m;
  (void)o;
  abort();
}
tmosTaskID TMOS_ProcessEventRegister(pTaskEventHandlerFn f) {
  (void)f;
  return 0;
}
void rbp_server_on_pair_prompt(rbp_server_t *s, uint8_t m, uint32_t k,
                               uint32_t n) {
  (void)s;
  assert(m==RBP_PROMPT_DISPLAY_PASSKEY && k==123456 && n==30000);
}
void rbp_server_on_scan_candidate(rbp_server_t *s, const rbp_candidate_t *c) {
  (void)s;
  scan_reports++;scan_last=*c;
}
uint8_t rc003_adapter_match(const char *n, uint8_t l, bool h) {
  (void)n;
  (void)l;
  (void)h;
  return 0;
}
bool rc003_adapter_mic_stop(rc003_adapter_t *a) {
  (void)a;
  abort();
}
uint32_t tmos_rand(void) { return 123456; }
bStatus_t tmos_set_event(tmosTaskID t, tmosEvents e) {
  (void)t;
  (void)e;
  return SUCCESS;
}
void wch_gatt_on_msg(void *p) {
  (void)p;
  abort();
}
void wch_gatt_set_adapter(rc003_adapter_t *a) { (void)a; }
