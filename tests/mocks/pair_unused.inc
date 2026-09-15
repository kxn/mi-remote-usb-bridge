/* Fail fast if this commit-path test unexpectedly enters radio code. */
bStatus_t GAPBondMgr_PasscodeRsp( uint16_t connectionHandle, uint8_t status, uint32_t passcode ) {abort();}
bStatus_t GAPBondMgr_SetParameter( uint16_t param, uint8_t len, void *pValue ) {abort();}
bStatus_t GAPRole_CentralStartDevice( uint8_t taskid, gapBondCBs_t *pCB, gapCentralRoleCB_t *pAppCallbacks ) {abort();}
bStatus_t GATT_InitClient( void ) {abort();}
void GATT_RegisterForInd( uint8_t taskId ) {abort();}
void GATT_bm_free( gattMsg_t *pMsg, uint8_t opcode ) {abort();}
tmosTaskID TMOS_ProcessEventRegister( pTaskEventHandlerFn eventCb ) {abort();}
bool board_begin_forget(void) {abort();}
uint8_t linkDB_State( uint16_t connectionHandle, uint8_t state ) {abort();}
void rbp_server_on_forget_done(rbp_server_t *s, uint16_t status, bool uncertain) {abort();}
void rbp_server_on_link_message(rbp_server_t *s, const char *msg) {abort();}
void rbp_server_on_pair_prompt(rbp_server_t *s, uint8_t method, uint32_t number,
                               uint32_t remaining_ms) {abort();}
void rbp_server_on_scan_candidate(rbp_server_t *s, const rbp_candidate_t *cand) {abort();}
void rbp_server_on_scan_done(rbp_server_t *s, uint8_t reason ,
                             uint32_t now_ms) {abort();}
bool rbp_server_should_reconnect(const rbp_server_t *s) {abort();}
void rc003_adapter_detach(rc003_adapter_t *a) {abort();}
uint8_t rc003_adapter_match(const char *name, uint8_t name_len, bool hid_uuid_in_adv) {abort();}
bool rc003_adapter_mic_stop(rc003_adapter_t *a) {abort();}
void rc003_adapter_start(rc003_adapter_t *a, uint32_t now_ms) {abort();}
uint32_t tmos_rand( void ) {abort();}
bStatus_t tmos_set_event( tmosTaskID taskID, tmosEvents event ) {abort();}
void wch_gatt_on_msg(void *pMsg) {abort();}
void wch_gatt_set_conn(uint16_t conn_handle) {abort();}
void wch_gatt_set_adapter(rc003_adapter_t *a){(void)a;abort();}
