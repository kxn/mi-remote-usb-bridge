"""Optional host decoders. No BLE/ATVV dependency; no decoding in USB IO."""
import struct
from .audio_wire import AudioError,UNKNOWN_COUNT,AudioStream

from .ico import NativeIco,available
SUPPORTED_CODECS=((1,1),(2,1)) if available() else ((1,1),)

def make_decoder(fmt):
    if fmt.codec_id==1:return ImaDecoder(fmt)
    if (fmt.codec_id,fmt.codec_revision)==(2,1):
        if fmt.sample_rate!=16000 or fmt.channels!=1 or fmt.config or fmt.max_unit_bytes!=40:
            raise AudioError("invalid_format")
        try:return NativeIco()
        except (OSError,AttributeError) as e:raise AudioError("ICO native decoder unavailable") from e
    raise AudioError("unsupported_format")

STEPS=(7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,
11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767)
INDEX=(-1,-1,-1,-1,2,4,6,8)


class ImaDecoder:
    def __init__(self,fmt):
        if (fmt.codec_id,fmt.codec_revision)!=(1,1):raise AudioError('unsupported_format')
        if fmt.sample_rate not in (8000,16000) or fmt.channels!=1 or len(fmt.config)!=4:
            raise AudioError('invalid_format')
        self.predictor,self.index,reserved=struct.unpack('<hBB',fmt.config)
        if self.index>88 or reserved:raise AudioError('invalid_format')

    def decode(self,data):
        result=bytearray(len(data)*4);pos=0
        for byte in data:
            for nibble in (byte>>4,byte&15):
                step=STEPS[self.index]
                delta=(step>>3)+(step if nibble&4 else 0)+((step>>1) if nibble&2 else 0)+((step>>2) if nibble&1 else 0)
                self.predictor=max(-32768,min(32767,self.predictor+(-delta if nibble&8 else delta)))
                self.index=max(0,min(88,self.index+INDEX[nibble&7]))
                struct.pack_into('<h',result,pos,self.predictor);pos+=2
        return bytes(result)


class AudioDecoder:
    """Standalone validated stream decoder. Any error ends this instance.

    Input fragments need no external continuity validator. Create a new
    instance for each START; FORMAT cannot recover a corrupt stream.
    """
    def __init__(self,fmt):
        self.buffer=bytearray();self.failed=False;self.fmt=fmt
        self.check=AudioStream(fmt,SUPPORTED_CODECS,65536)
        self.decoder=make_decoder(fmt)

    def close(self):
        self.failed=True;self.buffer.clear()
        if hasattr(self.decoder,"close"):self.decoder.close()

    def _ready(self):
        if self.failed:raise AudioError('decoder stream already failed or ended')

    def format(self,fmt):
        self._ready()
        try:
            self.check.format(fmt)
            if hasattr(self.decoder,"close"):self.decoder.close()
            self.decoder=make_decoder(fmt);self.fmt=fmt
        except ValueError:
            self.close();raise

    def data(self,c):
        self._ready()
        try:
            self.check.feed(c)
            # Sample counts are validated against the selected codec profile.
            if c.unit_sample_count!=UNKNOWN_COUNT and c.unit_sample_count!=(320 if self.fmt.codec_id==2 else c.unit_size*2):
                raise AudioError('local_decode_error: sample count')
            if self.fmt.codec_id==2 and c.unit_size!=40:raise AudioError("invalid ICO unit size")
            self.buffer.extend(c.data)
            if len(self.buffer)<c.unit_size:return b''
            pcm=self.decoder.decode(self.buffer);self.buffer.clear()
            return pcm
        except ValueError:
            self.close();raise

    def end(self,event):
        self._ready()
        try:self.check.end(event)
        finally:
            self.failed=True;self.buffer.clear()
            if hasattr(self.decoder,"close"):self.decoder.close()
