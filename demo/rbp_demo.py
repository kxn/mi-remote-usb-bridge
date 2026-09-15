#!/usr/bin/env python3
"""RBP CLI: dedicated IO worker; observe by default, explicit configurable actions."""
import argparse
import queue
import sys
import time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"client/python"))
from rbp.worker import BridgeWorker
from rbp.transport import SerialTransport,TcpTransport
from audio import WaveRecorder
from input_actions import InputMapper,WindowsKeys,MacKeys


def transport(args):
    if args.tcp:return TcpTransport(args.tcp[0],int(args.tcp[1]))
    if args.port:return SerialTransport(args.port)
    raise ValueError("specify --port or --tcp HOST PORT")


def ready(worker, timeout=25):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        device=worker.call("get_device")
        if device.state==5:return device
        time.sleep(.1)  # IO and heartbeat continue on the worker
    raise TimeoutError("device did not become ready")


def pairing_prompt(worker,prompt):
    method=prompt["method"]
    deadline=time.monotonic()+prompt["remaining_ms"]/1000
    print(f"Pairing method={method}, number={prompt.get('number','-')}")
    if method==3:
        print(f"Enter {prompt['number']:06d} on the remote. Ctrl+C cancels pairing.")
        return
    reply=input("Passkey, or yes/no to confirm (empty rejects): ").strip()
    if time.monotonic()>=deadline: print("pair prompt expired");return
    accept=reply.lower() in ("yes","y") or reply.isdecimal()
    passkey=int(reply) if method==1 and reply.isdecimal() else None
    worker.call("pair_reply",prompt["operation_id"],prompt["prompt_id"],accept,passkey)


def pair(worker,args):
    search=worker.call("find_start",args.scan_ms)
    time.sleep(args.scan_ms/1000+.1)
    entries=[];cursor=0
    while True:
        cursor,batch=worker.call("find_list",search,cursor);entries.extend(batch)
        if cursor==255:break
    if not entries:raise RuntimeError("no candidates")
    for i,c in enumerate(entries):print(i,c.name,c.signal)
    choice=input("Candidate index (empty cancels): ").strip()
    if not choice:return 0
    if not choice.isdecimal() or int(choice)>=len(entries):raise ValueError("invalid candidate index")
    selected=int(choice)
    operation=worker.call("pair_begin",search,entries[selected].candidate_id)
    deadline=time.monotonic()+65
    while time.monotonic()<deadline:
        try:name,args_=worker.next_event(.1)
        except queue.Empty:continue
        if name=="pair_prompt":pairing_prompt(worker,args_[0])
        elif name=="operation" and args_[0]["operation_id"]==operation:
            print(args_[0]);return 0 if args_[0]["result"]==0 else 1
        elif name=="error":raise RuntimeError(args_[0])
    raise TimeoutError("pairing operation")


def monitor(worker,args):
    mapper=None
    if args.inject:
        if not args.mapping:raise ValueError("--inject requires --mapping FILE")
        backend=WindowsKeys() if sys.platform=="win32" else MacKeys()
        mapper=InputMapper(backend,InputMapper.load(args.mapping))
    catalog=[];device=None
    def attach(d):
        nonlocal device,catalog
        if mapper:mapper.release()
        device=d
        if d.state==5:
            catalog=worker.call("key_catalog");worker.call("keys_snapshot");worker.call("events_enable",True)
    attach(ready(worker))
    try:
        while True:
            try:name,values=worker.next_event(1)
            except queue.Empty:continue
            if name=="keys":
                state=values[0]
                print(" + ".join(k.name for k in catalog if state.pressed_bits&(1<<k.slot)) or "released")
                if mapper:
                    if state.kind==2:mapper.release()
                    else:mapper.update(device.model_id,catalog,state.pressed_bits)
            elif name=="device":
                d=values[0]
                if device is None or (device.connection_id,device.catalog_revision)!=(d.connection_id,d.catalog_revision):attach(d)
            elif name=="session_lost":
                if mapper:mapper.release()
                raise ConnectionError("session lost; reopen the port")
            elif name=="error":raise RuntimeError(values[0])
    except KeyboardInterrupt:return 0
    finally:
        if mapper:mapper.release()


def voice(worker,args):
    ready(worker);rec=WaveRecorder(args.output);worker.call("voice_enable",True)
    deadline=time.monotonic()+args.timeout
    print("Listening: press the remote voice button. Ctrl-C stops recording.")
    try:
        while time.monotonic()<deadline:
            try:name,values=worker.next_event(.1)
            except queue.Empty:continue
            if name=="voice_start":rec.start(*values)
            elif name=="voice_data":rec.data(*values)
            elif name=="voice_format":rec.format(*values)
            elif name=="voice_end":
                rec.end(*values);reason=values[0].reason
                print(f"Saved {args.output}; reason={reason}");return 0 if reason in (0,7,8) else 1
            elif name in ("voice_abort","session_lost","error"):
                rec.abort(name);raise RuntimeError(f"recording aborted: {values}")
        rec.abort("wait_timeout");return 1
    except KeyboardInterrupt:
        rec.abort("user_cancelled");return 1
    finally:
        try:worker.call("voice_enable",False)
        except Exception:pass
        rec.abort("closed")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port");parser.add_argument("--tcp",nargs=2,metavar=("HOST","PORT"))
    sub=parser.add_subparsers(dest="command",required=True)
    sub.add_parser("device")
    p=sub.add_parser("pair");p.add_argument("--scan-ms",type=int,default=3000)
    sub.add_parser("forget")
    sub.add_parser("connect");sub.add_parser("disconnect")
    p=sub.add_parser("reconnect");p.add_argument("enabled",choices=("on","off"))
    p=sub.add_parser("monitor");p.add_argument("--inject",action="store_true");p.add_argument("--mapping")
    p=sub.add_parser("voice");p.add_argument("output");p.add_argument("--timeout",type=float,default=120)
    args=parser.parse_args();worker=None
    try:
        worker=BridgeWorker(transport(args));print(worker.call("hello"))
        if args.command=="pair":return pair(worker,args)
        if args.command=="monitor":return monitor(worker,args)
        if args.command=="voice":return voice(worker,args)
        if args.command=="device":print(worker.call("get_device"));print(worker.call("get_peer"))
        elif args.command=="forget":
            peer=worker.call("get_peer");op=worker.call("forget_peer",peer["peer_id"])
            result=worker.call("wait_operation",op,timeout=10);print(result)
            return 0 if result["result"]==0 else 1
        elif args.command=="reconnect":
            peer=worker.call("get_peer");worker.call("set_reconnect",peer["peer_id"],args.enabled=="on")
        elif args.command=="connect":worker.call("connect_peer")
        elif args.command=="disconnect":worker.call("disconnect")
        return 0
    except (Exception,KeyboardInterrupt) as exc:
        print(f"Error: {exc}",file=sys.stderr);return 1
    finally:
        if worker:worker.close()


if __name__=="__main__":raise SystemExit(main())
