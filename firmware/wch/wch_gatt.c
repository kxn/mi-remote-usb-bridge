#include "../product/faults.h"
#include "wch_gatt.h"
#include "rc003_adapter.h"
#include "CH58xBLE_LIB.h"
#include <string.h>
#include "../debug/trace.h"

extern uint8_t wch_central_task_id; /* defined in wch_central.c */
#define gatt_task_id wch_central_task_id

static uint16_t conn_handle=GAP_CONNHANDLE_INIT;
static uint16_t neg_mtu = ATT_MTU_SIZE;
static rc003_adapter_t *adapter; /* set by main() after adapter init */

typedef enum {
    GOP_NONE = 0,
    GOP_EXCHANGE_MTU,
    GOP_DISC_SVC,
    GOP_READ_CHARS,
    GOP_DISC_DESCS,
    GOP_READ_VALUE,
    GOP_READ_LONG,
    GOP_WRITE,
    GOP_FAILED,
} gop_t;

/* TMOS task-owned, never touched by radio/USB ISRs. generation detects
 * synchronous adapter callback reentry; volatile would not provide that. */
static uint8_t cur_op = GOP_FAILED;
static uint32_t generation;
static uint16_t read_offset;
static bool found_service;
static uint16_t pending_start, pending_end;
static int rejected(gop_t op,uint8_t status) {
    (void)op;
    DT(DT_GATT,status?DT_ERROR:DT_DETAIL,1,op,status,conn_handle,generation);
    if(status==SUCCESS)return 0;
    rbp_fault_record(RBP_FAULT_GATT_API,op,status,0);
    if(status==blePending || status==bleMemAllocError || status==bleNoResources ||
       status==MSG_BUFFER_NOT_AVAIL)return RBP_GATT_RETRY;
    return -1;
}

void wch_gatt_set_adapter(rc003_adapter_t *a) { adapter = a; }
void wch_gatt_set_conn(uint16_t h) { conn_handle = h; cur_op=h==GAP_CONNHANDLE_INIT?GOP_FAILED:GOP_NONE; neg_mtu=ATT_MTU_SIZE; generation++; }
void wch_gatt_quiesce(void) {cur_op=GOP_FAILED;generation++;}
uint16_t wch_gatt_conn(void) { return conn_handle; }

static void emit(rbp_gatt_evt_t *evt) { rc003_adapter_on_gatt(adapter, evt); }

static void bearer_failed(uint8_t status) {
    rbp_fault_record(RBP_FAULT_GATT_EVENT,cur_op,status,0);
    rbp_gatt_evt_t e;memset(&e,0,sizeof e);
    DT(DT_GATT,DT_ERROR,8,cur_op,status,conn_handle,generation);
    generation++;cur_op=GOP_FAILED;
    e.type=RBP_GATT_EVT_BEARER_FAILED;e.status=status;emit(&e);
}

static void emit_done(void)
{
    rbp_gatt_evt_t e;
    memset(&e, 0, sizeof(e));
    e.type = RBP_GATT_EVT_PROC_DONE;
    generation++;
    cur_op = GOP_NONE;
    emit(&e);
}

static void emit_error(uint8_t status)
{
    rbp_fault_record(RBP_FAULT_ATT,cur_op,status,0);
    DT(DT_GATT,DT_ERROR,3,cur_op,status,generation,0);
    rbp_gatt_evt_t e;
    memset(&e, 0, sizeof(e));
    e.type = RBP_GATT_EVT_PROC_ERROR;
    e.status = status;
    generation++;
    cur_op = GOP_NONE;
    emit(&e);
}

/* ---------------- rbp_gatt_client_t ops ---------------- */

static int op_exchange_mtu(void *user, uint16_t mtu)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    attExchangeMTUReq_t req;
    req.clientRxMTU = mtu;
    DT(DT_GATT,DT_DETAIL,5,GOP_EXCHANGE_MTU,mtu,0,generation);
    int result=rejected(GOP_EXCHANGE_MTU,GATT_ExchangeMTU(conn_handle, &req, gatt_task_id));
    if(result)
        return result;
    generation++;
    cur_op = GOP_EXCHANGE_MTU;
    return 0;
}

static int op_disc_service(void *user, const uint8_t *uuid, uint8_t len)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    DT(DT_GATT,DT_DETAIL,5,GOP_DISC_SVC,len,0,generation);
    DT_BLOB(DT_GATT,10,uuid,len);
    int result=rejected(GOP_DISC_SVC,GATT_DiscPrimaryServiceByUUID(conn_handle, (uint8_t *)uuid, len,
                                      gatt_task_id));
    if(result)
        return result;
    generation++;
    found_service=false;
    pending_start=1;pending_end=0xffff;
    cur_op = GOP_DISC_SVC;
    return 0;
}

static int op_disc_service16(void *user, uint16_t uuid16)
{
    uint8_t u[2] = { (uint8_t)uuid16, (uint8_t)(uuid16 >> 8) };
    return op_disc_service(user, u, 2);
}

static int op_disc_service128(void *user, const uint8_t *uuid)
{
    return op_disc_service(user, uuid, 16);
}

static int op_read_chars(void *user, uint16_t start, uint16_t end,
                         const uint8_t *uuid, uint8_t len)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    attReadByTypeReq_t req;
    req.startHandle = start;
    req.endHandle = end;
    req.type.len = len;
    memcpy(req.type.uuid, uuid, len);
    DT(DT_GATT,DT_DETAIL,5,GOP_READ_CHARS,start,end,generation);
    DT_BLOB(DT_GATT,10,uuid,len);
    int result=rejected(GOP_READ_CHARS,GATT_DiscCharsByUUID(conn_handle, &req, gatt_task_id));
    if(result)
        return result;
    generation++;
    pending_start=start;pending_end=end;
    cur_op = GOP_READ_CHARS;
    return 0;
}

static int op_read_chars16(void *user, uint16_t start, uint16_t end, uint16_t uuid16)
{
    uint8_t u[2] = { (uint8_t)uuid16, (uint8_t)(uuid16 >> 8) };
    return op_read_chars(user, start, end, u, 2);
}

static int op_read_chars128(void *user, uint16_t start, uint16_t end,
                            const uint8_t *uuid)
{
    return op_read_chars(user, start, end, uuid, 16);
}

static int op_disc_descs(void *user, uint16_t start, uint16_t end)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    DT(DT_GATT,DT_DETAIL,5,GOP_DISC_DESCS,start,end,generation);
    int result=rejected(GOP_DISC_DESCS,GATT_DiscAllCharDescs(conn_handle, start, end, gatt_task_id));
    if(result)
        return result;
    generation++;
    pending_start=start;pending_end=end;
    cur_op = GOP_DISC_DESCS;
    return 0;
}

static int op_read_value(void *user, uint16_t handle)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    attReadReq_t req;
    req.handle = handle;
    DT(DT_GATT,DT_DETAIL,5,GOP_READ_VALUE,handle,0,generation);
    int result=rejected(GOP_READ_VALUE,GATT_ReadCharValue(conn_handle, &req, gatt_task_id));
    if(result)
        return result;
    generation++;
    pending_start=pending_end=handle;
    cur_op = GOP_READ_VALUE;
    return 0;
}

static int op_read_long(void *user, uint16_t handle, uint16_t offset)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    attReadBlobReq_t req;
    req.handle = handle;
    req.offset = offset;
    DT(DT_GATT,DT_DETAIL,5,GOP_READ_LONG,handle,offset,generation);
    int result=rejected(GOP_READ_LONG,GATT_ReadLongCharValue(conn_handle, &req, gatt_task_id));
    if(result)
        return result;
    generation++;
    read_offset=offset;
    pending_start=pending_end=handle;
    cur_op = GOP_READ_LONG;
    return 0;
}

static int op_write(void *user, uint16_t handle, const uint8_t *val, uint16_t len)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if (cur_op != GOP_NONE) return RBP_GATT_RETRY;
    attWriteReq_t req;
    req.handle = handle;
    req.len = len;
    req.pValue = GATT_bm_alloc(conn_handle, ATT_WRITE_REQ, len, NULL, 0);
    if (!req.pValue) {rbp_fault_record(RBP_FAULT_GATT_API,GOP_WRITE,bleMemAllocError,0);return RBP_GATT_RETRY;}
    memcpy(req.pValue, val, len);
    req.sig = 0;
    req.cmd = 0;
    DT(DT_GATT,DT_DETAIL,5,GOP_WRITE,handle,len,generation);
    DT_BLOB(DT_GATT,10,val,len);
    int result=rejected(GOP_WRITE,GATT_WriteCharValue(conn_handle, &req, gatt_task_id));
    if(result) {
        GATT_bm_free((gattMsg_t *)&req, ATT_WRITE_REQ);
        return result;
    }
    generation++;
    pending_start=pending_end=handle;
    cur_op = GOP_WRITE;
    return 0;
}

static int op_command(void *user, uint16_t handle, const uint8_t *val, uint16_t len)
{
    (void)user;
    if(cur_op==GOP_FAILED)return -1;
    if(cur_op!=GOP_NONE)return RBP_GATT_RETRY;
    if (!val || len > neg_mtu-3) return -1;
    attWriteReq_t req; memset(&req,0,sizeof req);
    req.handle=handle;req.len=len;req.cmd=1;
    req.pValue=GATT_bm_alloc(conn_handle,ATT_WRITE_CMD,len,NULL,0);
    if(!req.pValue){rbp_fault_record(RBP_FAULT_GATT_API,GOP_NONE,bleMemAllocError,0);return RBP_GATT_RETRY;}
    memcpy(req.pValue,val,len);
    DT(DT_GATT,DT_DETAIL,5,GOP_NONE,handle,len,generation);
    DT_BLOB(DT_GATT,10,val,len);
    int result=rejected(GOP_NONE,GATT_WriteNoRsp(conn_handle,&req));
    if(result) {
        GATT_bm_free((gattMsg_t *)&req,ATT_WRITE_CMD);return result;
    }
    return 0;
}

static const rbp_gatt_client_t k_gatt = {
    NULL,
    op_exchange_mtu,
    op_disc_service16,
    op_disc_service128,
    op_read_chars16,
    op_read_chars128,
    op_disc_descs,
    op_read_value,
    op_read_long,
    op_write,
    op_command,
};

const rbp_gatt_client_t *wch_gatt_client(void) { return &k_gatt; }

/* ---------------- message translation ---------------- */

void wch_gatt_on_msg(void *p)
{
    gattMsgEvent_t *pMsg = (gattMsgEvent_t *)p;
    rbp_gatt_evt_t e;
    gop_t op = cur_op;
    DT(DT_GATT,DT_DETAIL,2,op,pMsg->method,pMsg->hdr.status,pMsg->connHandle);
    if(pMsg->hdr.status!=SUCCESS && pMsg->hdr.status!=bleProcedureComplete)
        rbp_fault_record(RBP_FAULT_GATT_EVENT,pMsg->method,pMsg->hdr.status,op);
    uint32_t old_generation=generation;
    if(pMsg->connHandle!=conn_handle || conn_handle==GAP_CONNHANDLE_INIT || cur_op==GOP_FAILED)return;
    if(pMsg->method==ATT_ERROR_RSP && pMsg->hdr.status==SUCCESS) {
        if(pMsg->msg.errorRsp.errCode!=ATT_ERR_ATTR_NOT_FOUND)
            rbp_fault_record(RBP_FAULT_ATT,pMsg->msg.errorRsp.reqOpcode,pMsg->msg.errorRsp.errCode,op);
        /* Keep the peer's actual request opcode and handle. cur_op alone
         * cannot distinguish an error from an earlier Write Command. */
        DT(DT_GATT,DT_ERROR,6,pMsg->msg.errorRsp.reqOpcode,
           pMsg->msg.errorRsp.handle,pMsg->msg.errorRsp.errCode,
           ((uint32_t)op<<24)|(generation&0xffffffu));
        /* Commands have no ATT response. RC003 nevertheless sent an error
         * for 0x52 while a Read By Type was pending (hardware 2026-09-14).
         * An unrelated error must not complete that pending transaction. */
        uint8_t expected=0;
        switch(op) {
        case GOP_EXCHANGE_MTU: expected=ATT_EXCHANGE_MTU_REQ;break;
        case GOP_DISC_SVC: expected=ATT_FIND_BY_TYPE_VALUE_REQ;break;
        case GOP_READ_CHARS: expected=ATT_READ_BY_TYPE_REQ;break;
        case GOP_DISC_DESCS: expected=ATT_FIND_INFO_REQ;break;
        case GOP_READ_VALUE: expected=ATT_READ_REQ;break;
        case GOP_READ_LONG: expected=ATT_READ_BLOB_REQ;break;
        case GOP_WRITE: expected=ATT_WRITE_REQ;break;
        default: break;
        }
        if(!expected || pMsg->msg.errorRsp.reqOpcode!=expected ||
           (op!=GOP_EXCHANGE_MTU &&
            (pMsg->msg.errorRsp.handle<pending_start || pMsg->msg.errorRsp.handle>pending_end))) {
            /* WCH gattProcessMultiReqs resets its transaction even for an
             * unrelated Error Response. Ignoring it here would hang until
             * timeout; replay could duplicate a write. Reset the bearer. */
            bearer_failed(pMsg->msg.errorRsp.errCode);return;
        }
    }
    if(pMsg->hdr.status==bleTimeout) {bearer_failed(bleTimeout);return;}

    /* notifications/indications are unrelated to the current procedure */
    if (pMsg->method == ATT_HANDLE_VALUE_NOTI ||
        pMsg->method == ATT_HANDLE_VALUE_IND) {
        /* Only successful SDK messages own a usable notification payload.
         * Do not expose failed or malformed message data to the adapter. */
        if(pMsg->hdr.status!=SUCCESS) {bearer_failed(pMsg->hdr.status);return;}
        if(pMsg->msg.handleValueNoti.len>neg_mtu-3 ||
           (pMsg->msg.handleValueNoti.len && !pMsg->msg.handleValueNoti.pValue)) {
            bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);return;
        }
        memset(&e, 0, sizeof(e));
        e.type = RBP_GATT_EVT_NOTIFY;
        e.handle = pMsg->msg.handleValueNoti.handle;
        e.value = pMsg->msg.handleValueNoti.pValue;
        e.len = pMsg->msg.handleValueNoti.len;
        DT(DT_GATT,DT_DETAIL,4,e.handle,e.len,pMsg->method,generation);
        if(pMsg->method==ATT_HANDLE_VALUE_IND) {
            uint8_t status=ATT_HandleValueCfm(conn_handle);
            if(status!=SUCCESS)rbp_fault_record(RBP_FAULT_GATT_API,9,status,0);
        }
        emit(&e);
        return;
    }

    bool complete = pMsg->hdr.status == bleProcedureComplete;
    if(pMsg->method==ATT_MTU_UPDATED_EVENT)return; /* stack side event */
    if(pMsg->method==ATT_FLOW_CTRL_VIOLATED_EVENT) {
        bearer_failed(ATT_ERR_INVALID_PDU);return;
    }
    if(op==GOP_NONE)return; /* no owner for a late completion */
    uint8_t response=0;
    switch(op) {
    case GOP_EXCHANGE_MTU:response=ATT_EXCHANGE_MTU_RSP;break;
    case GOP_DISC_SVC:response=ATT_FIND_BY_TYPE_VALUE_RSP;break;
    case GOP_READ_CHARS:response=ATT_READ_BY_TYPE_RSP;break;
    case GOP_DISC_DESCS:response=ATT_FIND_INFO_RSP;break;
    case GOP_READ_VALUE:response=ATT_READ_RSP;break;
    case GOP_READ_LONG:response=ATT_READ_BLOB_RSP;break;
    case GOP_WRITE:response=ATT_WRITE_RSP;break;
    default:break;
    }
    if((pMsg->method!=response && pMsg->method!=ATT_ERROR_RSP) ||
       (pMsg->hdr.status!=SUCCESS && !complete) ||
       (complete && op!=GOP_DISC_SVC && op!=GOP_READ_CHARS &&
        op!=GOP_DISC_DESCS && op!=GOP_READ_LONG)) {
        bearer_failed(ATT_ERR_INVALID_PDU);return;
    }

    /* SDK message payloads are borrowed buffers, not trusted C arrays.
     * Validate every variable-length response before pointer arithmetic or
     * emitting an event, including batched service/descriptor discovery. */
    if(pMsg->method==ATT_FIND_BY_TYPE_VALUE_RSP &&
       ((pMsg->msg.findByTypeValueRsp.numInfo && !pMsg->msg.findByTypeValueRsp.pHandlesInfo) ||
        pMsg->msg.findByTypeValueRsp.numInfo>(neg_mtu-1)/4)) {
        bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);return;
    }
    if(pMsg->method==ATT_FIND_INFO_RSP && pMsg->msg.findInfoRsp.numInfo) {
        uint8_t format=pMsg->msg.findInfoRsp.format;
        if((format!=1 && format!=2) || !pMsg->msg.findInfoRsp.pInfo ||
           pMsg->msg.findInfoRsp.numInfo>(neg_mtu-2)/(format==1?4:18)) {
            bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);return;
        }
    }
    if(pMsg->method==ATT_READ_RSP &&
       (pMsg->msg.readRsp.len>neg_mtu-1 || (pMsg->msg.readRsp.len && !pMsg->msg.readRsp.pValue))) {
        bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);return;
    }
    if(pMsg->method==ATT_READ_BLOB_RSP &&
       (pMsg->msg.readBlobRsp.len>neg_mtu-1 ||
        (pMsg->msg.readBlobRsp.len && !pMsg->msg.readBlobRsp.pValue) ||
        pMsg->msg.readBlobRsp.len>UINT16_MAX-read_offset)) {
        bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);return;
    }

    switch (op) {
    case GOP_EXCHANGE_MTU:
        if (pMsg->method == ATT_EXCHANGE_MTU_RSP) {
            neg_mtu = pMsg->msg.exchangeMTURsp.serverRxMTU;
            if(neg_mtu>247)neg_mtu=247;
            if(neg_mtu<23) {bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);return;}
            memset(&e, 0, sizeof(e));
            e.type = RBP_GATT_EVT_MTU_UPDATED;
            e.mtu = neg_mtu;
            cur_op=GOP_NONE;
            emit(&e);
            if(generation==old_generation) emit_done();
        } else if (pMsg->method == ATT_ERROR_RSP) {
            emit_error(pMsg->msg.errorRsp.errCode);
        }
        break;

    case GOP_DISC_SVC:
        if (pMsg->method == ATT_FIND_BY_TYPE_VALUE_RSP &&
            pMsg->msg.findByTypeValueRsp.numInfo > 0 && !found_service) {
            memset(&e, 0, sizeof(e));
            found_service=true;
            e.type = RBP_GATT_EVT_SERVICE_FOUND;
            e.svc_start = ATT_ATTR_HANDLE(pMsg->msg.findByTypeValueRsp.pHandlesInfo, 0);
            e.svc_end = ATT_GRP_END_HANDLE(pMsg->msg.findByTypeValueRsp.pHandlesInfo, 0);
            emit(&e);
            if(generation!=old_generation)return;
        }
        if (pMsg->method == ATT_FIND_BY_TYPE_VALUE_RSP && complete) {
            emit_done();
        } else if (pMsg->method == ATT_ERROR_RSP) {
            memset(&e, 0, sizeof(e));
            if(pMsg->msg.errorRsp.errCode!=ATT_ERR_ATTR_NOT_FOUND) {emit_error(pMsg->msg.errorRsp.errCode);break;}
            if(found_service) {emit_done();break;}
            e.type = RBP_GATT_EVT_SERVICE_NOT_FOUND;
            cur_op=GOP_NONE;emit(&e);
        }
        break;

    case GOP_READ_CHARS:
        if (pMsg->method == ATT_READ_BY_TYPE_RSP &&
            pMsg->msg.readByTypeRsp.numPairs > 0) {
            uint16_t pair_len = pMsg->msg.readByTypeRsp.len; /* bytes per pair */
            DT(DT_GATT,DT_DETAIL,7,pMsg->msg.readByTypeRsp.numPairs,pair_len,complete,generation);
            if((pair_len!=7 && pair_len!=21) || !pMsg->msg.readByTypeRsp.pDataList ||
               pMsg->msg.readByTypeRsp.numPairs>(neg_mtu-2)/pair_len) {
                bearer_failed(ATT_ERR_INVALID_VALUE_SIZE);break;
            }
            const uint8_t *d = pMsg->msg.readByTypeRsp.pDataList;
            for (uint8_t i = 0; i < pMsg->msg.readByTypeRsp.numPairs; i++) {
                memset(&e, 0, sizeof(e));
                e.type = RBP_GATT_EVT_CHARS_FOUND;
                e.handle = (uint16_t)(d[0] | (d[1] << 8));
                e.value = d + 2;
                e.len = (uint16_t)(pair_len > 2 ? pair_len - 2 : 0);
                e.proc_complete =
                    complete && (uint8_t)(i + 1) == pMsg->msg.readByTypeRsp.numPairs;
                emit(&e);
                if(generation!=old_generation)return;
                d += pair_len;
            }
        }
        if (pMsg->method == ATT_READ_BY_TYPE_RSP && complete) {
            emit_done();
        } else if (pMsg->method == ATT_ERROR_RSP) {
            if(pMsg->msg.errorRsp.errCode==ATT_ERR_ATTR_NOT_FOUND)emit_done();
            else emit_error(pMsg->msg.errorRsp.errCode);
        }
        break;

    case GOP_DISC_DESCS:
        if (pMsg->method == ATT_FIND_INFO_RSP &&
            pMsg->msg.findInfoRsp.numInfo > 0 &&
            pMsg->msg.findInfoRsp.format == 0x01) { /* handle + uuid16 */
            const uint8_t *d = pMsg->msg.findInfoRsp.pInfo;
            for (uint8_t i = 0; i < pMsg->msg.findInfoRsp.numInfo; i++) {
                memset(&e, 0, sizeof(e));
                e.type = RBP_GATT_EVT_DESC_FOUND;
                e.desc_handle = (uint16_t)(d[0] | (d[1] << 8));
                e.uuid16 = (uint16_t)(d[2] | (d[3] << 8));
                e.proc_complete = complete &&
                                  (uint8_t)(i + 1) == pMsg->msg.findInfoRsp.numInfo;
                emit(&e);
                if(generation!=old_generation)return;
                d += 4;
            }
        }
        if (pMsg->method == ATT_FIND_INFO_RSP && complete) {
            emit_done();
        } else if (pMsg->method == ATT_ERROR_RSP) {
            if(pMsg->msg.errorRsp.errCode==ATT_ERR_ATTR_NOT_FOUND)emit_done();
            else emit_error(pMsg->msg.errorRsp.errCode);
        }
        break;

    case GOP_READ_VALUE:
        if (pMsg->method == ATT_READ_RSP) {
            memset(&e, 0, sizeof(e));
            e.type = RBP_GATT_EVT_READ_RSP;
            e.value = pMsg->msg.readRsp.pValue;
            e.len = pMsg->msg.readRsp.len;
            e.offset = 0;
            e.proc_complete = e.len < neg_mtu-1;
            cur_op=GOP_NONE;
            emit(&e);

        } else if (pMsg->method == ATT_ERROR_RSP) {
            emit_error(pMsg->msg.errorRsp.errCode);
        }
        break;

    case GOP_READ_LONG:
        if (pMsg->method == ATT_READ_BLOB_RSP) {
            memset(&e, 0, sizeof(e));
            e.type = RBP_GATT_EVT_READ_RSP;
            e.value = pMsg->msg.readBlobRsp.pValue;
            e.len = pMsg->msg.readBlobRsp.len;
            e.offset=read_offset;read_offset+=e.len;
            e.proc_complete=complete;
            if(complete)cur_op=GOP_NONE;
            emit(&e);

        } else if (pMsg->method == ATT_ERROR_RSP) {
            if(pMsg->msg.errorRsp.errCode==ATT_ERR_INVALID_OFFSET && read_offset) {
                /* Value length exactly equals an MTU-1 boundary. */
                memset(&e,0,sizeof e);e.type=RBP_GATT_EVT_READ_RSP;
                e.offset=read_offset;e.proc_complete=true;cur_op=GOP_NONE;emit(&e);
            } else emit_error(pMsg->msg.errorRsp.errCode);
        }
        break;

    case GOP_WRITE:
        if (pMsg->method == ATT_WRITE_RSP) {
            memset(&e, 0, sizeof(e));
            e.type = RBP_GATT_EVT_WRITE_DONE;
            cur_op=GOP_NONE;
            emit(&e);
            if(generation==old_generation) emit_done();
        } else if (pMsg->method == ATT_ERROR_RSP) {
            emit_error(pMsg->msg.errorRsp.errCode);
        }
        break;

    case GOP_NONE:
    default:
        break;
    }
}
