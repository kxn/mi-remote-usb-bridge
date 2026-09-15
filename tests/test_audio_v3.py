"""RBP3: wire state, host decoders, fragmented units and failure isolation."""
import ctypes as C
import dataclasses
import json
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'client/python'),str(ROOT/'demo')]
from rbp.audio_wire import AudioFormat,EncodedFragment,AudioEnd,AudioStream,AudioError,UNKNOWN_INDEX,UNKNOWN_COUNT
from rbp.codecs import AudioDecoder,ImaDecoder
from rbp.client import BridgeClient
from rbp import wire
from audio import WaveRecorder


def fmt(**kw):
    return dataclasses.replace(AudioFormat(1,1,1,1,16000,1,b'\0'*4,1024,0,0,1),**kw)


class IMA(C.Structure):
    _fields_=[('predictor',C.c_int16),('index',C.c_uint8)]
class Decoder(C.Structure):
    _fields_=[('size',C.c_uint32),('abi',C.c_uint32),('maximum',C.c_uint32),('rate',C.c_uint32),('ima',IMA),('ready',C.c_uint8)]
class Format(C.Structure):
    _fields_=[('codec',C.c_uint32),('rate',C.c_uint32),('maximum',C.c_uint32),('revision',C.c_uint16),('config_len',C.c_uint16),('channels',C.c_uint8),('config',C.POINTER(C.c_uint8))]


class AudioTests(unittest.TestCase):
    def test_slow_audio_consumer_keeps_session(self):
        import queue
        from rbp.worker import BridgeWorker
        class T:
            def write(self,b):pass
        c=BridgeClient(T());c.session_id=42;c._stream=AudioStream(fmt(),[(1,1)],1024)
        w=BridgeWorker.__new__(BridgeWorker);w.client=c;w.events=queue.Queue(40)
        w._overflow=False;w._dropping_audio=False
        c.on_voice_abort=w._callback('voice_abort')
        for i in range(12):w._callback('voice_data')(None)
        self.assertEqual(c.session_id,42);self.assertFalse(w._overflow)
        self.assertTrue(c._audio_ignored);self.assertTrue(any(x[0]=='voice_abort' for x in w.events.queue))
        w._callback('keys')('still delivered')
        self.assertEqual(w.events.get_nowait()[0],'voice_data')
        self.assertTrue(any(x[0]=='keys' for x in w.events.queue))

    def test_recording_io_failure_is_audio_error(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            r=WaveRecorder(Path(d)/'x.wav')
            with patch('pathlib.Path.mkdir',side_effect=OSError('disk unavailable')):
                with self.assertRaises(AudioError):r.start(fmt())
            self.assertIsNone(r.active)

    def test_c_python_decoder_exact(self):
        lib=C.CDLL(str(ROOT/'build/host/rbp_decoder.dll'))
        lib.rbp_decoder_init.argtypes=[C.POINTER(Decoder),C.c_uint32,C.POINTER(Format)]
        lib.rbp_decoder_decode.argtypes=[C.POINTER(Decoder),C.POINTER(C.c_uint8),C.c_uint32,C.POINTER(C.c_int16),C.c_uint32,C.POINTER(C.c_uint32)]
        rng=random.Random(42)
        for predictor,index in [(0,0),(-1234,42),(32767,88),(-32768,88)]+[(rng.randint(-32768,32767),rng.randrange(89)) for _ in range(30)]:
            config=struct.pack('<hBB',predictor,index,0);seed=(C.c_uint8*4).from_buffer_copy(config)
            cf=Format(1,16000,1024,1,4,1,seed);d=Decoder()
            self.assertEqual(lib.rbp_decoder_init(C.byref(d),C.sizeof(d),C.byref(cf)),0)
            py=ImaDecoder(fmt(config=config))
            for length in [1,120,256,7,512]:
                encoded=rng.randbytes(length);src=(C.c_uint8*length).from_buffer_copy(encoded)
                out=(C.c_int16*(length*2))();written=C.c_uint32()
                before=bytes(d)
                self.assertEqual(lib.rbp_decoder_decode(C.byref(d),src,length,out,length*2-1,C.byref(written)),3)
                self.assertEqual(bytes(d),before);self.assertEqual(written.value,0)
                self.assertEqual(lib.rbp_decoder_decode(C.byref(d),src,length,out,length*2,C.byref(written)),0)
                self.assertEqual(bytes(out),py.decode(encoded));self.assertEqual(written.value,length*2)
                self.assertEqual((d.ima.predictor,d.ima.index),(py.predictor,py.index))
        seed[2]=89
        self.assertEqual(lib.rbp_decoder_init(C.byref(d),C.sizeof(d),C.byref(cf)),2)
        cf.codec=999;self.assertEqual(lib.rbp_decoder_init(C.byref(d),C.sizeof(d),C.byref(cf)),1)

    def test_vectors_and_reseed(self):
        for row in json.loads((ROOT/'protocol/v3/codec-vectors.json').read_text()):
            d=ImaDecoder(fmt(config=bytes.fromhex(row['config_hex'])))
            self.assertEqual(d.decode(bytes.fromhex(row['encoded_hex'])).hex(),row['pcm_s16le_hex'])
        decoder=AudioDecoder(fmt());self.assertEqual(decoder.data(EncodedFragment(1,1,1,1,1,0,0,2,b'\x17')),b'\x01\0\x0c\0')
        decoder.format(fmt(epoch=2,first_sample_index=2,first_unit_seq=2,config=bytes.fromhex('2efb2a00')))
        self.assertEqual(decoder.data(EncodedFragment(1,2,2,2,1,0,2,2,b'\x17')).hex(),'c7fb7dfe')

    def test_fragment_boundaries(self):
        state=AudioStream(fmt(),[(1,1)],1024);decoder=AudioDecoder(fmt())
        data=bytes(range(256))*2
        for seq,offset in enumerate([0,256],1):
            c=EncodedFragment(1,seq,1,1,512,offset,0,1024,data[offset:offset+256]);state.feed(c)
            pcm=decoder.data(c)
            self.assertEqual(len(pcm),0 if offset==0 else 2048)
        state.end(AudioEnd(1,0,512,0,1,1024,1))
        for change in [dict(frame_seq=3),dict(epoch=2),dict(unit_seq=2),dict(unit_size=1025),dict(fragment_offset=1),dict(first_sample_index=1)]:
            st=AudioStream(fmt(),[(1,1)],1024)
            with self.assertRaises(AudioError):st.feed(dataclasses.replace(EncodedFragment(1,1,1,1,1,0,0,2,b'\0'),**change))

    def test_partial_end_timeout_unknown(self):
        st=AudioStream(fmt(),[(1,1)],1024)
        with patch('rbp.audio_wire.time.monotonic',return_value=0):st.feed(EncodedFragment(1,1,1,1,2,0,0,4,b'\0'))
        with self.assertRaises(AudioError):st.end(AudioEnd(1,0,1,0,0,0,1))
        st.end(AudioEnd(1,3,1,0,0,0,1))
        with patch('rbp.audio_wire.time.monotonic',return_value=2.1):
            with self.assertRaises(AudioError):st.check_timeout()
        st=AudioStream(fmt(),[(1,1)],1024)
        st.feed(EncodedFragment(1,1,1,1,1,0,0,UNKNOWN_COUNT,b'\0'))
        st.feed(EncodedFragment(1,2,1,2,1,0,UNKNOWN_INDEX,2,b'\0'))
        st.end(AudioEnd(1,0,2,0,2,UNKNOWN_INDEX,1))

    def test_unknown_codec_keeps_session(self):
        c=BridgeClient(None);c.session_id=5;c.connection_id=1;c._accepted_codecs=((1,1),);c._audio_max_unit=1024
        errors=[];c.on_voice_abort=lambda *a:errors.append(a)
        f=fmt(codec_id=999)
        w=wire.TlvWriter()
        for tag,typ,value in zip(range(1,12),['u32','u32','u32','u16','u32','u8','blob','u32','u64','u64','u32'],dataclasses.astuple(f)):
            getattr(w,typ)(tag,value)
        h=wire.Header(kind=wire.KIND_EVENT,session_id=5,connection_id=1,opcode=wire.OP_VOICE_STARTED_EV,tx_seq=1,payload_size=len(w.data))
        c._handle_frame(h,w.data,True)
        self.assertEqual(c.session_id,5);self.assertTrue(errors);self.assertEqual(c._replies[0],{'voice_disable':True})

    def test_raw_capture_replay(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
            target=Path(d)/'test.wav';r=WaveRecorder(target);f=fmt();r.start(f)
            r.data(EncodedFragment(1,1,1,1,1,0,0,2,b'\x17'));r.end(AudioEnd(1,0,1,0,1,2,1))
            rows=[json.loads(l) for l in target.with_suffix('.encoded.jsonl').read_text().splitlines()]
            self.assertEqual(rows[0]['version'],1);self.assertEqual(rows[1]['value']['config'],{'hex':'00000000'})
            self.assertEqual(rows[2]['value']['data'],{'hex':'17'})
            manifest=json.loads(target.with_suffix('.json').read_text());self.assertTrue(manifest['complete']);self.assertEqual(manifest['samples'],2)
            from replay_audio import replay
            output=Path(d)/'replayed.wav'
            replay(target.with_suffix('.encoded.jsonl'),output)
            self.assertEqual(target.read_bytes(),output.read_bytes())
            with self.assertRaises(ValueError):replay(target.with_suffix('.encoded.jsonl'),target)


if __name__=='__main__':unittest.main(verbosity=2)
