"""Executable RBP/3.0 field schema. Export: python -m rbp.schema > schema.json.

Only logical product fields are public; no BLE handles; audio carries codec descriptors and encoded units.
"""
import json
from . import wire

def fields(spec):
    result={}
    for item in spec.split():
        tag,name,typ=item.split(':');optional=typ.endswith('?');typ=typ.rstrip('?')
        result[int(tag)]={'name':name,'type':{'u8':1,'u16':2,'u32':3,'u64':4,'bool':5,'text':6,'bytes':7}[typ],'required':not optional}
    return result

DEVICE=fields('1:connection_id:u32 2:peer_id:u32 3:state:u8 4:model_id:text 5:name:text 6:catalog_revision:u32 7:key_count:u8 8:voice_state:u8 9:sample_rate:u32 10:battery:u8 11:charging:u8 12:voice_enabled:bool 13:delivering:bool 14:stream_id:u32 15:waiting_idle:bool 16:reason:u16 17:message:text? 18:voice_interaction:u8 19:max_capture_ms:u32')
OPERATION=fields('1:operation_id:u32 2:state:u8 3:result:u16 4:peer_id:u32? 5:connection_id:u32? 6:uncertain:bool?')
REQUESTS={}
RESPONSES={}
def rpc(op,req='',resp=''):
    REQUESTS[op]=fields(req);RESPONSES[op]=fields(resp) if isinstance(resp,str) else resp
rpc(wire.OP_HELLO,'1:nonce:bytes','1:echo_nonce:bytes 2:bridge_uid:bytes 3:firmware:text 4:max_keys:u8 5:max_payload:u16 6:features:u32')
rpc(wire.OP_PING,'1:cookie:u32','1:cookie:u32 2:board_time_us:u64')
rpc(wire.OP_GET_DEVICE,resp=DEVICE)
rpc(wire.OP_GET_PEER,resp='1:peer_id:u32 2:name:text 3:auto_reconnect:bool')
rpc(wire.OP_GET_OPERATION,'1:operation_id:u32',OPERATION)
rpc(wire.OP_GET_STATS,resp='1:protocol_errors:u32 2:input_resets:u32 3:voice_overruns:u32 4:voice_errors:u32 5:reset_reason:u16 6:fault_sequence:u32? 7:fault_records:bytes? 8:fault_evictions:u32?')
rpc(wire.OP_GOODBYE)
rpc(wire.OP_FIND_START,'1:duration_ms:u16','1:search_id:u32')
rpc(wire.OP_FIND_LIST,'1:search_id:u32 2:cursor:u8','1:next_cursor:u8 2:entries:bytes')
rpc(wire.OP_FIND_STOP,'1:search_id:u32')
rpc(wire.OP_PAIR_BEGIN,'1:search_id:u32 2:candidate_id:u32','1:operation_id:u32')
rpc(wire.OP_PAIR_REPLY,'1:operation_id:u32 2:prompt_id:u32 3:accept:bool 4:passkey:u32?')
rpc(wire.OP_PAIR_CANCEL,'1:operation_id:u32')
rpc(wire.OP_FORGET_PEER,'1:peer_id:u32','1:operation_id:u32')
rpc(wire.OP_SET_RECONNECT,'1:peer_id:u32 2:enabled:bool')
rpc(wire.OP_CONNECT_PEER,'1:peer_id:u32')
rpc(wire.OP_DISCONNECT)
rpc(wire.OP_KEY_CATALOG,'1:cursor:u8?','1:revision:u32 2:next_cursor:u8 3:entries:bytes')
rpc(wire.OP_KEYS_SNAPSHOT)
rpc(wire.OP_EVENTS_ENABLE,'1:enabled:bool')
rpc(wire.OP_GET_VOICE_CAPS,'','1:codecs:bytes 2:max_unit_bytes:u32 3:config_max_bytes:u16')
rpc(wire.OP_VOICE_ENABLE,'1:enabled:bool 2:accepted_codecs:bytes? 3:max_unit_bytes:u32?','1:enabled:bool 2:waiting_idle:bool')
rpc(wire.OP_VOICE_STOP,'1:stream_id:u32')
rpc(wire.OP_VOICE_START,'')
EVENTS={wire.OP_DEVICE_STATE_EV:DEVICE,wire.OP_OPERATION_EV:OPERATION,
    wire.OP_FIND_DONE_EV:fields('1:search_id:u32 2:reason:u8'),
    wire.OP_PAIR_PROMPT_EV:fields('1:operation_id:u32 2:prompt_id:u32 3:method:u8 4:number:u32? 5:remaining_ms:u32'),
    wire.OP_VOICE_STARTED_EV:fields('1:stream_id:u32 2:epoch:u32 3:codec_id:u32 4:codec_revision:u16 5:sample_rate:u32 6:channels:u8 7:codec_config:bytes 8:max_unit_bytes:u32 9:first_sample_index:u64 10:captured_us:u64 11:first_unit_seq:u32'),
    wire.OP_VOICE_ENDED_EV:fields('1:stream_id:u32 2:reason:u8 3:delivered_encoded_bytes:u64 4:ended_us:u64 5:delivered_units:u32 6:delivered_samples:u64 7:final_epoch:u32'),
    wire.OP_VOICE_FORMAT_EV:fields('1:stream_id:u32 2:epoch:u32 3:codec_id:u32 4:codec_revision:u16 5:sample_rate:u32 6:channels:u8 7:codec_config:bytes 8:max_unit_bytes:u32 9:first_sample_index:u64 10:captured_us:u64 11:first_unit_seq:u32')}
ERROR=fields('1:message:text? 2:uncertain:bool? 3:echo_nonce:bytes?')

def parse(payload, definition):
    return wire.parse_tlv(payload,schema={k:v['type'] for k,v in definition.items()},
                          required=[k for k,v in definition.items() if v['required']])

def validate_response(op,status,payload):
    if status>=2:return parse(payload,ERROR)
    expected=1 if op in (wire.OP_PAIR_BEGIN,wire.OP_FORGET_PEER) else 0
    if status!=expected:raise ValueError('incorrect successful response status')
    if op==wire.OP_KEYS_SNAPSHOT:
        if len(payload)!=24:raise ValueError('snapshot length')
        return None
    result=parse(payload,RESPONSES[op])
    if op==wire.OP_GET_DEVICE:validate_device(result)
    if op in (wire.OP_FIND_LIST,wire.OP_KEY_CATALOG):
        blob=result[2 if op==wire.OP_FIND_LIST else 3]
        maximum=8 if op==wire.OP_FIND_LIST else 64
        if not blob or blob[0]>maximum:raise ValueError('entry count')
        pos=1
        for _ in range(blob[0]):
            prefix=7 if op==wire.OP_FIND_LIST else 4
            if len(blob)-pos<prefix:raise ValueError('truncated entry')
            length=blob[pos+prefix-1]
            if length>48 or pos+prefix+length>len(blob):raise ValueError('entry name length')
            blob[pos+prefix:pos+prefix+length].decode('utf-8')
            pos+=prefix+length
        if pos!=len(blob):raise ValueError('entry trailing bytes')
    return result

def validate_device(t):
    if t[3]>7 or t[7]>64 or t[8]>4 or t[11]>2 or t[18]>3 or (t[10]>100 and t[10]!=255) or not 0<=t[9]<=384000:raise ValueError('device values')
    if len(t[4].encode())>40 or len(t[5].encode())>48:raise ValueError('device text')

def document():
    names={v:k.removeprefix('OP_') for k,v in vars(wire).items() if k.startswith('OP_')}
    messages=[]
    for op,req in REQUESTS.items():
        messages.append(dict(opcode=op,name=names[op],request=req,response=RESPONSES[op],
            connection='current' if op>=0x200 else 'zero',success_status=1 if op in (0x110,0x113) else 0,
            response_binary='keys_state' if op==0x201 else None))
    for op,fields_ in EVENTS.items():messages.append(dict(opcode=op,name=names[op],kind=3,fields=fields_))
    messages.extend([dict(opcode=0x280,name='KEYS_STATE',kind=3,binary='keys_state'),dict(opcode=0x381,name='VOICE_DATA',kind=4,binary='voice_data')])
    return dict(protocol='RBP',version=[3,0],byte_order='little',header_format='<2sBBBBHIIIHHHHI',header_bytes=32,
        magic_hex='5242',max_payload=512,max_raw=548,max_cobs_nonzero=551,delimiter_hex='00',crc='CRC32C reflected 0x82F63B78 init/final 0xffffffff',
        tlv=dict(header_format='<BBH',unknown_tags='skip bounded value',duplicate_tags='reject',types={1:'u8',2:'u16',3:'u32',4:'u64',5:'bool',6:'UTF-8',7:'bytes'},max_text=96,max_bytes=480),
        binary=dict(keys_state=dict(format='<IQQBBH',fields=['input_seq','captured_us','pressed_bits','kind','reason','reserved'],bytes=24),
                    voice_data=dict(format='<IIIIIIQII',fields=['stream_id','frame_seq','epoch','unit_seq','unit_size','fragment_offset','first_sample_index','unit_sample_count','reserved'],header_bytes=40,tail='1..472 encoded bytes',max_unit_bytes=65536),
                    candidates=dict(prefix='count:u8',entry='candidate_id:u32,support:u8,signal:u8,name_length:u8,name:UTF-8',max_count=8),
                    keys=dict(prefix='count:u8',entry='slot:u8,key_id:u16,name_length:u8,name:UTF-8',max_count=64)),
        statuses=wire.STATUS,error_fields=ERROR,messages=messages,
        lifecycle=dict(hello='session=connection=0, tx_seq=request_id=1; fresh random 16-byte nonce echoed by response',sequence='strict increment per direction; no zero or wrap',heartbeat_ms=1000,session_timeout_ms=5000,partial_frame_timeout_ms=1000,max_pending=4),
        normative_document='../../docs/wire-protocol.md')

if __name__=='__main__':print(json.dumps(document(),ensure_ascii=False,indent=2))
