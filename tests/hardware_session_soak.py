"""Explicit-port, read-only hardware soak. Records raw frames; never pairs/flashes."""
import argparse
import json
from pathlib import Path
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'client/python'))
from rbp.client import BridgeClient
from rbp.transport import SerialTransport

p=argparse.ArgumentParser();p.add_argument('--port',required=True)
p.add_argument('--seconds',type=float,default=300);p.add_argument('--log',default='build/session-soak.jsonl')
a=p.parse_args();start=time.monotonic()
with open(a.log,'w',encoding='utf-8',buffering=1) as f:
    def log(kind,**fields):
        f.write(json.dumps(dict(t=round(time.monotonic()-start,6),kind=kind,**fields))+'\n')
    class TraceTransport(SerialTransport):
        def read(self,timeout):
            data=super().read(timeout)
            if data:log('rx',hex=data.hex())
            return data
        def write(self,data):
            log('tx',hex=data.hex());super().write(data)
    c=BridgeClient(TraceTransport(a.port));c.on_session_lost=lambda:log('lost',reason=c.last_session_error)
    failures=0;attempts=0;next_report=0
    try:
        while time.monotonic()-start<a.seconds:
            if not c.session_id:
                attempts+=1
                try:c.hello();c.get_device();c.get_peer();log('connected',session=c.session_id)
                except Exception as e:failures+=1;log('connect_error',error=str(e));time.sleep(.5);continue
            c.drain(.05)
            if not c.session_id:failures+=1
            if time.monotonic()-start>=next_report:
                print(f'{time.monotonic()-start:.1f}s session={c.session_id} attempts={attempts} failures={failures}',flush=True)
                next_report+=30
        log('summary',attempts=attempts,failures=failures)
        print(f'FINISHED attempts={attempts} failures={failures}',flush=True)
    finally:
        try:c.goodbye()
        except Exception as e:log('close_error',error=str(e))
        c.t.close()
