#!/usr/bin/env python3
"""Unattended serial diagnostics with explicit pairing and no OS input injection."""
import argparse
import dataclasses
from datetime import datetime
import json
import logging
import math
from logging.handlers import RotatingFileHandler
from pathlib import Path
import queue
import threading
import sys
import time
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'client/python'),str(Path(__file__).resolve().parent)]
from rbp.transport import SerialTransport,TcpTransport
from rbp.worker import BridgeWorker
from rbp.client import RpcError
from rbp.audio_wire import AudioError
from rbp.faults import decode_faults
from rbp.wire import FrameParser
from audio import WaveRecorder
from rbp_demo import pairing_prompt
from rbp.debug import EVENT as DEBUG_EVENT,decode as decode_debug


def serializable(value):
    if dataclasses.is_dataclass(value):return dataclasses.asdict(value)
    if isinstance(value,bytes):return {'hex':value.hex()}
    return str(value)


class Trace:
    def __init__(self,path,compact=False):
        self.compact=compact
        self.start=time.monotonic();self.frames=0;self.bad_frames=0
        self.logger=logging.Logger('rbp-diagnostic')
        handler=RotatingFileHandler(path,maxBytes=8*1024*1024,backupCount=5,encoding='utf-8')
        self.logger.addHandler(handler)
        self.pending=queue.Queue(4096);self.dropped=0
        self.thread=threading.Thread(target=self._writer,name='rbp-log',daemon=True)
        self.thread.start()
    def _writer(self):
        while True:
            item=self.pending.get()
            if item is None:return
            record,console,fields=item
            self.logger.info(json.dumps(record,default=serializable,ensure_ascii=False))
            if console:print(f"{record['time']} {record['event']} {json.dumps(fields,default=serializable,ensure_ascii=False)}",flush=True)
    def emit(self,event,console=False,**fields):
        if self.compact and event=="voice_data" and "values" in fields:
            chunk=fields.pop("values")[0]
            fields.update(stream_id=chunk.stream_id, frame_seq=chunk.frame_seq,
                          first_sample_index=chunk.first_sample_index, encoded_bytes=len(chunk.data), unit_seq=chunk.unit_seq, epoch=chunk.epoch,
                          unit_sample_count=chunk.unit_sample_count, fragment_offset=chunk.fragment_offset)
        record=dict(time=datetime.now().astimezone().isoformat(),elapsed=round(time.monotonic()-self.start,6),event=event,**fields)
        record['logger_dropped_total']=self.dropped
        try:self.pending.put_nowait((record,console,fields))
        except queue.Full:self.dropped+=1
    def close(self):
        self.pending.put(None);self.thread.join()
        if self.dropped:self.logger.warning(json.dumps(dict(event='logger_overrun',dropped=self.dropped)))
        for h in self.logger.handlers:h.close()


class TracedTransport:
    def __init__(self,transport,trace):
        self.transport=transport;self.trace=trace;self.parsers={d:FrameParser() for d in ('tx','rx')}
        self.debug_dropped=0
    def record(self,direction,data):
        now=int(time.monotonic()*1000)
        if data and not self.trace.compact:self.trace.emit(direction,hex=data.hex())
        for ev in self.parsers[direction].feed(data,now):
            if ev[0]=='frame':
                self.trace.frames+=1
                self.trace.emit('frame',direction=direction,header=ev[1],crc_ok=ev[3],
                                **({'payload_size':len(ev[2])} if self.trace.compact else {'payload':ev[2]}))
                if direction=='rx' and ev[3] and ev[1].opcode==DEBUG_EVENT:
                    try:
                        decoded=decode_debug(ev[2]);self.trace.emit('firmware_debug',**decoded)
                        if decoded['dropped']>self.debug_dropped:
                            self.trace.emit('firmware_log_overflow',True,dropped=decoded['dropped'])
                        self.debug_dropped=decoded['dropped']
                        for record in decoded['records']:
                            if record['level']==1:self.trace.emit('firmware_error',True,**record)
                    except ValueError as exc:self.trace.emit('debug_decode_error',True,error=str(exc))
                if not ev[3]:self.trace.bad_frames+=1
            else:
                self.trace.bad_frames+=1;self.trace.emit('parser_error',True,direction=direction,reason=ev[0])
    def read(self,timeout):
        data=self.transport.read(timeout);self.record('rx',data);return data
    def write(self,data):
        self.record('tx',data)
        started=time.monotonic()
        try:self.transport.write(data)
        except Exception as exc:
            self.trace.emit('write_failed',True,bytes=len(data),duration_ms=(time.monotonic()-started)*1000,error=repr(exc))
            raise
        self.trace.emit('write_completed',bytes=len(data),duration_ms=(time.monotonic()-started)*1000)
    def close(self):self.transport.close()


def run(args):
    path=Path(args.log);path.parent.mkdir(parents=True,exist_ok=True);trace=Trace(path,compact=args.compact_log)
    deadline=time.monotonic()+args.seconds if args.seconds else float('inf')
    backoff=1;attempt=0;pair_attempted=False;connect_attempted=False
    start_attempted=False
    try:
        while time.monotonic()<deadline:
            worker=None;recorder=None;scheduled_stop=None
            try:
                attempt+=1;trace.emit('opening',True,port=args.port,attempt=attempt)
                transport=TcpTransport(args.tcp[0],int(args.tcp[1])) if args.tcp else SerialTransport(args.port)
                worker=BridgeWorker(TracedTransport(transport,trace))
                def call(method,*values):
                    trace.emit('request',method=method,arguments=values)
                    try:result=worker.call(method,*values)
                    except Exception as e:
                        trace.emit('request_error',True,method=method,error=str(e));raise
                    trace.emit('result',method=method,result=result);return result
                info=call('hello');device=call('get_device');peer=call('get_peer')
                if 'debug' in info[3]:
                    call('debug_config',args.debug_modules,args.debug_level)
                trace.emit('connected',True,hello=info,device=device,peer=peer)
                last_fault_sequence=None
                connected_at=time.monotonic();catalog=[];configured=None;search=None
                next_scan=0;next_status=0;ready_retry_at=0;audio_failed_connection=None
                while time.monotonic()<deadline:
                    try:
                        now=time.monotonic()
                        if now-connected_at>=10:backoff=1
                        if scheduled_stop and now>=scheduled_stop[1]:
                            stream_id=scheduled_stop[0];scheduled_stop=None
                            # Consume before RPC: an ambiguous timeout must not replay STOP.
                            trace.emit('timed_voice_stop',True,stream_id=stream_id)
                            call('voice_stop',stream_id)
                        if now>=next_status:
                            device=call('get_device');peer=call('get_peer')
                            stats=call('get_stats')
                            if stats.get(6,0)!=last_fault_sequence:
                                last_fault_sequence=stats.get(6,0)
                                trace.emit('release_faults',True,sequence=last_fault_sequence,evicted_records=stats.get(8,0),records=decode_faults(stats))
                            trace.emit('status',True,device=device,peer=peer,frames=trace.frames,bad_frames=trace.bad_frames)
                            next_status=now+10
                            if not args.no_connect and peer['peer_id'] and device.state==1 and not connect_attempted:
                                connect_attempted=True;call('connect_peer',peer['peer_id'])
                        if device.state==5 and device.peer_id and now>=ready_retry_at:
                            identity=(device.connection_id,device.catalog_revision)
                            if configured!=identity:
                                catalog=call('key_catalog');call('keys_snapshot');call('events_enable',True)
                                trace.emit('ready',True,keys=catalog);configured=identity
                            if args.record_dir and device.voice_state==2 and not device.voice_enabled and audio_failed_connection!=device.connection_id:
                                call('voice_enable',True);device=call('get_device')
                            if args.start_voice and not start_attempted and device.voice_enabled:
                                start_attempted=True;call('voice_start')
                        else:configured=None
                        if (args.pair or args.scan) and not peer['peer_id'] and not pair_attempted and search is None and now>=next_scan:
                            search=call('find_start',3000);trace.emit('scanning',True,search_id=search)
                        try:name,values=worker.next_event(.1)
                        except queue.Empty:continue
                        trace.emit(name,console=name not in ('voice_data',),values=values)
                        if name in ('session_lost','error'):raise ConnectionError(str(values))
                        if name=='voice_start':
                            delay=getattr(args,'stop_voice_after',0)
                            scheduled_stop=(values[0].stream_id,time.monotonic()+delay) if delay else None
                        elif name in ('voice_end','voice_abort'):
                            scheduled_stop=None
                        if name=='device':device=values[0]
                        elif name=='keys':
                            trace.emit('buttons',True,pressed=[k.name for k in catalog if values[0].pressed_bits&(1<<k.slot)])
                        elif name=='find_done' and values[0]['search_id']==search:
                            entries=[];cursor=0
                            while cursor!=255:
                                cursor,batch=call('find_list',search,cursor);entries.extend(batch)
                            trace.emit('candidates',True,entries=entries)
                            if entries:
                                trace.emit('pair_selection_required',True,search_id=search,message='Select explicitly; discovery never pairs automatically.')
                                if args.pair:
                                    pair_attempted=True  # no replay after EOF, cancellation or ambiguous RPC
                                    for index,candidate in enumerate(entries):
                                        print(f"{index}: {candidate.name} · signal {candidate.signal}",flush=True)
                                    try: choice=input('Candidate index (empty cancels): ').strip()
                                    except EOFError: choice=''
                                    if choice.isdecimal() and int(choice)<len(entries):
                                        call('pair_begin',search,entries[int(choice)].candidate_id)
                                    else:trace.emit('pair_cancelled',True,message='No valid explicit selection; nothing paired.')
                            search=None;next_scan=now+2
                        elif name=='pair_prompt':
                            if sys.stdin.isatty():pairing_prompt(worker,values[0])
                            else:trace.emit('pair_input_required',True,prompt=values[0],message='Run in an interactive terminal to reply; no automatic approval.')
                        elif name=='operation':next_status=0
                        elif name=='voice_start' and args.record_dir:
                            if recorder:recorder.abort('new_stream')
                            directory=Path(args.record_dir)
                            recorder=WaveRecorder(directory/datetime.now().strftime('voice-%Y%m%d-%H%M%S-%f.wav'))
                            recorder.start(*values)
                        elif name=='voice_data' and recorder:recorder.data(*values)
                        elif name=='voice_format' and recorder:recorder.format(*values)
                        elif name=='voice_end' and recorder:
                            recorder.end(*values);trace.emit('wav_saved',True,files=recorder.completed,complete=values[0].reason in (0,7,8),reason=values[0].reason);recorder=None
                        elif name=='voice_abort':
                            audio_failed_connection=device.connection_id
                            if recorder:recorder.abort(str(values));recorder=None
                    except AudioError as exc:
                        scheduled_stop=None
                        cleanup_errors=[]
                        if recorder:
                            recorder.abort(str(exc));cleanup_errors=list(recorder.cleanup_errors);recorder=None
                        audio_failed_connection=device.connection_id
                        trace.emit('audio_consumer_failed',True,error=str(exc),cleanup_errors=cleanup_errors)
                        call('voice_enable',False)
                    except RpcError as exc:
                        # Device RPC failure is not loss of the USB session.
                        # Preserve queued operation results and refresh state.
                        trace.emit('device_request_failed',True,error=str(exc))
                        configured=None;next_status=0;ready_retry_at=time.monotonic()+1
                        next_scan=ready_retry_at

            except Exception as exc:
                trace.emit('connection_failed',True,error=str(exc),retry_seconds=backoff)
            finally:
                if recorder:recorder.abort('session_closed')
                if worker:
                    try:worker.close()
                    except Exception as exc:trace.emit('close_error',True,error=str(exc))
            if time.monotonic()<deadline:
                time.sleep(min(backoff,max(0,deadline-time.monotonic())));backoff=min(8,backoff*2)
    except KeyboardInterrupt:trace.emit('stopped',True)
    finally:
        trace.emit('summary',True,attempts=attempt,frames=trace.frames,bad_frames=trace.bad_frames)
        trace.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    group=p.add_mutually_exclusive_group(required=True)
    group.add_argument('--port');group.add_argument('--tcp',nargs=2,metavar=('HOST','PORT'))
    selection=p.add_mutually_exclusive_group()
    selection.add_argument('--pair',action='store_true',help='Scan, list candidates, then require a typed index before pairing (even one candidate).')
    selection.add_argument('--scan',action='store_true',help='Show discovered candidates only; never initiate pairing.')
    p.add_argument('--start-voice',action='store_true',help='Request one capture after READY and voice enable; never retry an ambiguous request.')
    p.add_argument('--no-connect',action='store_true',help='Never send CONNECT_PEER; observe firmware autonomous reconnection.')
    p.add_argument('--record-dir',help='Save compressed stream, metadata and decoded WAV when voice becomes available.')
    p.add_argument('--stop-voice-after',type=float,default=0,help='Diagnostic: send one VOICE_STOP per stream after N host seconds; 0 disables. Does not trim audio.')
    p.add_argument('--seconds',type=float,default=0,help='0: run until Ctrl+C')
    p.add_argument('--log',default='build/logs/diagnostic.jsonl')
    p.add_argument('--compact-log',action='store_true',help='Keep frame headers, encoded counters and firmware diagnostics; omit duplicate hex dumps. Encoded capture and WAV are retained.')
    p.add_argument('--debug-level',type=int,choices=range(5),default=3,help='Debug firmware: 0 off, 1 errors, 2 state, 3 detail, 4 raw attributes.')
    p.add_argument('--debug-modules',type=lambda s:int(s,0),default=255,help='Debug firmware module bitmask, default 0xff.')
    args=p.parse_args()
    if not math.isfinite(args.seconds) or args.seconds<0:p.error('--seconds must be finite and nonnegative')
    if not math.isfinite(args.stop_voice_after) or args.stop_voice_after<0:p.error('--stop-voice-after must be finite and nonnegative')
    if args.start_voice and not args.record_dir:p.error('voice start requires --record-dir')
    if not 0<=args.debug_modules<=255:p.error('--debug-modules must be 0..255')
    run(args)


if __name__=='__main__':main()
