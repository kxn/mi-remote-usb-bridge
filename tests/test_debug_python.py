import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'client/python'))
from rbp.debug import decode,validate_response
from rbp.wire import TlvWriter
record=struct.pack('<IIBBHIIII',7,123,5,4,10,3<<16,0x00636261,0,0)
decoded=decode(struct.pack('<III',1,2,16)+record)
assert decoded['dropped']==2 and decoded['records'][0]['bytes']=='616263'
for bad in (b'',b'\0'*13,struct.pack('<III',2,0,0)):
    try:decode(bad)
    except ValueError:pass
    else:raise AssertionError('bad debug payload accepted')
assert validate_response(TlvWriter().u32(1,1).u32(2,2).u32(3,3).data)[2]==2
print('debug Python: record layout, raw chunks, version/length, response schema passed')
record=struct.pack('<IIBBHIIII',8,124,3,3,8,2,6,0,5000)
event=decode(struct.pack('<III',1,0,16)+record)['records'][0]
assert event['previous_state']=='initiating' and event['next_state']=='initiation_canceling'
assert event['pending_intent']=='none' and event['timeout_ms']==5000
record=struct.pack('<IIBBHIIII',9,125,3,3,11,4,18,65535,0)
event=decode(struct.pack('<III',1,0,16)+record)['records'][0]
assert event['api']=='cancel_initiation' and event['status']==18
def event_for(code,*args):
    return decode(struct.pack('<III',1,0,16)+struct.pack('<IIBBHIIII',10,126,3,3,code,*args))['records'][0]
event=event_for(15,0xC239B240,0x0000C05D,1,200)
assert event['address']=='C0:5D:C2:39:B2:40' and event['target_matches_local_identity'] is True
assert event['rssi']==-56
assert event_for(15,0,1<<16,255,0)['target_matches_local_identity'] is None
assert event_for(14,0,0,1,1)['live_matches_nv'] is True
event=event_for(16,1,24,0,300)
assert event['interval_ms']==30 and event['supervision_timeout_ms']==3000
event=event_for(17,1,6,0,300)
assert event['name']=='connection_parameters_updated' and event['interval_ms']==7.5
record=struct.pack('<IIBBHIIII',11,127,0,2,3,16,8,0,0)
event=decode(struct.pack('<III',1,0,16)+record)['records'][0]
assert event['receive_processing_ms']==10 and event['output_flush_ms']==5
