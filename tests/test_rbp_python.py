"""Python protocol tests: golden vectors + wire unit tests."""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "client", "python"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from rbp import wire  # noqa: E402
from rbp.wire import Header, FrameParser, TlvWriter, parse_tlv, TlvError  # noqa: E402

failures = 0


def check(cond, msg=""):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL: {msg}")


def test_crc():
    check(wire.crc32c(b"") == 0, "crc empty")
    check(wire.crc32c(b"123456789") == 0xE3069283, "crc check value")


def test_cobs():
    # known example: "ABC" -> 04 41 42 43
    check(wire.cobs_encode(b"ABC") == bytes([4, 0x41, 0x42, 0x43]), "cobs ABC")
    check(wire.cobs_decode(bytes([4, 0x41, 0x42, 0x43])) == b"ABC", "cobs dec ABC")
    # 254 non-zero then a zero: ff aa*254 01 01
    data = b"\xaa" * 254 + b"\x00"
    enc = wire.cobs_encode(data)
    check(enc[0] == 0xFF and enc[-2:] == b"\x01\x01", "cobs block boundary")
    check(wire.cobs_decode(enc) == data, "cobs block boundary round trip")
    # random round trips
    import random
    random.seed(7)
    for _ in range(200):
        n = random.randrange(0, 700)
        blob = bytes(random.randrange(256) for _ in range(n))
        if b"\x00" == blob[-1:] and not blob:
            continue
        enc = wire.cobs_encode(blob)
        check(wire.cobs_decode(enc) == blob, "cobs random round trip")
        # malformed -> None, no crash
    check(wire.cobs_decode(b"\xff\x01") is None, "cobs truncated block")


def test_header():
    h = Header(kind=wire.KIND_RESPONSE, session_id=0xDEADBEEF, tx_seq=42,
               request_id=7, opcode=wire.OP_PING, status=0,
               connection_id=0x12345678)
    raw = h.pack()
    check(len(raw) == 32, "header size")
    h2 = Header.unpack(raw)
    check(h2.session_id == 0xDEADBEEF and h2.tx_seq == 42 and h2.request_id == 7,
          "header roundtrip")
    check(h2.connection_id == 0x12345678, "header conn")
    raw_bad = bytearray(raw)
    raw_bad[0] = ord("X")
    check(Header.unpack(bytes(raw_bad)) is None, "bad magic rejected")
    raw_bad[0] = ord("R")
    raw_bad[26] = 1
    check(Header.unpack(bytes(raw_bad)) is None, "reserved must be zero")


def test_vectors():
    path = os.path.join(os.path.dirname(__file__), "..", "protocol", "vectors",
                        "golden.json")
    with open(path, "r", encoding="utf-8") as f:
        vectors = json.load(f)
    by_name = {v["name"]: bytes.fromhex(v["data"]) for v in vectors}

    # crc vectors (u32 LE of the checksum)
    check(struct.unpack("<I", by_name["crc32c_0"])[0] == wire.crc32c(b""),
          "vector crc empty")
    check(struct.unpack("<I", by_name["crc32c_1"])[0] ==
          wire.crc32c(b"123456789"), "vector crc 123456789")
    check(struct.unpack("<I", by_name["crc32c_2"])[0] ==
          wire.crc32c(b"The quick brown fox jumps over the lazy dog"),
          "vector crc fox")

    # cobs vectors: decode matches; re-encode is identical
    for i in range(6):
        name = f"cobs_{i}"
        enc = by_name[name]
        dec = wire.cobs_decode(enc)
        check(dec is not None, f"{name} decodes")
        check(wire.cobs_encode(dec) == enc, f"{name} re-encode identical")

    # frame vectors parse and re-encode identically
    def parse_frame(frame):
        assert frame[-1] == 0
        raw = wire.cobs_decode(frame[:-1])
        hdr = Header.unpack(raw[:32])
        plen = hdr.payload_size
        payload = raw[32:32 + plen]
        crc_ok = struct.unpack("<I", raw[32 + plen:])[0] == wire.crc32c(raw[:32 + plen])
        return hdr, payload, crc_ok

    hdr, payload, ok = parse_frame(by_name["hello_request_frame"])
    check(ok and hdr.opcode == wire.OP_HELLO and hdr.request_id == 1 and
          hdr.kind == wire.KIND_REQUEST, "hello request frame")
    t = parse_tlv(payload)
    check(t[1] == bytes(range(0xA0, 0xB0)), "hello nonce bytes")
    re = wire.encode_frame(hdr, payload, hdr.tx_seq)
    check(re == by_name["hello_request_frame"], "hello request re-encode")

    hdr, payload, ok = parse_frame(by_name["hello_response_frame"])
    check(ok and hdr.status == 0 and hdr.session_id == 0x01020304,
          "hello response frame")
    t = parse_tlv(payload)
    check(t[3] == "bridge sim 1.0.0" and t[4] == 64 and t[5] == 512 and t[6] == 0x0F,
          "hello response TLVs")

    ks = by_name["keys_state_physical"]
    seq, captured, bits = struct.unpack("<IQQ", ks[:20])
    kind, reason = ks[20], ks[21]
    check(seq == 7 and kind == 1 and bits == (1 << 8) | (1 << 2) and reason == 0,
          "keys state struct")

    vd=by_name['voice_data_payload']
    sid,seq,epoch,unit,size,offset,index,count,res=struct.unpack('<IIIIIIQII',vd[:40])
    check((sid,seq,epoch,unit,size,offset,index,count,res)==(0x12,3,1,3,2,0,480,4,0),'compressed unit layout')
    check(vd[40:]==bytes.fromhex('1780'),'compressed bytes unchanged')

    hdr, payload, ok = parse_frame(by_name["voice_started_event"])
    check(ok and hdr.kind == wire.KIND_EVENT and hdr.opcode == wire.OP_VOICE_STARTED_EV
          and hdr.tx_seq == 9, "voice started event header")
    t = parse_tlv(payload)
    check(t[1] == 0x12 and t[2] == 1 and t[3] == 1 and t[4] == 1 and t[5] == 16000 and t[10] == 123456789,
          "voice started TLVs")

    hdr, payload, ok = parse_frame(by_name["ping_unknown_tag_frame"])
    check(ok, "ping unknown tag frame")
    t = parse_tlv(payload, strict_scalars=False)
    check(t[1] == 0x12345678, "ping cookie after skipping unknown tag")
    check(isinstance(t[0xF7], tuple) and t[0xF7][0] == 0x55, "unknown tag preserved raw")


def test_tlv_errors():
    try:
        TlvWriter().u8(3, 1).u8(2, 1)
        check(False, "descending tags must raise")
    except TlvError:
        pass
    try:
        parse_tlv(bytes([9, 1, 1, 0, 0xAA, 9, 1, 1, 0, 0xBB]))
        check(False, "duplicate tag must raise")
    except TlvError:
        pass
    try:
        parse_tlv(bytes([9, 1]))
        check(False, "truncated header must raise")
    except TlvError:
        pass
    try:
        parse_tlv(bytes([4, 6, 2, 0, 0xC0, 0x80]))
        check(False, "overlong utf8 must raise")
    except TlvError:
        pass
    try:
        parse_tlv(bytes([4, 6, 1, 0, 0x00]))
        check(False, "embedded NUL must raise")
    except TlvError:
        pass
    v = parse_tlv(bytes([5, 5, 1, 0, 2]), strict_scalars=False)
    check(isinstance(v[5], tuple), "unknown-typed tag kept raw")


def test_parser_streaming():
    h = Header(kind=wire.KIND_REQUEST, opcode=wire.OP_PING, request_id=3)
    frame = wire.encode_frame(h, b"", 1)
    p = FrameParser()
    check(p.feed(b"", 0) == [], "empty feed")
    evs = p.feed(frame[:5], 0)
    check(evs == [], "partial frame no events")
    evs = p.feed(frame[5:], 10)
    check(len(evs) == 1 and evs[0][0] == "frame", "frame completes")
    check(evs[0][1].opcode == wire.OP_PING and evs[0][3], "frame content")

    # corrupt CRC (second to last byte: crc high byte)
    bad = bytearray(frame)
    bad[-2] ^= 0x40
    p = FrameParser()
    evs = p.feed(bytes(bad), 0)
    check(len(evs) == 1 and evs[0][0] == "frame" and not evs[0][3], "bad crc flagged")

    # garbage + two frames in one feed
    f2 = wire.encode_frame(Header(kind=wire.KIND_REQUEST, opcode=wire.OP_GET_STATS,
                                  request_id=4), b"", 2)
    stream = b"\x00" + frame + b"\x77\x00" + f2
    p = FrameParser()
    evs = p.feed(stream, 0)
    kinds = [e[0] for e in evs]
    check(kinds == ["frame", "garbage", "frame"], "two frames + garbage from one feed")

    # overflow
    p = FrameParser()
    evs = p.feed(bytes([0xAB]) * 600, 0)
    check(evs and evs[0][0] == "overflow", "overflow detected")
    evs = p.feed(frame, 0)
    check(not evs, "overflow discard persists through next delimiter")
    evs = p.feed(frame, 0)
    check(evs and evs[0][0] == "frame", "recovers after overflow delimiter")

    # half-frame timeout
    p = FrameParser()
    p.feed(frame[:10], 1000)
    evs = p.feed(b"", 2100)
    check(evs and evs[0][0] == "timeout", "half-frame timeout")


def main():
    test_crc()
    test_cobs()
    test_header()
    test_vectors()
    test_tlv_errors()
    test_parser_streaming()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("python protocol tests: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
