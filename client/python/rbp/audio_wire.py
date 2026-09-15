"""Codec-independent RBP/3 audio objects and bounded continuity validation."""
import struct
import time
from dataclasses import dataclass,astuple

UNKNOWN_INDEX=(1<<64)-1
UNKNOWN_COUNT=(1<<32)-1


class AudioError(ValueError):
    pass


@dataclass(frozen=True)
class AudioFormat:
    stream_id:int
    epoch:int
    codec_id:int
    codec_revision:int
    sample_rate:int
    channels:int
    config:bytes
    max_unit_bytes:int
    first_sample_index:int
    captured_us:int
    first_unit_seq:int

    @classmethod
    def from_tlv(cls,t):
        f=cls(*(t[i] for i in range(1,12)))
        if not (0<f.stream_id<=UNKNOWN_COUNT and 0<f.epoch<=UNKNOWN_COUNT and 0<f.codec_id<0x80000000 and 0<f.codec_revision<=65535 and
                1<=f.sample_rate<=384000 and 1<=f.channels<=8 and len(f.config)<=256 and
                1<=f.max_unit_bytes<=65536 and 0<f.first_unit_seq<=UNKNOWN_COUNT and
                0<=f.first_sample_index<=UNKNOWN_INDEX and 0<=f.captured_us<=UNKNOWN_INDEX):
            raise AudioError('invalid_format')
        return f


@dataclass(frozen=True)
class EncodedFragment:
    stream_id:int
    frame_seq:int
    epoch:int
    unit_seq:int
    unit_size:int
    fragment_offset:int
    first_sample_index:int
    unit_sample_count:int
    data:bytes

    @classmethod
    def parse(cls,p):
        if not 41<=len(p)<=512:raise AudioError('audio length')
        *values,reserved=struct.unpack_from('<IIIIIIQII',p)
        if reserved:raise AudioError('audio reserved')
        return cls(*values,p[40:])


@dataclass(frozen=True)
class AudioEnd:
    stream_id:int
    reason:int
    delivered_encoded_bytes:int
    ended_us:int
    delivered_units:int
    delivered_samples:int
    final_epoch:int


class AudioStream:
    def __init__(self,fmt,accepted,max_unit):
        self.stream_id=fmt.stream_id;self.seq=0;self.unit=1;self.offset=0
        self.samples=0;self.encoded_bytes=0;self.epoch=0;self.pending=None
        self.accepted=set(accepted);self.max_unit=max_unit;self.started=0;self.progress=0
        self.format(fmt)

    def format(self,f):
        f=AudioFormat.from_tlv(dict(enumerate(astuple(f),1)))
        if (f.codec_id,f.codec_revision) not in self.accepted or f.max_unit_bytes>self.max_unit:
            raise AudioError('unsupported_format')
        if f.stream_id!=self.stream_id or f.epoch!=self.epoch+1 or self.offset or f.first_unit_seq!=self.unit or f.first_sample_index!=self.samples:
            raise AudioError('format boundary')
        self.fmt=f;self.epoch=f.epoch

    def feed(self,c):
        self.check_timeout()
        if not c.data:raise AudioError('empty fragment')
        meta=(c.unit_seq,c.unit_size,c.first_sample_index,c.unit_sample_count,c.epoch)
        if c.stream_id!=self.stream_id or c.frame_seq!=self.seq+1 or c.epoch!=self.epoch or c.unit_seq!=self.unit or not 1<=c.unit_size<=self.fmt.max_unit_bytes or c.fragment_offset!=self.offset or c.first_sample_index!=self.samples or self.offset+len(c.data)>c.unit_size or (self.pending is not None and meta!=self.pending):
            raise AudioError('audio continuity')
        now=time.monotonic()
        if not self.offset:self.started=now
        self.progress=now;self.pending=meta;self.offset+=len(c.data);self.seq=c.frame_seq
        self.encoded_bytes+=len(c.data)
        if self.offset==c.unit_size:
            self.samples=UNKNOWN_INDEX if self.samples==UNKNOWN_INDEX or c.unit_sample_count==UNKNOWN_COUNT else self.samples+c.unit_sample_count
            if self.samples>=UNKNOWN_INDEX:self.samples=UNKNOWN_INDEX
            self.unit+=1;self.offset=0;self.pending=None

    def check_timeout(self):
        now=time.monotonic()
        if self.offset and (now-self.progress>=2 or now-self.started>=10):raise AudioError('incomplete_unit')

    def end(self,e):
        if (e.stream_id!=self.stream_id or not 0<=e.reason<=9 or
            e.delivered_encoded_bytes!=self.encoded_bytes or e.delivered_units!=self.unit-1 or
            e.delivered_samples!=self.samples or e.final_epoch!=self.epoch or
            (e.reason in (0,7,8) and self.offset)):
            raise AudioError('audio end integrity')
