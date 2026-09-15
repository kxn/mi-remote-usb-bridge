"""RBP/3.0 specification artifacts. Generated from the RBP/3 runtime schema."""
import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def fields(spec):
    types = {'u8': 1, 'u16': 2, 'u32': 3, 'u64': 4, 'bool': 5, 'text': 6, 'bytes': 7}
    out = {}
    for token in spec.split():
        tag, name, typ = token.split(':')
        out[tag] = dict(name=name, type=types[typ.rstrip('?')], required=not typ.endswith('?'))
    return out


def crc32c(data):
    crc = 0xffffffff
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def cobs(raw):
    out = bytearray(b'\0'); code_pos = 0; code = 1
    for b in raw:
        if b:
            out.append(b); code += 1
            if code < 255:
                continue
        out[code_pos] = code
        code_pos = len(out); out.append(0); code = 1
    out[code_pos] = code
    return bytes(out) + b'\0'


def frame(kind, opcode, seq, payload, session=1, connection=1, request=0):
    raw = struct.pack('<2sBBBBHIIIHHHHI', b'RB', 3, 0, kind, 0, 32,
                      session, seq, request, opcode, 0, len(payload), 0, connection) + payload
    return cobs(raw + struct.pack('<I', crc32c(raw)))


def tlv(*items):
    formats = {1: '<B', 2: '<H', 3: '<I', 4: '<Q', 5: '<?'}
    result = b''
    for tag, typ, value in items:
        value = struct.pack(formats[typ], value) if typ in formats else value
        result += struct.pack('<BBH', tag, typ, len(value)) + value
    return result


def descriptor(epoch=1, index=0, unit=1, predictor=0, step=0, rate=16000):
    return tlv((1,3,1), (2,3,epoch), (3,3,1), (4,2,1), (5,3,rate), (6,1,1),
               (7,7,struct.pack('<hBB',predictor,step,0)), (8,3,1024),
               (9,4,index), (10,4,1000000), (11,3,unit))


def data(seq, unit, size, offset, index, samples, blob, epoch=1):
    return struct.pack('<IIIIIIQII', 1, seq, epoch, unit, size, offset, index, samples, 0) + blob


def build():
    # Derive management definitions from the current executable schema.
    import sys
    sys.path.insert(0,str(ROOT/'client/python'))
    from rbp.schema import document
    doc = json.loads(json.dumps(document()))
    doc.update(version=[3,0], implementation_status='implemented_hardware_validation_pending',
               normative_document='../../docs/wire-protocol.md',
               audio_document='../../docs/audio-wire-v3.md')
    descriptor_fields = fields('1:stream_id:u32 2:epoch:u32 3:codec_id:u32 4:codec_revision:u16 5:sample_rate:u32 6:channels:u8 7:codec_config:bytes 8:max_unit_bytes:u32 9:first_sample_index:u64 10:captured_us:u64 11:first_unit_seq:u32')
    for msg in doc['messages']:
        op = msg['opcode']
        if op == 0x300:
            msg['request'] = fields('1:enabled:bool 2:accepted_codecs:bytes? 3:max_unit_bytes:u32?')
            msg['constraints'] = ['enabled=true requires tags 2,3; false forbids them',
                                  'codec entries sorted unique <IH; count 1..16',
                                  'max_unit_bytes 1..65536; acceptance exact id/revision']
        if op in (0x380, 0x383):
            msg['fields'] = descriptor_fields
        if op == 0x382:
            msg['fields'] = fields('1:stream_id:u32 2:reason:u8 3:delivered_encoded_bytes:u64 4:ended_us:u64 5:delivered_units:u32 6:delivered_samples:u64 7:final_epoch:u32')
    doc['messages'].append(dict(opcode=0x302, name='GET_VOICE_CAPS', connection='current',
        request={}, response=fields('1:codecs:bytes 2:max_unit_bytes:u32 3:config_max_bytes:u16'), success_status=0))
    doc['binary']['voice_data'] = dict(format='<IIIIIIQII', header_bytes=40,
        fields=['stream_id','frame_seq','epoch','unit_seq','unit_size','fragment_offset',
                'first_sample_index','unit_sample_count','reserved'],
        tail='1..472 original encoded bytes', max_unit_bytes=65536,
        unknown_sample_index='0xffffffffffffffff', unknown_sample_count='0xffffffff')
    doc['audio'] = dict(config_max_bytes=256, sample_rate=[1,384000], channels=[1,8],
        codecs=[dict(id=1, revision=1, name='IMA_ADPCM_CONTINUOUS_HI',
                     config_format='<hBB', config_fields=['predictor','step_index','reserved'],
                     rates=[8000,16000], channels=1, state='continuous; replace at each epoch',
                     samples_per_byte=2, nibble_order='high_then_low', initial_sample_emitted=False)],
        end_reasons=['normal','consumer_disabled','link_lost','buffer_overrun','invalid_encoded_data',
                     'source_data_lost','device_error','requested_stop','capture_limit','unsupported_format'])
    (HERE/'schema.json').write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    vectors = []
    def add(name, kind, op, seq, payload, **kw):
        vectors.append(dict(name=name, kind=kind, opcode=op, payload_hex=payload.hex(),
                            wire_hex=frame(kind,op,seq,payload,**kw).hex()))
    add('host_start_request',1,0x303,1,b'',request=1)
    add('enable_ima',1,0x300,1,tlv((1,5,True),(2,7,struct.pack('<IH',1,1)),(3,3,1024)),request=1)
    add('start_seed_zero',3,0x380,1,descriptor())
    # A 600-byte stateful unit exercises two RBP fragments without changing codec framing.
    blob = bytes(range(256))*2 + bytes(range(88))
    add('unit1_fragment0',4,0x381,2,data(1,1,600,0,0,1200,blob[:472]))
    add('unit1_fragment472',4,0x381,3,data(2,1,600,472,0,1200,blob[472:]))
    add('same_rate_new_seed',3,0x383,4,descriptor(epoch=2,index=1200,unit=2,predictor=-1234,step=42))
    add('unit2',4,0x381,5,data(3,2,1,0,1200,2,b'\x17',epoch=2))
    add('end_normal',3,0x382,6,tlv((1,3,1),(2,1,0),(3,4,601),(4,4,2000000),(5,3,2),(6,4,1202),(7,3,2)))
    (HERE/'vectors.json').write_text(json.dumps(vectors,indent=2)+'\n',encoding='utf-8')


if __name__ == '__main__':
    build()
