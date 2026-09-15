"""RBP/3.0 wire format: CRC32C, COBS framing, header, TLV.

Independent Python implementation (no C dependency).  Byte-for-byte
compatible with protocol/src/rbp_frame.c and rbp_tlv.c; both are verified
against protocol/vectors/golden.json.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, replace
from typing import Optional

MAGIC = b"RB"
MAJOR = 3
MINOR = 0
HEADER_SIZE = 32
MAX_PAYLOAD = 512
MAX_DECODED = HEADER_SIZE + MAX_PAYLOAD  # header + payload, no CRC (544)
MAX_FRAME_RAW = MAX_DECODED + 4          # + CRC32C (548)
MAX_ENCODED = 552          # COBS worst case + delimiter
COBS_RECV_MAX = 551

KIND_REQUEST = 1
KIND_RESPONSE = 2
KIND_EVENT = 3
KIND_AUDIO = 4

# ---- opcodes ----
OP_HELLO = 0x0001
OP_PING = 0x0002
OP_GET_DEVICE = 0x0003
OP_GET_PEER = 0x0004
OP_GET_OPERATION = 0x0005
OP_GET_STATS = 0x0006
OP_GOODBYE = 0x0007
OP_FIND_START = 0x0100
OP_FIND_LIST = 0x0101
OP_FIND_STOP = 0x0102
OP_PAIR_BEGIN = 0x0110
OP_PAIR_REPLY = 0x0111
OP_PAIR_CANCEL = 0x0112
OP_FORGET_PEER = 0x0113
OP_SET_RECONNECT = 0x0114
OP_CONNECT_PEER = 0x0115
OP_DISCONNECT = 0x0116
OP_KEY_CATALOG = 0x0200
OP_KEYS_SNAPSHOT = 0x0201
OP_EVENTS_ENABLE = 0x0202
OP_KEYS_STATE_EV = 0x0280
OP_VOICE_ENABLE = 0x0300
OP_VOICE_STOP = 0x0301
OP_VOICE_START = 0x0303
OP_VOICE_STARTED_EV = 0x0380
OP_VOICE_DATA = 0x0381
OP_VOICE_ENDED_EV = 0x0382
OP_VOICE_FORMAT_EV = 0x0383
OP_FIND_DONE_EV = 0x0180
OP_PAIR_PROMPT_EV = 0x0181
OP_OPERATION_EV = 0x0182
OP_DEVICE_STATE_EV = 0x0183

# ---- statuses ----
STATUS = {
    0: "OK", 1: "ACCEPTED", 2: "INVALID_ARGUMENT", 3: "UNSUPPORTED",
    4: "BAD_STATE", 5: "BUSY", 6: "NOT_FOUND", 7: "TIMEOUT",
    8: "PAIRING_FAILED", 9: "LINK_LOST", 10: "STORAGE_FAILED",
    11: "RESOURCE_LIMIT", 12: "CANCELLED", 13: "DUPLICATE",
    14: "SESSION_MISMATCH", 15: "DEVICE_ERROR", 16: "VOICE_UNAVAILABLE",
    17: "VERSION_MISMATCH",
}


# ---------------------------------------------------------------- CRC32C

_CRC_TABLE = []


def _crc_init() -> None:
    for i in range(256):
        c = i
        for _ in range(8):
            c = (0x82F63B78 ^ (c >> 1)) if (c & 1) else (c >> 1)
        _CRC_TABLE.append(c)


_crc_init()


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


# ---------------------------------------------------------------- COBS

def cobs_encode(src: bytes) -> bytes:
    """COBS encode (no trailing delimiter)."""
    out = bytearray()
    code_pos = 0
    out.append(0)  # placeholder
    code = 1
    for b in src:
        if b == 0:
            out[code_pos] = code
            code = 1
            code_pos = len(out)
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_pos] = code
                code = 1
                code_pos = len(out)
                out.append(0)
    out[code_pos] = code
    return bytes(out)


def cobs_decode(src: bytes) -> Optional[bytes]:
    """Decode a COBS record (delimiter already removed).

    Returns None on malformed input.
    """
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        code = src[i]
        i += 1
        if code == 0:
            return None
        block = code - 1
        if n - i < block:
            return None
        out += src[i:i + block]
        i += block
        if code < 0xFF and i != n:
            out.append(0)
    return bytes(out)


# ---------------------------------------------------------------- header

@dataclass
class Header:
    kind: int = 0
    flags: int = 0
    session_id: int = 0
    tx_seq: int = 0
    request_id: int = 0
    opcode: int = 0
    status: int = 0
    payload_size: int = 0
    connection_id: int = 0
    major: int = MAJOR
    minor: int = MINOR

    def pack(self) -> bytes:
        return struct.pack(
            "<2sBBBBHIIIHHHHI",
            MAGIC, self.major, self.minor, self.kind, self.flags,
            HEADER_SIZE, self.session_id, self.tx_seq, self.request_id,
            self.opcode, self.status, self.payload_size, 0,
            self.connection_id,
        )

    @staticmethod
    def unpack(raw: bytes) -> Optional["Header"]:
        if len(raw) != HEADER_SIZE:
            return None
        (magic, major, minor, kind, flags, hdr_size, session_id, tx_seq,
         request_id, opcode, status, payload_size, reserved,
         connection_id) = struct.unpack("<2sBBBBHIIIHHHHI", raw)
        if magic != MAGIC or hdr_size != HEADER_SIZE or reserved != 0:
            return None
        return Header(kind, flags, session_id, tx_seq, request_id, opcode,
                      status, payload_size, connection_id, major, minor)


def encode_frame(hdr: Header, payload: bytes, tx_seq: int) -> bytes:
    """Encode a full wire frame: COBS(header+payload+crc) + 0x00."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    if not 1 <= tx_seq <= 0xffffffff:
        raise ValueError("tx_seq must be nonzero u32")
    h = replace(hdr, tx_seq=tx_seq)
    h.payload_size = len(payload)
    raw = h.pack() + payload + struct.pack("<I", crc32c(h.pack() + payload))
    return cobs_encode(raw) + b"\x00"


# ---------------------------------------------------------------- receiver

class FrameParser:
    """Bounded byte-stream receiver. Overflow/timeouts discard to delimiter."""
    def __init__(self): self.reset()
    def reset(self):
        self._buf = bytearray()
        self._start_ms = None
        self._discard = False
    def feed(self, data: bytes, now_ms: int):
        events = []
        if self._buf and now_ms - self._start_ms >= 1000:
            self._buf.clear(); self._discard = True
            events.append(("timeout",))
        for b in data:
            if self._discard:
                if b == 0: self._discard = False; self._start_ms = None
                continue
            if b == 0:
                if not self._buf: continue
                raw = cobs_decode(bytes(self._buf)); self._buf.clear(); self._start_ms = None
                if raw is None or not HEADER_SIZE + 4 <= len(raw) <= MAX_FRAME_RAW:
                    events.append(("garbage",)); continue
                hdr = Header.unpack(raw[:HEADER_SIZE])
                if hdr is None or hdr.payload_size != len(raw)-HEADER_SIZE-4 or hdr.flags or hdr.kind not in (1,2,3,4):
                    events.append(("garbage",)); continue
                good = struct.unpack("<I",raw[-4:])[0] == crc32c(raw[:-4])
                events.append(("frame",hdr,raw[HEADER_SIZE:-4],good))
            else:
                if not self._buf: self._start_ms = now_ms
                self._buf.append(b)
                if len(self._buf) > COBS_RECV_MAX:
                    self._buf.clear(); self._discard = True; events.append(("overflow",))
        return events


# ---------------------------------------------------------------- TLV

T_U8, T_U16, T_U32, T_U64, T_BOOL, T_TEXT, T_BYTES = 1, 2, 3, 4, 5, 6, 7


class TlvError(ValueError):
    pass


class TlvWriter:
    def __init__(self) -> None:
        self._buf = bytearray()
        self._last = -1

    @property
    def data(self) -> bytes:
        return bytes(self._buf)

    def _put(self, tag: int, vtype: int, value: bytes) -> "TlvWriter":
        if tag <= self._last:
            raise TlvError("tags must be ascending")
        self._last = tag
        if len(value) > 65535:
            raise TlvError("value too long")
        self._buf += struct.pack("<BBH", tag, vtype, len(value)) + value
        return self

    def u8(self, tag: int, v: int) -> "TlvWriter":
        return self._put(tag, T_U8, struct.pack("<B", v))

    def u16(self, tag: int, v: int) -> "TlvWriter":
        return self._put(tag, T_U16, struct.pack("<H", v))

    def u32(self, tag: int, v: int) -> "TlvWriter":
        return self._put(tag, T_U32, struct.pack("<I", v))

    def u64(self, tag: int, v: int) -> "TlvWriter":
        return self._put(tag, T_U64, struct.pack("<Q", v))

    def boolean(self, tag: int, v: bool) -> "TlvWriter":
        return self._put(tag, T_BOOL, b"\x01" if v else b"\x00")

    def text(self, tag: int, v: str) -> "TlvWriter":
        raw = v.encode("utf-8")
        if len(raw) > 96:
            raise TlvError("text too long")
        if b"\x00" in raw:
            raise TlvError("NUL in text")
        return self._put(tag, T_TEXT, raw)

    def blob(self, tag: int, v: bytes) -> "TlvWriter":
        if len(v) > 480:
            raise TlvError("bytes too long")
        return self._put(tag, T_BYTES, v)


def _validate_utf8(raw: bytes) -> bool:
    try:
        s = raw.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return "\x00" not in s


def parse_tlv(buf: bytes, strict_scalars: bool = True, schema=None, required=()) -> dict:
    """Parse TLVs into {tag: (type, value)}.

    Value types: int for scalars/bool, str for text, bytes for bytes.
    Unknown tags with unknown types are kept as (type, raw_bytes).
    """
    out: dict = {}
    seen = set()
    pos = 0
    n = len(buf)
    while pos < n:
        if n - pos < 4:
            raise TlvError("truncated TLV header")
        tag, vtype, length = struct.unpack("<BBH", buf[pos:pos + 4])
        pos += 4
        if n - pos < length:
            raise TlvError("TLV value runs past end")
        value = buf[pos:pos + length]
        pos += length
        if tag in seen:
            raise TlvError("duplicate tag")
        seen.add(tag)
        if schema is not None:
            if tag not in schema: continue
            if vtype != schema[tag]: raise TlvError(f"wrong type for tag {tag}")
        if vtype == T_U8 and length == 1:
            out[tag] = value[0]
        elif vtype == T_U16 and length == 2:
            out[tag] = struct.unpack("<H", value)[0]
        elif vtype == T_U32 and length == 4:
            out[tag] = struct.unpack("<I", value)[0]
        elif vtype == T_U64 and length == 8:
            out[tag] = struct.unpack("<Q", value)[0]
        elif vtype == T_BOOL and length == 1 and value[0] <= 1:
            out[tag] = bool(value[0])
        elif vtype == T_TEXT and length <= 96 and _validate_utf8(value):
            out[tag] = value.decode("utf-8")
        elif vtype == T_BYTES and length <= 480:
            out[tag] = bytes(value)
        elif not strict_scalars:
            out[tag] = (vtype, bytes(value))
        else:
            raise TlvError(f"bad TLV tag {tag} type {vtype} len {length}")
    if any(tag not in out for tag in required):
        raise TlvError("missing required TLV")
    return out

OP_GET_VOICE_CAPS = 0x0302
