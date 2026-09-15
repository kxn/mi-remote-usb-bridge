"""Opt-in development extension. Not required by the product RBP interface."""
import struct
from .wire import parse_tlv
CONFIG=0x7f01
EVENT=0x7f00
MODULES=['system','usb','session','ble','gatt','hid','atvv','storage']
CODES={
 ('system',1):'boot_reset',('system',2):'stack_unused_and_tick_delta',
 ('system',3):'receive_processing_ticks',('system',4):'sdk_status_error',
 ('system',5):'sdk_rx_check_failed',
 ('system',6):'sdk_rx_hardware_after_check',
 ('system',7):'sdk_rx_packet_metadata',
 ('system',8):'sdk_rx_arm_context',('system',9):'sdk_rx_snapshot_overflow',
 ('usb',1):'transport_reset',('usb',2):'bus_reset',('usb',3):'suspend_resume',('usb',4):'rx_overflow',
 ('usb',5):'bulk_out_received',('usb',6):'setup_request',
 ('session',1):'created',('session',2):'teardown',('session',3):'heartbeat_timeout',
 ('session',7):'request_frame_received',
 ('session',8):'first_audio_timeout',
 ('session',4):'parser_timeout',('session',5):'voice_end',('session',6):'queue_metrics',
 ('ble',1):'gap_event',('ble',2):'candidate_filter',('ble',3):'link_terminated',('ble',4):'pair_state',('ble',10):'advertising_name',
 ('gatt',1):'api_result',('gatt',2):'sdk_message',('gatt',3):'procedure_error',
 ('gatt',4):'notification_origin',
 ('gatt',5):'request_arguments',('gatt',6):'att_error_response',
 ('gatt',7):'characteristic_batch',('gatt',10):'request_bytes',
 ('gatt',8):'bearer_failed',
 ('ble',5):'data_length_changed',
 ('ble',6):'smp_identity_address_type',
 ('ble',7):'reconnect_address_match',
 ('ble',8):'central_state_transition',('ble',9):'radio_fault',
 ('ble',11):'radio_api_result',('ble',12):'bond_hid_capabilities',
 ('ble',13):'local_identity_address',('ble',14):'local_irk_persistence',
 ('ble',15):'directed_advertising_target',('ble',16):'connection_parameters',
 ('ble',17):'connection_parameters_updated',('ble',18):'connection_update_status',
 ('hid',12):'hid_information',
 ('hid',13):'bond_cache_restore',('hid',14):'bond_cache_invalidated',
 ('hid',1):'step_started',('hid',2):'gatt_event',('hid',3):'initialization_failed',('hid',4):'api_rejected',('hid',10):'attribute_bytes',
 ('hid',5):'input_notification',('hid',6):'input_rejected',
 ('atvv',1):'control',('atvv',2):'audio_progress',('atvv',3):'fault',('atvv',4):'mic_open_rejected',('atvv',10):'control_bytes',
 ('storage',9):'sdk_flash_read',('storage',11):'sdk_flash_erase',('storage',12):'sdk_flash_write',
 ('storage',4):'bond_identity_read',('storage',5):'bond_identity_invalid',
 ('storage',6):'bond_irk_status',('storage',7):'pair_rollback',
 ('storage',8):'peer_commit_result',('storage',10):'bond_identity_bytes',
 ('storage',1):'write_begin',('storage',2):'write_complete',('storage',3):'write_failed',
}
def validate_response(payload):return parse_tlv(payload,schema={1:3,2:3,3:3},required=(1,2,3))
def decode(payload):
    if len(payload)<12 or (len(payload)-12)%28:raise ValueError('debug record length')
    version,dropped,highwater=struct.unpack_from('<III',payload)
    if version!=1:raise ValueError('debug version')
    records=[]
    for offset in range(12,len(payload),28):
        seq,ms,module,level,code,a,b,c,d=struct.unpack_from('<IIBBHIIII',payload,offset)
        name=MODULES[module] if module<len(MODULES) else str(module)
        r=dict(sequence=seq,board_ms=ms,module=name,level=level,code=code,
               name=CODES.get((name,code),'unknown'),args=[a,b,c,d])
        if name=='ble' and code==8:
            states=('idle','discovery','initiating','connected','wake_scan','scan_stopping','initiation_canceling','disconnecting')
            r.update(previous_state=states[a] if a<len(states) else a,
                     next_state=states[b] if b<len(states) else b,
                     pending_intent={0:'none',1:'pair',2:'reconnect'}.get(c,c),timeout_ms=d)
        if name=='ble' and code==11:
            r.update(api={1:'cancel_discovery',2:'establish_link',3:'start_discovery',4:'cancel_initiation',
                          5:'terminate_link',6:'set_privacy_mode',7:'start_wake_scan',8:'set_local_address_type'}.get(a,a),status=b)
        if name=='ble' and code in (13,15):
            addr=struct.pack('<IH',a,b&65535)
            r.update(address=':'.join(f'{v:02X}' for v in reversed(addr)),address_type=(b>>16)&255)
            if code==13:r.update(read_status=c)
            else:r.update(target_matches_local_identity={0:False,1:True}.get(c),rssi=d-256 if d>=128 else d)
        if name=='ble' and code==14:
            r.update(live_read_status=a,nv_read_status=b,live_matches_nv=bool(c),live_key_nonzero=bool(d))
        if name=='ble' and code in (16,17):
            r.update(handle=a,interval_ms=b*1.25,latency=c,supervision_timeout_ms=d*10)
        if name=='system' and code==3:
            r.update(receive_processing_ms=a*0.625,output_flush_ms=b*0.625)
        if name=='system' and code==4:
            r.update(sdk_code=a,sdk_status=b)
            if c or d:r.update(rx_checks=c,rx_crc_failures=d)
        if name=='system' and code==5:
            r.update(rx_check_result=a,encrypted_check=b,precheck_state=c,rx_check_sequence=d)
        if name=='system' and code==6:
            r.update(rx_check_sequence=a,aes_control_after=b,bb_status_after=c,ip_state_after=d)
        if name=='system' and code==7:
            r.update(rx_check_sequence=a,checked_tail_status=None if c==0xffffffff else c,caller_pc=d)
            if b!=0xffffffff:r.update(ll_header=b&255,ll_length=(b>>8)&255,llid=b&3,nesn=(b>>2)&1,sn=(b>>3)&1)
        if name=='system' and code==8:
            r.update(rx_check_sequence=a,arm_sequence=b,arm_ip_state=c&255,arm_direction=(c>>8)&255,crc_failures_at_poll=d)
        if name=='system' and code==9:
            r.update(snapshot_dropped=a,rx_checks=b,rx_crc_failures=c,arm_sequence=d)
        if code==10:r.update(blob_offset=a&65535,blob_total=a>>16,bytes=struct.pack('<III',b,c,d)[:min(12,max(0,(a>>16)-(a&65535)))].hex())
        records.append(r)
    return dict(dropped=dropped,highwater=highwater,records=records)
