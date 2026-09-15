/* Actual WCH bridge, SDK public types and asynchronous message shapes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/wch/wch_gatt.c"
uint8_t wch_central_task_id=1;
static unsigned allocs,frees,discovered,commands,cfms,done,reads;
static bool fail,chain,detach_in_callback;static uint16_t offsets[8];static bool completed[8];
static unsigned bearer_errors,procedure_errors,notifications;
static unsigned char_events;static uint8_t submit_error;
bStatus_t GATT_ExchangeMTU(uint16_t c,attExchangeMTUReq_t*r,uint8_t t){(void)c;(void)r;(void)t;return SUCCESS;}
bStatus_t GATT_DiscPrimaryServiceByUUID(uint16_t c,uint8_t*u,uint8_t n,uint8_t t){(void)c;(void)u;(void)n;(void)t;return SUCCESS;}
bStatus_t GATT_DiscCharsByUUID(uint16_t c,attReadByTypeReq_t*r,uint8_t t){(void)c;(void)t;assert(r->type.len==2);discovered++;return SUCCESS;}
bStatus_t GATT_DiscAllCharDescs(uint16_t c,uint16_t s,uint16_t e,uint8_t t){(void)c;(void)s;(void)e;(void)t;return SUCCESS;}
bStatus_t GATT_ReadCharValue(uint16_t c,attReadReq_t*r,uint8_t t){(void)c;(void)r;(void)t;return submit_error;}
bStatus_t GATT_ReadLongCharValue(uint16_t c,attReadBlobReq_t*r,uint8_t t){(void)c;(void)r;(void)t;return SUCCESS;}
void *GATT_bm_alloc(uint16_t c,uint8_t op,uint16_t n,uint16_t*a,uint8_t f){(void)c;(void)op;(void)a;(void)f;allocs++;return malloc(n);}
void GATT_bm_free(gattMsg_t*m,uint8_t op){(void)op;frees++;free(((attWriteReq_t*)m)->pValue);}
bStatus_t GATT_WriteCharValue(uint16_t c,attWriteReq_t*r,uint8_t t){(void)c;(void)t;assert(!r->cmd&&r->len==2);if(fail)return 1;GATT_bm_free((gattMsg_t*)r,ATT_WRITE_REQ);return SUCCESS;}
bStatus_t GATT_WriteNoRsp(uint16_t c,attWriteReq_t*r){(void)c;assert(r->cmd);commands++;if(fail)return 1;GATT_bm_free((gattMsg_t*)r,ATT_WRITE_CMD);return SUCCESS;}
bStatus_t ATT_HandleValueCfm(uint16_t c){(void)c;cfms++;return SUCCESS;}
void rc003_adapter_on_gatt(rc003_adapter_t*a,const rbp_gatt_evt_t*e){
    (void)a;
    if(e->type==RBP_GATT_EVT_NOTIFY)notifications++;
    if(e->type==RBP_GATT_EVT_BEARER_FAILED)bearer_errors++;
    if(e->type==RBP_GATT_EVT_PROC_ERROR)procedure_errors++;
    if(e->type==RBP_GATT_EVT_MTU_UPDATED&&chain)assert(op_disc_service16(NULL,0x1812)==0);
    if(e->type==RBP_GATT_EVT_WRITE_DONE&&chain)assert(op_read_value(NULL,30)==0);
    if(e->type==RBP_GATT_EVT_PROC_DONE)done++;
    if(e->type==RBP_GATT_EVT_READ_RSP){assert(reads<8);offsets[reads]=e->offset;completed[reads++]=e->proc_complete;}
    if(e->type==RBP_GATT_EVT_CHARS_FOUND){char_events++;if(detach_in_callback)wch_gatt_set_conn(8);}
}
int main(void){
    wch_gatt_set_conn(7);chain=true;assert(op_exchange_mtu(NULL,247)==0);
    gattMsgEvent_t msg={0};msg.connHandle=7;msg.method=ATT_EXCHANGE_MTU_RSP;msg.msg.exchangeMTURsp.serverRxMTU=247;
    wch_gatt_on_msg(&msg);assert(cur_op==GOP_DISC_SVC&&done==0);
    msg.method=ATT_ERROR_RSP;msg.msg.errorRsp.reqOpcode=ATT_FIND_BY_TYPE_VALUE_REQ;msg.msg.errorRsp.handle=1;msg.msg.errorRsp.errCode=ATT_ERR_ATTR_NOT_FOUND;wch_gatt_on_msg(&msg);assert(cur_op==GOP_NONE);
    assert(op_read_chars16(NULL,1,100,0x2a4b)==0&&discovered==1);
    /* Exact peer error from hardware-031-debug-run2.jsonl, record 310.
     * It belongs to Protocol Mode Write Command, not this Map discovery. */
    msg.method=ATT_ERROR_RSP;msg.msg.errorRsp.reqOpcode=0x52;
    msg.msg.errorRsp.handle=0x005b;msg.msg.errorRsp.errCode=0x0d;
    uint32_t before_generation=generation;
    wch_gatt_on_msg(&msg);assert(cur_op==GOP_FAILED&&done==0&&generation!=before_generation&&bearer_errors==1);
    assert(op_read_value(NULL,30)==-1&&op_command(NULL,91,(const uint8_t*)"x",1)==-1);
    wch_gatt_set_conn(7);assert(op_read_chars16(NULL,1,100,0x2a4b)==0);
    msg.msg.errorRsp.reqOpcode=ATT_READ_BY_TYPE_REQ;msg.msg.errorRsp.errCode=ATT_ERR_ATTR_NOT_FOUND;
    wch_gatt_on_msg(&msg);assert(cur_op==GOP_NONE&&done==1);
    uint8_t value[]={1,0};fail=true;assert(op_write(NULL,20,value,2)<0&&allocs==frees);
    fail=false;assert(op_write(NULL,20,value,2)==0);
    msg.method=ATT_WRITE_RSP;wch_gatt_on_msg(&msg);assert(cur_op==GOP_READ_VALUE&&done==1);
    msg.method=ATT_READ_RSP;msg.msg.readRsp.pValue=value;msg.msg.readRsp.len=2;wch_gatt_on_msg(&msg);
    assert(cur_op==GOP_NONE&&reads==1&&completed[0]);
    assert(op_read_long(NULL,30,246)==0);
    msg.method=ATT_READ_BLOB_RSP;msg.hdr.status=SUCCESS;msg.msg.readBlobRsp.pValue=value;msg.msg.readBlobRsp.len=2;wch_gatt_on_msg(&msg);
    assert(offsets[1]==246&&!completed[1]&&cur_op==GOP_READ_LONG);
    msg.hdr.status=bleProcedureComplete;msg.msg.readBlobRsp.len=0;wch_gatt_on_msg(&msg);
    assert(offsets[2]==248&&completed[2]&&cur_op==GOP_NONE);
    assert(op_command(NULL,20,value,2)==0&&cur_op==GOP_NONE&&allocs==frees);
    fail=true;assert(op_command(NULL,20,value,2)<0&&allocs==frees&&commands==2);
    msg.hdr.status=SUCCESS;msg.method=ATT_HANDLE_VALUE_IND;msg.msg.handleValueNoti.len=0;wch_gatt_on_msg(&msg);assert(cfms==1);
    submit_error=blePending;assert(op_read_value(NULL,30)==RBP_GATT_RETRY&&cur_op==GOP_NONE);
    submit_error=bleNotConnected;assert(op_read_value(NULL,30)==-1&&cur_op==GOP_NONE);submit_error=0;
    assert(op_read_long(NULL,30,246)==0);
    msg.method=ATT_ERROR_RSP;msg.msg.errorRsp.reqOpcode=ATT_READ_BLOB_REQ;msg.msg.errorRsp.handle=30;msg.msg.errorRsp.errCode=ATT_ERR_INVALID_OFFSET;wch_gatt_on_msg(&msg);
    assert(cur_op==GOP_NONE&&offsets[3]==246&&completed[3]);
    /* Consumer detach during first result invalidates remaining batch and
     * terminal event; neither may be delivered into the new generation. */
    assert(op_read_chars16(NULL,1,100,0x2a4d)==0);detach_in_callback=true;
    uint8_t pairs[]={1,0,0x12,2,0,0x4d,0x2a,3,0,0x12,4,0,0x4d,0x2a};
    unsigned old_done=done;msg.method=ATT_READ_BY_TYPE_RSP;msg.hdr.status=bleProcedureComplete;
    msg.msg.readByTypeRsp.numPairs=2;msg.msg.readByTypeRsp.len=7;msg.msg.readByTypeRsp.pDataList=pairs;
    wch_gatt_on_msg(&msg);assert(char_events==1&&done==old_done&&cur_op==GOP_NONE&&conn_handle==8);
    /* All procedure families reject a foreign error, then refuse new work
     * until physical link reset. Reproduce SDK reset + queued completion. */
    const uint8_t reqs[]={0,ATT_EXCHANGE_MTU_REQ,ATT_FIND_BY_TYPE_VALUE_REQ,ATT_READ_BY_TYPE_REQ,ATT_FIND_INFO_REQ,ATT_READ_REQ,ATT_READ_BLOB_REQ,ATT_WRITE_REQ};
    for(unsigned op=GOP_EXCHANGE_MTU;op<=GOP_WRITE;op++) {
        wch_gatt_set_conn(7);cur_op=op;pending_start=20;pending_end=40;
        unsigned errors=bearer_errors;msg.connHandle=7;msg.hdr.status=SUCCESS;msg.method=ATT_ERROR_RSP;
        msg.msg.errorRsp.reqOpcode=0x52;msg.msg.errorRsp.handle=91;msg.msg.errorRsp.errCode=13;
        wch_gatt_on_msg(&msg);assert(cur_op==GOP_FAILED&&bearer_errors==errors+1);
        msg.method=reqs[op]+1;msg.hdr.status=bleProcedureComplete;
        wch_gatt_on_msg(&msg);assert(cur_op==GOP_FAILED&&bearer_errors==errors+1);
        wch_gatt_set_conn(7);cur_op=op;msg.hdr.status=bleTimeout;
        wch_gatt_on_msg(&msg);assert(cur_op==GOP_FAILED&&bearer_errors==errors+2);
        /* Matching ATT errors still belong to that procedure. */
        wch_gatt_set_conn(7);cur_op=op;pending_start=20;pending_end=40;
        unsigned pe=procedure_errors;msg.hdr.status=SUCCESS;msg.method=ATT_ERROR_RSP;
        msg.msg.errorRsp.reqOpcode=reqs[op];msg.msg.errorRsp.handle=20;msg.msg.errorRsp.errCode=13;
        wch_gatt_on_msg(&msg);assert(cur_op==GOP_NONE&&procedure_errors==pe+1&&bearer_errors==errors+2);
        /* Previous physical connection cannot poison a new connection. */
        wch_gatt_set_conn(8);cur_op=op;msg.connHandle=7;msg.hdr.status=bleTimeout;
        wch_gatt_on_msg(&msg);assert(cur_op==op&&bearer_errors==errors+2);
    }
    wch_gatt_set_conn(7);assert(op_read_value(NULL,30)==0);
    unsigned errors=bearer_errors;msg.connHandle=7;msg.hdr.status=SUCCESS;msg.method=ATT_ERROR_RSP;
    msg.msg.errorRsp.reqOpcode=ATT_READ_REQ;msg.msg.errorRsp.handle=31;msg.msg.errorRsp.errCode=13;
    wch_gatt_on_msg(&msg);assert(cur_op==GOP_FAILED&&bearer_errors==errors+1);
    wch_gatt_set_conn(7);assert(op_read_value(NULL,30)==0);
    unsigned submitted=commands;assert(op_command(NULL,91,(const uint8_t*)"x",1)==RBP_GATT_RETRY&&commands==submitted);
    wch_gatt_quiesce();assert(op_read_value(NULL,30)==-1);
    unsigned old_reads=reads;msg.method=ATT_READ_RSP;msg.msg.readRsp.len=0;
    wch_gatt_on_msg(&msg);assert(reads==old_reads&&cur_op==GOP_FAILED);
    wch_gatt_set_conn(GAP_CONNHANDLE_INIT);assert(op_read_value(NULL,30)==-1);
    /* Failed SDK notification must not expose its payload or confirm it. */
    wch_gatt_set_conn(7);memset(&msg,0,sizeof msg);msg.connHandle=7;
    msg.method=ATT_HANDLE_VALUE_IND;msg.hdr.status=bleMemAllocError;
    msg.msg.handleValueNoti.len=1;msg.msg.handleValueNoti.pValue=(uint8_t*)1;
    unsigned saved_notifications=notifications,saved_cfms=cfms,saved_errors=bearer_errors;
    wch_gatt_on_msg(&msg);
    assert(notifications==saved_notifications && cfms==saved_cfms && bearer_errors==saved_errors+1);
    wch_gatt_set_conn(7);msg.hdr.status=SUCCESS;msg.msg.handleValueNoti.pValue=NULL;
    wch_gatt_on_msg(&msg);assert(notifications==saved_notifications && cfms==saved_cfms);
    wch_gatt_set_conn(7);msg.msg.handleValueNoti.len=21;msg.msg.handleValueNoti.pValue=value;
    wch_gatt_on_msg(&msg);assert(notifications==saved_notifications && cfms==saved_cfms);
    wch_gatt_set_conn(7);msg.msg.handleValueNoti.len=2;
    wch_gatt_on_msg(&msg);assert(notifications==saved_notifications+1 && cfms==saved_cfms+1);
    /* No malformed borrowed SDK buffer may reach pointer arithmetic. */
    unsigned before_bad=bearer_errors;
    wch_gatt_set_conn(7);assert(op_disc_service16(NULL,0x1812)==0);
    memset(&msg,0,sizeof msg);msg.connHandle=7;msg.method=ATT_FIND_BY_TYPE_VALUE_RSP;
    msg.msg.findByTypeValueRsp.numInfo=1;wch_gatt_on_msg(&msg);
    assert(bearer_errors==++before_bad && cur_op==GOP_FAILED);
    wch_gatt_set_conn(7);assert(op_disc_descs(NULL,1,100)==0);
    msg.method=ATT_FIND_INFO_RSP;msg.msg.findInfoRsp.format=1;msg.msg.findInfoRsp.numInfo=1;msg.msg.findInfoRsp.pInfo=NULL;
    wch_gatt_on_msg(&msg);assert(bearer_errors==++before_bad && cur_op==GOP_FAILED);
    wch_gatt_set_conn(7);assert(op_read_value(NULL,30)==0);
    msg.method=ATT_READ_RSP;msg.msg.readRsp.len=1;msg.msg.readRsp.pValue=NULL;
    wch_gatt_on_msg(&msg);assert(bearer_errors==++before_bad && cur_op==GOP_FAILED);
    wch_gatt_set_conn(7);assert(op_read_long(NULL,30,65535)==0);
    uint8_t byte=0;msg.method=ATT_READ_BLOB_RSP;msg.msg.readBlobRsp.len=1;msg.msg.readBlobRsp.pValue=&byte;
    wch_gatt_on_msg(&msg);assert(bearer_errors==++before_bad && cur_op==GOP_FAILED);
    puts("WCH GATT: discovery API, callback reentrancy, allocation ownership, long-read boundary, commands passed");return 0;
}
