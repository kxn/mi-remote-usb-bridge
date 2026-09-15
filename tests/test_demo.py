"""No real OS input. WAV, mapping ABI, strict client, worker and Qt regression."""
import ctypes
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import wave
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'client/python'),str(ROOT/'demo')]
from rbp import wire
from rbp.client import BridgeClient,KeyDef,AudioFormat,EncodedFragment,AudioEnd
from rbp.transport import TcpTransport
from rbp.worker import BridgeWorker
from audio import WaveRecorder
from input_actions import InputMapper,WindowsKeys

class Offline(unittest.TestCase):
    def test_release_fault_metadata(self):
        from rbp.faults import decode_faults
        fault=decode_faults({6:9,7:struct.pack('<IHHIIII',9,1,0x86,5,0,23,1200)})[0]
        self.assertEqual((fault['source'],fault['stage'],fault['code'],fault['count']),('sdk',0x86,5,23))
        self.assertEqual(decode_faults({}),[])
        with self.assertRaises(ValueError):decode_faults({7:b'x'})

    def test_gui_discovery_requires_explicit_pair(self):
        from types import SimpleNamespace
        os.environ['QT_QPA_PLATFORM']='offscreen'
        from PySide6 import QtWidgets
        from rbp_gui import Window
        app=QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
        window=Window();window.timer.stop();calls=[]
        window.submit=lambda method,*args,**kwargs:calls.append((method,args))
        window.search_id=12;generation=window.scan_generation
        candidate=SimpleNamespace(name='Remote',signal=3,candidate_id=7)
        window.show_candidates((255,[candidate]),12,generation)
        self.assertEqual(calls,[]);self.assertTrue(window.pair_button.isEnabled())
        window.pair();window.pair()
        self.assertEqual(calls,[('pair_begin',(12,7))])
        window.clear_discovery('New scan')
        window.show_candidates((255,[candidate]),12,generation)
        self.assertEqual(window.candidates.count(),0);self.assertFalse(window.pair_button.isEnabled())
        self.assertTrue(callable(window.prompt))
        window.close()

    def test_delayed_heartbeat_and_failed_hello(self):
        class Transport:
            def write(self,data):pass
        c=BridgeClient(Transport());c.session_id=9
        c._last_ping_ok=time.monotonic()-2;c._heartbeat()
        rid,ev=next(iter(c._pending.items()))
        ev['deadline']=time.monotonic()-1
        c._last_ping_sent=time.monotonic()-1.5;c._heartbeat()
        self.assertIs(c._pending[rid],ev)
        payload=wire.TlvWriter().u32(1,ev['cookie']).u64(2,123).data
        h=wire.Header(kind=wire.KIND_RESPONSE,session_id=9,tx_seq=1,request_id=rid,
                      opcode=wire.OP_PING,payload_size=len(payload))
        c._handle_frame(h,payload,True)
        self.assertEqual(c.session_id,9);self.assertFalse(c._pending)
        self.assertLess(time.monotonic()-c._last_ping_ok,.2)
        class BrokenTransport:
            def write(self,data):raise OSError('unplugged during HELLO')
        c=BridgeClient(BrokenTransport())
        with self.assertRaises(OSError):c.hello()
        self.assertIsNone(c._hello_nonce);self.assertFalse(c._pending)
        self.assertEqual(c.session_id,0)

    def test_gui_hello_failure_stops_initialization(self):
        from concurrent.futures import Future
        from unittest.mock import patch
        os.environ['QT_QPA_PLATFORM']='offscreen'
        from PySide6 import QtWidgets
        from rbp_gui import Window
        app=QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
        class Worker:
            def __init__(self,*args):self.calls=[];self.closed=False
            def submit(self,method,*args):
                self.calls.append(method);f=Future();f.set_exception(TimeoutError('HELLO timed out'));return f
            def close(self):self.closed=True
        with patch('rbp_gui.SerialTransport'),patch('rbp_gui.BridgeWorker',Worker):
            window=Window();window.port.setEditText('COM_TEST');window.open_bridge()
            worker=window.worker;window.poll()
            self.assertEqual(worker.calls,['hello']);self.assertTrue(worker.closed)
            self.assertIsNone(window.worker);self.assertIn('Connection failed',window.status.text())
            window.close()

    def test_session_loss_diagnostics(self):
        c=BridgeClient(None);c.session_id=7
        w=BridgeWorker.__new__(BridgeWorker);w.client=c;w.events=queue.Queue(4)
        c.on_session_lost=w._callback("session_lost")
        c._handle_frame(wire.Header(),b"",False)
        self.assertEqual(w.next_event(0),("session_lost",("frame CRC mismatch",)))
        c._session_broken()
        self.assertEqual(c.last_session_error,"frame CRC mismatch")
        self.assertTrue(w.events.empty())
        c.session_id=8;c._last_ping_ok=time.monotonic()-6
        c._heartbeat()
        self.assertIn("heartbeat timeout",w.next_event(0)[1][0])

    def test_wav_segments(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            path=Path(d)/'voice.wav';r=WaveRecorder(path);r.start(AudioFormat(1,1,1,1,8000,1,b'\0'*4,512,0,0,1))
            r.data(EncodedFragment(1,1,1,1,5,0,0,10,b'\x17'*5));r.format(AudioFormat(1,2,1,1,16000,1,b'\0'*4,512,10,0,2))
            r.data(EncodedFragment(1,2,2,2,10,0,10,20,b'\x17'*10));r.end(AudioEnd(1,0,15,0,2,30,2))
            m=json.loads(path.with_suffix('.json').read_text());self.assertTrue(m['complete'])
            for entry,count,rate in zip(m['segments'],(10,20),(8000,16000)):
                with wave.open(entry['path']) as f:self.assertEqual((f.getnframes(),f.getframerate()),(count,rate))
            r.start(AudioFormat(2,1,1,1,8000,1,b'\0'*4,512,0,0,1));r.abort('session_lost');self.assertFalse(json.loads(path.with_suffix('.json').read_text())['complete'])

    def test_input_diff(self):
        class Backend:
            def __init__(self):self.events=[]
            def key(self,code,down):self.events.append((code,down))
        b=Backend();m=InputMapper(b,{'test':{'9':{'windows':[17,65]},'8':{'windows':[17,66]}}},'windows')
        cat=[KeyDef(2,9,'Home'),KeyDef(5,8,'Back')]
        m.update('test',cat,4);m.update('test',cat,4);self.assertEqual(b.events,[(17,True),(65,True)])
        m.update('test',cat,32);m.release();self.assertEqual(b.events[-4:],[(65,False),(66,True),(66,False),(17,False)])

    @unittest.skipUnless(sys.platform=='win32','Windows ABI')
    def test_windows_abi_without_injection(self):
        b=WindowsKeys();self.assertEqual(ctypes.sizeof(b.Input),40 if ctypes.sizeof(ctypes.c_void_p)==8 else 28)
        events=[]
        def fake(n,p,size):
            v=ctypes.cast(p,ctypes.POINTER(b.Input)).contents;events.append(v.value.keyboard.flags);return 1
        b.send=fake;b.key(0x24,True);b.key(0x24,False);self.assertEqual(events,[1,3])

    def test_strict_session_audio(self):
        class T:
            def write(self,d):pass
        for case in ('sequence','audio_without_start','truncated','wrong_opcode'):
            c=BridgeClient(T());c.session_id=42;c.connection_id=9;c._rx_seq=1;c.pressed_bits=7
            c.catalog=[KeyDef(0,9,'Home')];lost=[];c.on_session_lost=lambda:lost.append(True)
            payload=b'';h=wire.Header(kind=wire.KIND_AUDIO,session_id=42,connection_id=9,tx_seq=2,opcode=wire.OP_VOICE_DATA)
            if case=='sequence':h.tx_seq=4
            elif case=='truncated':
                payload=b'\x00'
            elif case=='wrong_opcode':
                h.kind=wire.KIND_RESPONSE;h.request_id=2;h.opcode=wire.OP_GET_PEER;c._pending[2]=dict(opcode=wire.OP_PING,connection=9)
            h.payload_size=len(payload);c._handle_frame(h,payload,True)
            if case in ('audio_without_start','truncated'):
                self.assertEqual(c.session_id,42);self.assertTrue(c._audio_ignored);self.assertFalse(lost)
            else:
                self.assertEqual((c.session_id,c.pressed_bits,c.catalog),(0,0,[]),case);self.assertTrue(lost)

    def test_schema_export(self):
        from rbp import schema
        self.assertEqual(json.loads((ROOT/'protocol/schema/rbp-3.0.json').read_text(encoding='utf-8-sig')),json.loads(json.dumps(schema.document())))

class DesktopIntegration(unittest.TestCase):
    def test_binding_reboot_and_ids(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            def launch():
                p=subprocess.Popen([str(ROOT/'build/host/sim_bridge.exe'),'--data-port','45841','--control-port','45842','--quiet'],cwd=d,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                time.sleep(.3);return p
            def connect():
                w=BridgeWorker(TcpTransport('127.0.0.1',45841));w.call('hello');return w
            def pair(w):
                search=w.call('find_start',50);time.sleep(.1);_,entries=w.call('find_list',search)
                op=w.call('pair_begin',search,entries[0].candidate_id);self.assertEqual(w.call('wait_operation',op)['result'],0)
                return w.call('get_peer')['peer_id']
            proc=launch();w=None
            try:
                w=connect();first=pair(w);conn=w.call('get_device').connection_id
                w.call('set_reconnect',first,False);w.call('disconnect');time.sleep(.2)
                self.assertEqual(w.call('get_device').state,1)
                w.call('connect_peer',first);time.sleep(.2);self.assertNotEqual(w.call('get_device').connection_id,conn)
                w.close();w=None;proc.terminate();proc.wait(timeout=3);proc=launch();w=connect()
                self.assertEqual(w.call('get_peer')['peer_id'],first)
                self.assertFalse(w.call('get_peer')['auto_reconnect'])
                op=w.call('forget_peer',first);self.assertEqual(w.call('wait_operation',op)['result'],0)
                w.close();w=None;proc.terminate();proc.wait(timeout=3);proc=launch();w=connect()
                self.assertEqual(w.call('get_peer')['peer_id'],0)
                self.assertGreater(pair(w),first)
            finally:
                if w:w.close()
                proc.terminate();proc.wait(timeout=3)

    def test_worker_heartbeat_recording_and_gui(self):
        os.environ['QT_QPA_PLATFORM']='offscreen'
        from PySide6 import QtWidgets,QtGui
        from rbp_gui import Window
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            proc=subprocess.Popen([str(ROOT/'build/host/sim_bridge.exe'),'--data-port','45831','--control-port','45832','--quiet'],cwd=d,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            worker=None;ctrl=None;window=None
            try:
                time.sleep(.4);ctrl=socket.create_connection(('127.0.0.1',45832));worker=BridgeWorker(TcpTransport('127.0.0.1',45831))
                worker.call('hello');search=worker.call('find_start',100);time.sleep(.2)
                _,entries=worker.call('find_list',search);op=worker.call('pair_begin',search,entries[0].candidate_id)
                self.assertEqual(worker.call('wait_operation',op)['result'],0)
                self.assertEqual(worker.call('get_device').state,5)
                # The UI/CLI thread can block waiting for a human while IO keeps pinging.
                time.sleep(5.3);self.assertGreater(worker.call('ping'),0)
                worker.call('voice_enable',True);ctrl.sendall(b'mic_on\n')
                r=WaveRecorder(Path(d)/'worker.wav');deadline=time.monotonic()+8
                while time.monotonic()<deadline:
                    try:name,args=worker.next_event(.2)
                    except queue.Empty:continue
                    if name=='voice_start':r.start(*args)
                    elif name=='voice_data':r.data(*args)
                    elif name=='voice_format':r.format(*args)
                    elif name=='voice_end':r.end(*args);break
                    elif name in ('error','voice_abort'):self.fail((name,args))
                self.assertTrue(r.completed);self.assertGreater(r.samples,1000)
                worker.close();worker=None
                app=QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
                if sys.platform=='win32':
                    QtGui.QFontDatabase.addApplicationFont('C:/Windows/Fonts/segoeui.ttf')
                    app.setFont(QtGui.QFont('Segoe UI',10))
                window=Window();window.port.setEditText('tcp://127.0.0.1:45831');window.show();window.open_bridge()
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    app.processEvents();time.sleep(.01)
                    if window.keys.rowCount()==12:break
                self.assertEqual(window.keys.rowCount(),12,window.log.toPlainText())
                ctrl.sendall(b'press home\n');deadline=time.monotonic()+.3
                while time.monotonic()<deadline:app.processEvents();time.sleep(.01)
                window.grab().save(str(ROOT/'build/gui-regression.png'))
                ctrl.sendall(b'release home\n');window.close();window=None
                # Display-only prompts never send PAIR_REPLY; cancellation is operation-scoped.
                window=Window();calls=[];window.submit=lambda *a,**k:calls.append(a)
                window.prompt(dict(operation_id=99,prompt_id=4,method=3,number=123456,remaining_ms=30000))
                self.assertEqual(calls,[]);window.prompts[0].reject();app.processEvents()
                self.assertEqual(calls,[('pair_cancel',99)]);window.close();window=None
            finally:
                if window:window.close()
                if worker:worker.close()
                if ctrl:ctrl.close()
                proc.terminate();proc.wait(timeout=3)

if __name__=='__main__':unittest.main(verbosity=2)
