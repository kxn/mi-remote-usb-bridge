"""Validate draft v3 vectors, boundaries and decoder metadata (not hardware)."""
import json
import re
import struct
from pathlib import Path
from build_contract import crc32c, data, descriptor, tlv

HERE = Path(__file__).resolve().parent


def uncobs(wire):
    assert wire[-1] == 0 and 0 not in wire[:-1]
    out = bytearray(); pos = 0
    while pos < len(wire)-1:
        n = wire[pos]; pos += 1
        assert pos+n-1 <= len(wire)-1
        out.extend(wire[pos:pos+n-1]); pos += n-1
        if n != 255 and pos < len(wire)-1: out.append(0)
    return bytes(out)


def parse_tlv(payload):
    out = {}; pos = 0
    formats = {1:'<B',2:'<H',3:'<I',4:'<Q',5:'<?'}
    while pos < len(payload):
        assert len(payload)-pos >= 4
        tag, typ, length = struct.unpack_from('<BBH',payload,pos); pos += 4
        assert tag not in out and pos+length <= len(payload)
        b = payload[pos:pos+length]; pos += length
        out[tag] = struct.unpack(formats[typ],b)[0] if typ in formats else b
    return out


class Stream:
    def __init__(self):
        self.epoch=0; self.seq=0; self.unit=1; self.offset=0
        self.index=0; self.byte_count=0; self.pending=None; self.active=False

    def format(self,payload,start=False):
        d=parse_tlv(payload)
        assert set(range(1,12)) <= d.keys()
        assert d[1]==1 and d[2]==self.epoch+1 and (d[3],d[4])==(1,1)
        assert d[5] in (8000,16000) and d[6]==1 and len(d[7])==4
        pred,step,res=struct.unpack('<hBB',d[7]); assert step<=88 and res==0
        assert 1<=d[8]<=1024 and d[9]==self.index and d[11]==self.unit
        assert self.offset==0 and start != self.active
        self.active=True; self.epoch=d[2]; self.limit=d[8]; self.seed=(pred,step)

    def feed(self,payload):
        assert self.active and 41<=len(payload)<=512
        sid,seq,epoch,unit,size,offset,index,samples,res=struct.unpack_from('<IIIIIIQII',payload)
        assert sid==1 and seq==self.seq+1 and epoch==self.epoch and unit==self.unit
        assert 1<=size<=self.limit and offset==self.offset and index==self.index and res==0
        assert samples==size*2 and offset+len(payload)-40<=size
        meta=(epoch,unit,size,index,samples)
        assert self.pending is None or self.pending==meta
        self.pending=meta; self.seq=seq; self.offset+=len(payload)-40
        self.byte_count+=len(payload)-40
        if self.offset==size:
            self.index+=samples; self.unit+=1; self.offset=0; self.pending=None

    def end(self,payload):
        d=parse_tlv(payload)
        assert self.active and d[1]==1 and 0<=d[2]<=9
        assert d[3]==self.byte_count and d[5]==self.unit-1 and d[6]==self.index and d[7]==self.epoch
        assert d[2] not in (0,7,8) or self.offset==0
        self.active=False


def check():
    assert crc32c(b'123456789')==0xe3069283
    schema=json.loads((HERE/'schema.json').read_text(encoding='utf-8'))
    assert schema['version']==[3,0]
    assert struct.calcsize(schema['binary']['voice_data']['format'])==40
    rows=json.loads((HERE/'vectors.json').read_text(encoding='utf-8'))
    stream=Stream()
    for row in rows:
        raw=uncobs(bytes.fromhex(row['wire_hex']))
        assert raw[:4]==b'RB\x03\x00' and crc32c(raw[:-4])==struct.unpack('<I',raw[-4:])[0]
        assert raw[32:-4].hex()==row['payload_hex']
        assert struct.unpack_from('<H',raw,24)[0]==len(raw)-36
        payload=raw[32:-4]
        if row['opcode']==0x380: stream.format(payload,True)
        elif row['opcode']==0x383:
            stream.format(payload); assert stream.seed==(-1234,42)
        elif row['opcode']==0x381: stream.feed(payload)
        elif row['opcode']==0x382: stream.end(payload)
    assert stream.index==1202 and stream.byte_count==601 and not stream.active
    # Check the normative profile examples against its documented integer arithmetic.
    specification=(HERE.parents[1]/'docs/audio-wire-v3.md').read_text(encoding='utf-8')
    table_text=specification.split('step_table：')[1].split('```text')[1].split('```')[0]
    steps=list(map(int,re.findall(r'\d+',table_text)))
    assert len(steps)==89
    codec_rows=json.loads((HERE/'codec-vectors.json').read_text(encoding='utf-8'))
    for row in codec_rows:
        predictor,index,res=struct.unpack('<hBB',bytes.fromhex(row['config_hex']))
        assert index<=88 and res==0
        samples=[]
        for byte in bytes.fromhex(row['encoded_hex']):
            for nibble in (byte>>4,byte&15):
                step=steps[index]
                diff=(step>>3)+(step if nibble&4 else 0)+((step>>1) if nibble&2 else 0)+((step>>2) if nibble&1 else 0)
                predictor=max(-32768,min(32767,predictor+(-diff if nibble&8 else diff)))
                index=max(0,min(88,index+[-1,-1,-1,-1,2,4,6,8][nibble&7]))
                samples.append(predictor)
        assert struct.pack('<'+'h'*len(samples),*samples).hex()==row['pcm_s16le_hex']
        assert len(samples)==row['sample_frames']
        assert (predictor,index)==(row['final_predictor'],row['final_step_index'])
    rejected=0
    def bad(action):
        nonlocal rejected
        s=Stream(); s.format(descriptor(),True)
        try: action(s)
        except (AssertionError,struct.error): rejected+=1
        else: raise AssertionError('invalid stream accepted')
    bad(lambda s:s.feed(data(2,1,1,0,0,2,b'\0'))) # sequence gap
    bad(lambda s:s.feed(data(1,1,1,0,0,2,b'\0',epoch=2)))
    bad(lambda s:s.feed(data(1,1,1025,0,0,2050,b'\0')))
    bad(lambda s:s.feed(data(1,1,1,1,0,2,b'\0')))
    bad(lambda s:s.feed(data(1,1,1,0,0,2,b'\0\0')))
    bad(lambda s:s.feed(data(1,1,1,0,1,2,b'\0')))
    bad(lambda s:s.feed(data(1,1,1,0,0,1,b'\0')))
    bad(lambda s:s.feed(data(1,1,1,0,0,2,b'')))
    bad(lambda s:(s.feed(data(1,1,2,0,0,4,b'\0')),s.feed(data(2,2,1,0,0,2,b'\0'))))
    bad(lambda s:(s.feed(data(1,1,2,0,0,4,b'\0')),s.format(descriptor(epoch=2))))
    bad(lambda s:(s.feed(data(1,1,2,0,0,4,b'\0')),s.end(tlv((1,3,1),(2,1,0),(3,4,1),(4,4,0),(5,3,0),(6,4,0),(7,3,1)))))
    bad(lambda s:s.format(descriptor(epoch=2,step=89)))
    print(f'v3 contract: {len(rows)} wire vectors, {len(codec_rows)} codec vectors; fragmented unit, same-rate reseed, exact END; {rejected} malformed cases rejected')


if __name__=='__main__': check()
