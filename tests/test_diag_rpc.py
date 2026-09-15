"""A device RPC failure must not tear down a healthy CLI USB session."""
import queue
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'demo')]
import rbp_diag as diag
from rbp.client import DeviceInfo,RpcError

events=[]
class Trace:
    frames=0;bad_frames=0
    def __init__(self,*a,**kw):pass
    def emit(self,name,*a,**kw):events.append((name,kw))
    def close(self):pass
class Worker:
    opened=0;closed=0;catalog_calls=0
    def __init__(self,*a):Worker.opened+=1;self.sent=False
    def call(self,name,*a):
        if name=='hello':return {3:'test'}
        if name=='get_device':return DeviceInfo(connection_id=1,peer_id=7,state=5)
        if name=='get_peer':return {'peer_id':7}
        if name=='key_catalog':Worker.catalog_calls+=1;raise RpcError(9)
        raise AssertionError(name)
    def next_event(self,timeout):
        if not self.sent:self.sent=True;return ('operation',({'result':10,'operation_id':1},))
        time.sleep(.005);raise queue.Empty
    def close(self):Worker.closed+=1
args=SimpleNamespace(compact_log=False,log='build/logs/diag-rpc-test.jsonl',seconds=.12,port='MOCK',tcp=None,pair=False,record_dir=None)
with patch.object(diag,'Trace',Trace),patch.object(diag,'BridgeWorker',Worker),patch.object(diag,'SerialTransport',lambda *a:None):
    diag.run(args)
assert Worker.opened==Worker.closed==1 and Worker.catalog_calls==1
assert any(n=='operation' and v['values'][0]['result']==10 for n,v in events)
assert any(n=='device_request_failed' for n,v in events)
assert not any(n=='connection_failed' for n,v in events)
print('CLI RPC failure: one session, original operation result retained, bounded retry passed')
