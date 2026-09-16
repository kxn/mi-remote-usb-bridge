"""Captured FC -> board reassembly -> host decoder -> independent recorded PCM hashes."""
import dataclasses
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys
import os
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'client/python'))
from rbp.audio_wire import AudioFormat,EncodedFragment,AudioEnd,AudioError
from rbp.codecs import AudioDecoder,SUPPORTED_CODECS

def format():return AudioFormat(1,1,2,1,16000,1,b'',40,0,0,1)

class IcoTests(unittest.TestCase):
    def test_missing_native_keeps_ima_only(self):
        env=dict(os.environ,RBP_ICO_LIBRARY=str(ROOT/'build/nonexistent-ico.dll'),PYTHONPATH=str(ROOT/'client/python'))
        subprocess.run([sys.executable,'-c','from rbp.codecs import SUPPORTED_CODECS; assert SUPPORTED_CODECS==((1,1),)'],env=env,check=True)
    def test_generic_c_api_and_legacy_ima_prefix(self):
        from test_audio_v3 import Decoder as Legacy,Format as CFormat
        class Decoder(C.Structure):
            _fields_=Legacy._fields_+[('codec',C.c_uint32),('state',C.c_uint64*128)]
        lib=C.CDLL(str(ROOT/'build/ico/rbp_decoder.dll'))
        lib.rbp_decoder_init.argtypes=[C.c_void_p,C.c_uint32,C.POINTER(CFormat)]
        lib.rbp_decoder_decode.argtypes=[C.c_void_p,C.c_void_p,C.c_uint32,C.c_void_p,C.c_uint32,C.POINTER(C.c_uint32)]
        seed=(C.c_uint8*4)();legacy=Legacy();f=CFormat(1,16000,1024,1,4,1,seed)
        self.assertEqual(lib.rbp_decoder_init(C.byref(legacy),C.sizeof(legacy),C.byref(f)),0)
        for v in json.loads((ROOT/'protocol/v3/ico-vectors.json').read_text()):
            d=Decoder();f=CFormat(2,16000,40,1,0,1,None)
            self.assertEqual(lib.rbp_decoder_init(C.byref(d),C.sizeof(d),C.byref(f)),0)
            data=bytes.fromhex(v['encoded_hex']);pcm=bytearray();out=(C.c_int16*320)();written=C.c_uint32()
            for offset in range(0,len(data),40):
                frame=C.create_string_buffer(data[offset:offset+40]);before=bytes(d)
                self.assertEqual(lib.rbp_decoder_decode(C.byref(d),frame,40,out,319,C.byref(written)),3)
                self.assertEqual(bytes(d),before)
                self.assertEqual(lib.rbp_decoder_decode(C.byref(d),frame,40,out,320,C.byref(written)),0)
                self.assertEqual(written.value,320);pcm.extend(bytes(out))
            self.assertEqual(hashlib.sha256(pcm).hexdigest(),v['pcm_sha256'])

    def test_captured_vectors_and_fragmentation(self):
        self.assertIn((2,1),SUPPORTED_CODECS,'Build client/c/ico/build.py before running ICO tests')
        for v in json.loads((ROOT/'protocol/v3/ico-vectors.json').read_text()):
            encoded=bytes.fromhex(v['encoded_hex']);pcm=bytearray();d=AudioDecoder(format());seq=0
            for i in range(len(encoded)//40):
                for offset,size in [(0,11),(11,29)]:
                    seq+=1
                    pcm.extend(d.data(EncodedFragment(1,seq,1,i+1,40,offset,i*320,320,encoded[i*40+offset:i*40+offset+size])))
            self.assertEqual(hashlib.sha256(pcm).hexdigest(),v['pcm_sha256'])
            self.assertEqual(len(pcm)//2,v['samples'])
            d.end(AudioEnd(1,0,len(encoded),0,len(encoded)//40,v['samples'],1))
            if v['label']=='fb_single_01_00':
                self.assertEqual((ROOT/'build/unicom-adapter-encoded.bin').read_bytes(),encoded)
                self.assertEqual((ROOT/'build/unicom-product-encoded.bin').read_bytes(),encoded)

    def test_bad_size_samples_and_reset(self):
        for size,samples in [(39,320),(40,80)]:
            d=AudioDecoder(format())
            with self.assertRaises(AudioError):d.data(EncodedFragment(1,1,1,1,size,0,0,samples,b'\0'*size))
            with self.assertRaises(AudioError):d.format(format())
        for f in [dataclasses.replace(format(),config=b'\0'),dataclasses.replace(format(),sample_rate=8000)]:
            with self.assertRaises(AudioError):AudioDecoder(f)

if __name__=='__main__':unittest.main()
