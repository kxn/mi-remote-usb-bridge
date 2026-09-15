import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'client/python'))
from rbp.debug import decode
for result in (2,6,14):
    payload=struct.pack('<III',1,0,2)+struct.pack('<IIBBHIIII',1,123,0,1,5,result,1,64,17)
    r=decode(payload)['records'][0]
    assert r['name']=='sdk_rx_check_failed'
    assert (r['rx_check_result'],r['encrypted_check'],r['precheck_state'],r['rx_check_sequence'])==(result,1,64,17)
payload=struct.pack('<III',1,0,2)+struct.pack('<IIBBHIIII',2,124,0,1,4,134,5,17,3)
r=decode(payload)['records'][0]
assert (r['sdk_code'],r['sdk_status'],r['rx_checks'],r['rx_crc_failures'])==(134,5,17,3)
print('SDK probe structured diagnostics passed')

def record(code,*args):
    return decode(struct.pack('<III',1,0,4)+struct.pack('<IIBBHIIII',1,0,0,1,code,*args))['records'][0]
r=record(6,17,0x40,0x123,0)
assert (r['rx_check_sequence'],r['aes_control_after'],r['bb_status_after'],r['ip_state_after'])==(17,0x40,0x123,0)
r=record(7,17,0x810e,0x81,0x1234)
assert (r['ll_length'],r['llid'],r['nesn'],r['sn'],r['checked_tail_status'])==(129,2,1,1,129)
r=record(7,17,0xffffffff,0xffffffff,0x1234)
assert 'll_header' not in r and r['checked_tail_status'] is None
r=record(8,17,42,0x180,9)
assert (r['arm_sequence'],r['arm_ip_state'],r['arm_direction'],r['crc_failures_at_poll'])==(42,128,1,9)
r=record(9,2,99,8,60)
assert (r['snapshot_dropped'],r['rx_checks'],r['rx_crc_failures'],r['arm_sequence'])==(2,99,8,60)
print('SDK hardware/context snapshot diagnostics passed')
