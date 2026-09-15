"""Regressions for the five findings in review-rbp3-050.md."""
import dataclasses,io,json,queue,sys,tempfile,time,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch,Mock
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'client/python'),str(ROOT/'demo')]
from rbp.audio_wire import AudioFormat,EncodedFragment,AudioEnd,AudioError
from rbp.codecs import AudioDecoder
from rbp.client import DeviceInfo
from audio import WaveRecorder
import rbp_demo,rbp_diag

def fmt():return AudioFormat(1,1,1,1,16000,1,b'\0'*4,512,0,0,1)
def end(reason=0):return AudioEnd(1,reason,0,0,0,0,1)

class AuditTests(unittest.TestCase):
    def test_regular_cli_end_codes(self):
        class Worker:
            def __init__(self,reason):self.events=iter([('voice_start',(fmt(),)),('voice_end',(end(reason),))])
            def call(self,*a):pass
            def next_event(self,*a):return next(self.events)
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d,patch.object(rbp_demo,'ready'),patch('sys.stdout',new_callable=io.StringIO):
            for reason,code in [(0,0),(7,0),(8,0),(6,1)]:
                args=SimpleNamespace(output=str(Path(d)/f'{reason}.wav'),timeout=2)
                self.assertEqual(rbp_demo.voice(Worker(reason),args),code)
                self.assertEqual(json.loads(Path(args.output).with_suffix('.json').read_text())['complete'],code==0)

    def test_decoder_identity_and_fail_closed(self):
        first=EncodedFragment(1,1,1,1,2,0,0,4,b'\x17')
        second=EncodedFragment(1,2,1,1,2,1,0,4,b'\x80')
        for changes in [dict(stream_id=99),dict(unit_seq=99),dict(frame_seq=99),dict(epoch=2),dict(unit_size=3),dict(first_sample_index=1),dict(unit_sample_count=6),dict(fragment_offset=0)]:
            decoder=AudioDecoder(fmt());self.assertEqual(decoder.data(first),b'')
            with self.assertRaises(AudioError):decoder.data(dataclasses.replace(second,**changes))
            self.assertFalse(decoder.buffer)
            with self.assertRaises(AudioError):decoder.data(second)
        with self.assertRaises(AudioError):AudioDecoder(dataclasses.replace(fmt(),stream_id=0))

    def test_record_end_and_cleanup_faults(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            r=WaveRecorder(Path(d)/'end.wav');r.start(fmt());handles=(r.file,r.raw)
            with patch.object(r,'_record',side_effect=OSError('disk full')):
                with self.assertRaises(AudioError):r.end(end())
                r.abort('second cleanup')
            self.assertIsNone(r.active);self.assertTrue(r.cleanup_errors)
            self.assertTrue(handles[1].closed)
            self.assertFalse(json.loads((Path(d)/'end.json').read_text())['complete'])
            r=WaveRecorder(Path(d)/'close.wav');r.start(fmt())
            old=r.file;old.close();r.file=Mock();r.file.close.side_effect=OSError('flush failed')
            with patch.object(Path,'write_text',side_effect=OSError('manifest failed')):
                r.abort('stop')
            self.assertIsNone(r.active);self.assertIsNone(r.raw);self.assertEqual(len(r.cleanup_errors),2)
            r.abort('idempotent')
            r=WaveRecorder(Path(d)/'manifest.wav');r.start(fmt())
            with patch.object(Path,'write_text',side_effect=OSError('manifest failed')):
                with self.assertRaises(AudioError):r.end(end())
            self.assertIsNone(r.active)

    def test_diagnostic_disk_error_keeps_keys_and_session(self):
        events=[];calls=[];workers=[]
        class Trace:
            frames=0;bad_frames=0
            def __init__(self,*a,**k):pass
            def emit(self,name,*a,**k):events.append((name,k))
            def close(self):pass
        class Worker:
            def __init__(self,*a):self.i=iter([('voice_start',(fmt(),)),('voice_end',(end(),)),('keys',(SimpleNamespace(pressed_bits=0),))]);workers.append(self);self.closed=False
            def call(self,name,*a):
                calls.append((name,a))
                if name=='hello':return {3:'test'}
                if name=='get_device':return DeviceInfo(connection_id=1,peer_id=7,state=5,voice_state=2,voice_enabled=True)
                if name=='get_peer':return {'peer_id':7}
                if name=='key_catalog':return []
                if name in ('keys_snapshot','events_enable','voice_enable'):return None
                raise AssertionError(name)
            def next_event(self,*a):
                try:return next(self.i)
                except StopIteration:time.sleep(.002);raise queue.Empty
            def close(self):self.closed=True
        original=WaveRecorder._record
        def fail_end(self,event,value):
            if event in ('end','abort'):raise OSError('disk full')
            return original(self,event,value)
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            args=SimpleNamespace(compact_log=False,log=str(Path(d)/'log'),seconds=.1,port='MOCK',tcp=None,pair=False,record_dir=d)
            with patch.object(rbp_diag,'Trace',Trace),patch.object(rbp_diag,'BridgeWorker',Worker),patch.object(rbp_diag,'SerialTransport',lambda *a:None),patch.object(WaveRecorder,'_record',fail_end):
                rbp_diag.run(args)
        self.assertEqual(len(workers),1);self.assertTrue(workers[0].closed)
        self.assertIn(('voice_enable',(False,)),calls)
        self.assertTrue(any(n=='buttons' for n,_ in events))
        self.assertTrue(any(n=='audio_consumer_failed' for n,_ in events))
        self.assertFalse(any(n=='connection_failed' for n,_ in events))

if __name__=='__main__':unittest.main(verbosity=2)
