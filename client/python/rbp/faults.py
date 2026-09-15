"""Release diagnostic metadata from GET_STATS; independent of debug traces."""
import struct
DOMAINS={1:'sdk',2:'gap',3:'gatt_api',4:'gatt_event',5:'att',6:'atvv',7:'storage',8:'adapter'}
GAP_STAGES={1:'set_parameter',2:'bond_get_parameter',3:'bond_set_parameter',4:'cancel_discovery',
            5:'establish_link',6:'start_device',7:'start_discovery',8:'role_get_parameter',
            9:'terminate_link',10:'configure_address',11:'passcode_response',12:'snv_read',
            64:'role_event',65:'link_termination'}
def decode_faults(stats):
    data=stats.get(7,b'')
    if len(data)>96 or len(data)%24:raise ValueError('invalid release fault records')
    result=[]
    for seq,domain,stage,code,context,count,board_ms in struct.iter_unpack('<IHHIIII',data):
        if not seq or not count:raise ValueError('invalid release fault counters')
        result.append(dict(sequence=seq,source=DOMAINS.get(domain,f'domain_{domain}'),stage=stage,
                           stage_name=GAP_STAGES.get(stage,'') if domain==2 else '',
                           code=code,code_hex=f'0x{code:08X}',context=context,count=count,board_ms=board_ms))
    return result
