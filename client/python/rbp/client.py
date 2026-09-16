"""RBP/3.0 bridge client (independent Python implementation).

Owns the USB session state machine: HELLO, heartbeat, request/response
correlation, event decoding (device state, pairing, keys) and voice stream
assembly. Single IO owner; this class is not thread-safe. Callbacks must
queue work instead of invoking blocking client methods recursively. Use
BridgeWorker for concurrent command producers and GUI/CLI consumers.
Transport agnostic: pass any object with
  write(bytes)                    and
  read(timeout)->bytes|b""        (serial or socket adapter).
"""
from __future__ import annotations

import random
import secrets
from collections import deque
import struct
import time
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional

from . import wire
from . import schema
from .wire import Header, FrameParser, TlvWriter, parse_tlv, TlvError

REQUEST_TIMEOUT_S = 1.0


@dataclass
class DeviceInfo:
    connection_id: int = 0
    peer_id: int = 0
    state: int = 0
    model_id: str = ""
    name: str = ""
    catalog_revision: int = 0
    key_count: int = 0
    voice_state: int = 0
    sample_rate: int = 0
    battery: int = 255
    charging: int = 0
    voice_enabled: bool = False
    delivering: bool = False
    stream_id: int = 0
    waiting_idle: bool = False
    reason: int = 0
    message: str = ""
    voice_interaction: int = 0
    max_capture_ms: int = 0

    @staticmethod
    def from_tlv(t: dict) -> "DeviceInfo":
        d = DeviceInfo()
        d.connection_id = t.get(1, 0)
        d.peer_id = t.get(2, 0)
        d.state = t.get(3, 0)
        d.model_id = t.get(4, "")
        d.name = t.get(5, "")
        d.catalog_revision = t.get(6, 0)
        d.key_count = t.get(7, 0)
        d.voice_state = t.get(8, 0)
        d.sample_rate = t.get(9, 0)
        d.battery = t.get(10, 255)
        d.charging = t.get(11, 0)
        d.voice_enabled = t.get(12, False)
        d.delivering = t.get(13, False)
        d.stream_id = t.get(14, 0)
        d.waiting_idle = t.get(15, False)
        d.reason = t.get(16, 0)
        d.message = t.get(17, "")
        d.voice_interaction = t.get(18, 0)
        d.max_capture_ms = t.get(19, 0)
        return d


@dataclass
class KeysState:
    input_seq: int
    captured_us: int
    pressed_bits: int
    kind: int
    reason: int


@dataclass
class Candidate:
    candidate_id: int
    support: int
    signal: int
    name: str


@dataclass
class KeyDef:
    slot: int
    key_id: int
    name: str


from .audio_wire import AudioFormat,EncodedFragment,AudioEnd,AudioStream,AudioError
from .codecs import SUPPORTED_CODECS
from .input import LogicalKeys,InputSource,model_profile,physical_layout


class RpcError(RuntimeError):
    def __init__(self, status: int, message: str = "") -> None:
        super().__init__(f"{wire.STATUS.get(status, status)}: {message}")
        self.status = status


class SessionLost(RuntimeError):
    pass


class BridgeClient:
    """Single-owner USB session client.  NOT thread-safe; use one thread or
    wrap all calls in a lock."""

    def __init__(self, transport, *, receiver_id=None) -> None:
        self.t = transport
        self.receiver_id=receiver_id or secrets.token_hex(16)
        self.on_key_event=None
        self.logical_keys=LogicalKeys(lambda event:self.on_key_event(event) if self.on_key_event else None)
        self._logical_enabled=False;self._logical_retry_at=0
        self.parser = FrameParser()
        self.session_id = 0
        self.last_session_error = ""
        self.connection_id = 0
        self._tx_seq = 0
        self._req_id = 0
        self._pending: Dict[int, dict] = {}
        self.device: Optional[DeviceInfo] = None
        self.peer: Optional[dict] = None
        self.hello_info: Optional[dict] = None
        self.keys_seq = 0
        self._have_keys = False
        self.pressed_bits = 0
        self.catalog: List[KeyDef] = []
        self.events_enabled_flag = False
        self.voice_enabled = False
        self._last_ping_ok = time.monotonic()

        self._rx_seq = 0
        self._hello_nonce = None
        self._last_ping_sent = 0.0
        self._stream = None
        self._audio_ignored=False;self._accepted_codecs=();self._audio_max_unit=0
        self._replies = deque()
        self.on_voice_abort = None
        self.should_cancel = None
        self.on_keys: Optional[Callable[[KeysState], None]] = None
        self.on_device: Optional[Callable[[DeviceInfo], None]] = None
        self.on_voice_start: Optional[Callable[[AudioFormat], None]] = None
        self.on_voice_data: Optional[Callable[[EncodedFragment], None]] = None
        self.on_voice_end: Optional[Callable[[AudioEnd], None]] = None
        self.on_voice_format: Optional[Callable[[AudioFormat], None]] = None
        self.on_pair_prompt: Optional[Callable[[dict], Optional[dict]]] = None
        self.on_operation: Optional[Callable[[dict], None]] = None
        self.on_find_done: Optional[Callable[[dict], None]] = None
        self.on_session_lost: Optional[Callable[[], None]] = None

    # ---------------- low level ----------------

    def _send(self, hdr: Header, payload: bytes = b"") -> None:
        if self._tx_seq >= 0xfffffffe:
            self._session_broken(); raise SessionLost("sequence exhausted")
        self._tx_seq += 1
        try:
            self.t.write(wire.encode_frame(hdr, payload, self._tx_seq))
        except (OSError, ConnectionError, TimeoutError) as exc:
            self._session_broken(f"transport write: {exc}"); raise

    def _new_request(self, opcode, payload=b"", internal=False, cookie=None):
        if not self.session_id: raise SessionLost("no session")
        if len(self._pending) >= 4: raise RuntimeError("four requests already pending")
        if self._req_id >= 0xfffffffe:
            self._session_broken(); raise SessionLost("request id exhausted")
        self._req_id += 1
        rid = self._req_id
        connection=self.connection_id if 0x200<=opcode<=wire.OP_VOICE_START else 0
        ev = dict(done=False, resp=None, opcode=opcode, connection=connection,
                  internal=internal, cookie=cookie, deadline=time.monotonic()+REQUEST_TIMEOUT_S)
        self._pending[rid] = ev
        self._send(Header(kind=wire.KIND_REQUEST, session_id=self.session_id,
                          request_id=rid, opcode=opcode, connection_id=connection), payload)
        return rid, ev

    def _request(self, opcode, payload=b"", timeout=REQUEST_TIMEOUT_S):
        rid, ev = self._new_request(opcode, payload)
        deadline = time.monotonic() + timeout
        try:
            while not ev["done"] and time.monotonic() < deadline:
                self._pump(min(.05, max(.001, deadline-time.monotonic())))
            if ev.get("error"): raise SessionLost(ev["error"])
            if not ev["done"]: raise TimeoutError(f"request {opcode:#x} timed out")
            return ev["resp"]
        finally:
            self._pending.pop(rid, None)

    def _check_status(self, hdr):
        if hdr.status not in (0,1): raise RpcError(hdr.status)

    @staticmethod
    def _tlv(payload, types, required=None):
        return parse_tlv(payload, schema=types, required=types if required is None else required)

    def _handle_frame(self, hdr, payload, crc_ok):
        # During recovery, only a nonce-correlated HELLO can establish a session.
        if self._hello_nonce is not None:
            if not crc_ok or hdr.kind != wire.KIND_RESPONSE or hdr.opcode != wire.OP_HELLO or hdr.request_id != 1 or hdr.tx_seq != 1 or hdr.connection_id:
                return
            try:
                if hdr.status == 17:
                    t = self._tlv(payload, {1:6,2:5,3:7}, (3,))
                    if t[3] != self._hello_nonce: return
                elif hdr.status == 0:
                    if hdr.major != wire.MAJOR or hdr.minor != 0 or not hdr.session_id: return
                    t = self._tlv(payload, {1:7,2:7,3:6,4:1,5:2,6:3})
                    if t[1] != self._hello_nonce or len(t[2]) != 16 or not 1 <= t[4] <= 64 or t[5] != 512: return
                    self.session_id=hdr.session_id; self._rx_seq=1
                    self.last_session_error=""
                    self.hello_info=t; self._last_ping_ok=time.monotonic();self._last_ping_sent=self._last_ping_ok
                else: return
                ev=self._pending.get(1)
                if ev is not None: ev.update(done=True,resp=(hdr,payload))
                self._hello_nonce=None
            except (TlvError, ValueError, KeyError): return
            return
        if not self.session_id: return
        if not crc_ok:
            self._session_broken("frame CRC mismatch"); return
        if hdr.session_id != self.session_id: return
        try:
            if hdr.major!=wire.MAJOR or hdr.minor!=0 or hdr.flags or hdr.payload_size!=len(payload) or hdr.tx_seq!=self._rx_seq+1:
                raise ValueError("invalid header or sequence gap")
            self._rx_seq=hdr.tx_seq
            if hdr.kind == wire.KIND_RESPONSE:
                ev=self._pending.get(hdr.request_id)
                if ev is None: return  # timed-out responses never replay a side effect
                if hdr.opcode!=ev["opcode"] or hdr.connection_id!=ev["connection"] or hdr.status not in wire.STATUS:
                    raise ValueError("response correlation mismatch")
                if hdr.opcode==0x7f01 and hdr.status==0:
                    from .debug import validate_response
                    validate_response(payload)
                else:schema.validate_response(hdr.opcode,hdr.status,payload)
                if ev["internal"]:
                    if hdr.status: raise ValueError("heartbeat refused")
                    t=self._tlv(payload,{1:3,2:4})
                    if t[1]!=ev["cookie"]: raise ValueError("heartbeat cookie")
                    self._last_ping_ok=time.monotonic();self._pending.pop(hdr.request_id,None)
                else: ev.update(done=True,resp=(hdr,payload))
            elif hdr.kind in (wire.KIND_EVENT,wire.KIND_AUDIO):
                if hdr.request_id or hdr.status: raise ValueError("event header")
                self._handle_event(hdr,payload)
            else: raise ValueError("unexpected request from board")
        except (TlvError, ValueError, KeyError, IndexError, struct.error) as exc:
            self._session_broken(f"protocol opcode={hdr.opcode:#x} seq={hdr.tx_seq}: {exc}")

    def _reject_audio(self,reason):
        if self._audio_ignored:return
        sid=self._stream.stream_id if self._stream else 0
        self._stream=None;self._audio_ignored=True
        if self.on_voice_abort:self.on_voice_abort(sid,reason)
        self._replies.append({'voice_disable':True})

    def _abort_voice(self, reason):
        active=self._stream
        self._stream=None
        if active is not None and self.on_voice_abort: self.on_voice_abort(active.stream_id,reason)

    def _handle_event(self, hdr, payload):
        op=hdr.opcode
        if hdr.kind!=(wire.KIND_AUDIO if op==wire.OP_VOICE_DATA else wire.KIND_EVENT):
            raise ValueError("event kind")
        if op==wire.OP_DEVICE_STATE_EV:
            types={1:3,2:3,3:1,4:6,5:6,6:3,7:1,8:1,9:3,10:1,11:1,12:5,13:5,14:3,15:5,16:2,17:6,18:1,19:3}
            values=self._tlv(payload,types,tuple(t for t in types if t!=17));schema.validate_device(values)
            d=DeviceInfo.from_tlv(values)
            if d.state>7 or d.key_count>64 or d.voice_state>4 or d.charging>2 or d.voice_interaction>3 or (d.battery>100 and d.battery!=255): raise ValueError("device values")
            if d.connection_id!=self.connection_id:
                self._abort_voice("connection_changed");self.keys_seq=0;self._have_keys=False;self.pressed_bits=0;self.catalog=[]
                self.events_enabled_flag=False;self.voice_enabled=False
            self._logical_device(d)
            self.device=d;self.connection_id=d.connection_id
            self.voice_enabled=d.voice_enabled
            if self.on_device:self.on_device(d)
            return
        if op in (wire.OP_KEYS_STATE_EV,wire.OP_VOICE_STARTED_EV,wire.OP_VOICE_DATA,wire.OP_VOICE_ENDED_EV,wire.OP_VOICE_FORMAT_EV):
            if not self.connection_id or hdr.connection_id!=self.connection_id:
                raise ValueError("stale connection event")
        if op==wire.OP_KEYS_STATE_EV:
            st=self._keys(payload)
            if self.device and (st.pressed_bits>>self.device.key_count):raise ValueError("unknown key slot")
            if self._have_keys and st.input_seq<self.keys_seq:raise ValueError("input sequence regressed")
            if self._have_keys and st.kind!=2 and st.input_seq<=self.keys_seq: return
            self._have_keys=True;self.keys_seq=st.input_seq;self.pressed_bits=st.pressed_bits
            self.logical_keys.feed(st)
            if self.on_keys:self.on_keys(st)
        elif op in (wire.OP_VOICE_STARTED_EV,wire.OP_VOICE_FORMAT_EV,wire.OP_VOICE_DATA,wire.OP_VOICE_ENDED_EV):
            if self._audio_ignored:
                if op==wire.OP_VOICE_ENDED_EV:self._audio_ignored=False
                return
            try:
                if op in (wire.OP_VOICE_STARTED_EV,wire.OP_VOICE_FORMAT_EV):
                    f=AudioFormat.from_tlv(schema.parse(payload,schema.EVENTS[op]))
                    if op==wire.OP_VOICE_STARTED_EV:
                        if self._stream:raise AudioError('overlapping streams')
                        self._stream=AudioStream(f,self._accepted_codecs,self._audio_max_unit)
                        if self.on_voice_start:self.on_voice_start(f)
                    else:
                        if not self._stream:raise AudioError('FORMAT without START')
                        self._stream.format(f)
                        if self.on_voice_format:self.on_voice_format(f)
                elif op==wire.OP_VOICE_DATA:
                    if not self._stream:raise AudioError('DATA without START')
                    c=EncodedFragment.parse(payload);self._stream.feed(c)
                    if self.on_voice_data:self.on_voice_data(c)
                else:
                    e=AudioEnd(*(schema.parse(payload,schema.EVENTS[op])[i] for i in range(1,8)))
                    if not self._stream:raise AudioError('END without START')
                    self._stream.end(e);self._stream=None
                    if self.on_voice_end:self.on_voice_end(e)
            except AudioError as exc:self._reject_audio(str(exc))
        elif op==wire.OP_PAIR_PROMPT_EV:
            t=self._tlv(payload,{1:3,2:3,3:1,4:3,5:3},(1,2,3,5))
            if not t[1] or not t[2] or t[3]>3 or (4 in t and t[4]>999999) or (t[3] in (2,3) and 4 not in t):raise ValueError("pair prompt")
            prompt=dict(operation_id=t[1],prompt_id=t[2],method=t[3],remaining_ms=t[5])
            if 4 in t:prompt["number"]=t[4]
            if self.on_pair_prompt:
                reply=self.on_pair_prompt(prompt)
                if reply:self._replies.append(reply) # enqueue, never reenter parser from callback
        elif op==wire.OP_OPERATION_EV:
            t=self._tlv(payload,{1:3,2:1,3:2,4:3,5:3,6:5},(1,2,3))
            if self.on_operation:self.on_operation(dict(operation_id=t[1],state=t[2],result=t[3],peer_id=t.get(4,0),uncertain=t.get(6,False)))
        elif op==wire.OP_FIND_DONE_EV:
            t=self._tlv(payload,{1:3,2:1})
            if self.on_find_done:self.on_find_done(dict(search_id=t[1],reason=t[2]))

    @staticmethod
    def _keys(payload):
        if len(payload)!=24:raise ValueError("key payload length")
        seq,captured,bits,kind,reason,reserved=struct.unpack("<IQQBBH",payload)
        if reserved or kind>2 or reason>3 or (kind==2 and bits):raise ValueError("key payload values")
        return KeysState(seq,captured,bits,kind,reason)

    def _heartbeat(self):
        if not self.session_id or self._hello_nonce is not None:return
        now=time.monotonic()
        if now-self._last_ping_ok>=5:
            self._session_broken("heartbeat timeout: no valid PING response for 5 seconds");return
        # Keep the outstanding cookie until the session deadline. A delayed
        # response is still valid; deleting it at the ordinary RPC deadline
        # would turn recoverable transport latency into a false session loss.
        if now-self._last_ping_sent>=1 and len(self._pending)<4 and not any(e.get("internal") for e in self._pending.values()):
            cookie=secrets.randbits(32);self._last_ping_sent=now
            rid,ev=self._new_request(wire.OP_PING,TlvWriter().u32(1,cookie).data,True,cookie)
            ev["deadline"]=self._last_ping_ok+5

    def _pump(self, timeout=.05):
        if self.should_cancel and self.should_cancel():
            self._session_broken();raise SessionLost("IO cancelled")
        try:data=self.t.read(timeout)
        except (OSError,ConnectionError,TimeoutError) as exc:
            self._session_broken(f"transport read: {exc}");raise
        for event in self.parser.feed(bytes(data),int(time.monotonic()*1000)):
            if event[0]=="frame":self._handle_frame(event[1],event[2],event[3])
            elif self._hello_nonce is None and self.session_id:self._session_broken(f"frame parser: {event[0]}")
        if self._stream:
            try:self._stream.check_timeout()
            except AudioError as exc:self._reject_audio(str(exc))
        self._heartbeat()

    def _session_broken(self, reason="session reset"):
        self.logical_keys.reset("session_lost")
        had_session=bool(self.session_id)
        if had_session:self.last_session_error=reason
        self.session_id=0;self.connection_id=0;self._rx_seq=0
        self.keys_seq=0;self._have_keys=False;self.pressed_bits=0;self.catalog=[]
        self.voice_enabled=False;self.events_enabled_flag=False;self.device=None;self.peer=None
        self._abort_voice("session_lost");self._replies.clear();self._audio_ignored=False;self._accepted_codecs=();self._audio_max_unit=0
        for ev in self._pending.values():ev.update(done=True,error="session lost")
        self._pending.clear()
        if had_session and self.on_session_lost:self.on_session_lost()

    def drain(self,duration=.1):
        deadline=time.monotonic()+duration
        while time.monotonic()<deadline:
            self._pump(min(.02,max(.001,deadline-time.monotonic())))
            if self._stream:
                try:self._stream.check_timeout()
                except AudioError as exc:self._reject_audio(str(exc))
            if self._logical_enabled and self.session_id:self._service_logical_keys()
            if self._replies and self.session_id:
                r=self._replies.popleft()
                if r.get("voice_disable"):self.voice_enable(False)
                else:self.pair_reply(r["operation_id"],r["prompt_id"],r.get("accept",True),r.get("passkey"))

    def hello(self):
        self._session_broken();self.parser.reset()
        self._hello_nonce=secrets.token_bytes(16)
        self._tx_seq=0;self._req_id=1
        ev=dict(done=False,resp=None);self._pending[1]=ev
        try:
            self.t.write(b"\x00")
            self._send(Header(kind=wire.KIND_REQUEST,session_id=0,request_id=1,opcode=wire.OP_HELLO),TlvWriter().blob(1,self._hello_nonce).data)
            deadline=time.monotonic()+2
            while not ev["done"] and time.monotonic()<deadline:self._pump(.05)
            if not ev["done"]:raise TimeoutError("HELLO timed out")
            if ev.get("error"):raise SessionLost(ev["error"])
            hdr,payload=ev["resp"]
            if hdr.status:raise RpcError(hdr.status,"HELLO refused")
            return self.hello_info
        except Exception:
            self._session_broken("HELLO failed");self.parser.reset()
            raise
        finally:
            self._pending.pop(1,None);self._hello_nonce=None

    def ping(self):
        cookie=secrets.randbits(32)
        hdr,payload=self._request(wire.OP_PING,TlvWriter().u32(1,cookie).data,timeout=1)
        self._check_status(hdr);t=self._tlv(payload,{1:3,2:4})
        if t[1]!=cookie:self._session_broken();raise SessionLost("ping cookie mismatch")
        self._last_ping_ok=time.monotonic();return t[2]

    # ---------------- queries ----------------

    def get_device(self) -> DeviceInfo:
        hdr, payload = self._request(wire.OP_GET_DEVICE)
        self._check_status(hdr)
        d = DeviceInfo.from_tlv(schema.parse(payload,schema.DEVICE))
        if d.connection_id!=self.connection_id:
            self._abort_voice("connection_changed");self.keys_seq=0;self._have_keys=False;self.pressed_bits=0;self.catalog=[]
            self.events_enabled_flag=False;self.voice_enabled=False
        self._logical_device(d)
        self.device = d
        self.connection_id = d.connection_id
        return d

    def get_peer(self) -> dict:
        hdr, payload = self._request(wire.OP_GET_PEER)
        self._check_status(hdr)
        t = schema.validate_response(hdr.opcode,hdr.status,payload)
        self.peer = {"peer_id": t.get(1, 0), "name": t.get(2, ""),
                     "auto_reconnect": t.get(3, False)}
        return self.peer

    def get_stats(self) -> dict:
        hdr, payload = self._request(wire.OP_GET_STATS)
        self._check_status(hdr)
        return schema.validate_response(hdr.opcode,hdr.status,payload)

    def debug_config(self, modules=255, level=3):
        """Optional debug firmware only; release firmware returns unsupported."""
        from .debug import CONFIG,validate_response
        h,p=self._request(CONFIG,TlvWriter().u32(1,modules).u8(2,level).data)
        self._check_status(h);return validate_response(p)

    # ---------------- find & pair ----------------

    def find_start(self, duration_ms: int = 5000) -> int:
        w = TlvWriter().u16(1, duration_ms)
        hdr, payload = self._request(wire.OP_FIND_START, w.data)
        if hdr.status != 0:
            raise RpcError(hdr.status)
        return schema.validate_response(hdr.opcode,hdr.status,payload)[1]

    def find_list(self, search_id: int, cursor: int = 0):
        w = TlvWriter().u32(1, search_id).u8(2, cursor)
        hdr, payload = self._request(wire.OP_FIND_LIST, w.data)
        self._check_status(hdr)
        t = schema.validate_response(hdr.opcode,hdr.status,payload)
        next_cursor = t[1]
        blob = t[2]
        entries: List[Candidate] = []
        pos = 0
        if not blob or blob[0]>8:raise ValueError("candidate count")
        count = blob[pos]
        pos += 1
        for _ in range(count):
            if len(blob)-pos<7:raise ValueError("candidate entry truncated")
            rid = struct.unpack("<I", blob[pos:pos + 4])[0]
            support, signal, name_len = blob[pos + 4], blob[pos + 5], blob[pos + 6]
            if name_len>48 or pos+7+name_len>len(blob) or not rid or support>2:raise ValueError("candidate entry")
            name = blob[pos + 7:pos + 7 + name_len].decode("utf-8")
            pos += 7 + name_len
            entries.append(Candidate(rid, support, signal, name))
        if pos!=len(blob):raise ValueError("candidate trailing bytes")
        return next_cursor, entries

    def pair_begin(self, search_id: int, candidate_id: int) -> int:
        w = TlvWriter().u32(1, search_id).u32(2, candidate_id)
        hdr, payload = self._request(wire.OP_PAIR_BEGIN, w.data)
        if hdr.status == 1:  # ACCEPTED, TLV 1 = operation_id u32
            return self._tlv(payload,{1:3})[1]
        raise RpcError(hdr.status)

    def pair_reply(self, operation_id: int, prompt_id: int, accept: bool,
                   passkey: Optional[int] = None) -> None:
        w = TlvWriter().u32(1, operation_id).u32(2, prompt_id).boolean(3, accept)
        if passkey is not None:
            w.u32(4, passkey)
        hdr, _ = self._request(wire.OP_PAIR_REPLY, w.data)
        self._check_status(hdr)

    def pair_cancel(self, operation_id: int) -> None:
        w = TlvWriter().u32(1, operation_id)
        hdr, _ = self._request(wire.OP_PAIR_CANCEL, w.data)
        self._check_status(hdr)

    def forget_peer(self, peer_id: int) -> None:
        w = TlvWriter().u32(1, peer_id)
        hdr, payload = self._request(wire.OP_FORGET_PEER, w.data)
        if hdr.status != 1:
            raise RpcError(hdr.status)
        return self._tlv(payload,{1:3})[1]

    def get_operation(self, operation_id: int) -> dict:
        w = TlvWriter().u32(1, operation_id)
        hdr, payload = self._request(wire.OP_GET_OPERATION, w.data)
        self._check_status(hdr)
        t = schema.validate_response(hdr.opcode,hdr.status,payload)
        return {"operation_id": t[1], "state": t[2], "result": t[3],
                "peer_id": t.get(4, 0), "uncertain": t.get(6, False)}

    def wait_operation(self, operation_id: int, timeout: float = 10.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            op = self.get_operation(operation_id)
            if op["state"] == 1:
                return op
            self.drain(0.1)
        raise TimeoutError("operation pending")

    # ---------------- keys ----------------

    def _input_source(self,d):
        return InputSource(self.receiver_id,self.session_id,d.peer_id,d.connection_id,d.model_id,d.catalog_revision)

    def _logical_device(self,d):
        if self.logical_keys.ready and (d.state!=5 or self.logical_keys.source!=self._input_source(d)):
            self.logical_keys.reset("device_changed")

    def enable_logical_keys(self,enable=True):
        """Opt-in automatic catalog/snapshot/subscription. Call drain() or use BridgeWorker.

        on_key_event receives KeyEvent; on_keys remains the raw slot API.
        Callbacks must enqueue work, never perform blocking RPCs recursively.
        """
        self._logical_enabled=bool(enable);self._logical_retry_at=0
        if not enable:
            self.logical_keys.reset("input_disabled");return
        self.get_device();self._service_logical_keys()

    def _service_logical_keys(self):
        if not self._logical_enabled or time.monotonic()<self._logical_retry_at:return
        d=self.device
        if d is None or d.state!=5 or not d.connection_id:return
        source=self._input_source(d)
        if self.logical_keys.source==source:return
        try:
            catalog=self.key_catalog()
            if self.device is None or self.device.state!=5 or self._input_source(self.device)!=source:return
            self.logical_keys.install(source,catalog,d.key_count)
            self.events_enable(True)
            if self.logical_keys.source!=source:return
            self.logical_keys.feed(self.keys_snapshot())
        except (RpcError,TimeoutError):
            self.logical_keys.reset("catalog_unavailable");self._logical_retry_at=time.monotonic()+1
        except ValueError as exc:
            self._session_broken(f"logical input: {exc}");raise

    def has_key(self,key):return self.logical_keys.has_key(key)

    def input_info(self):
        d=self.device
        return dict(source=self.logical_keys.source,ready=self.logical_keys.ready,
            keys=tuple(self.logical_keys.catalog),pressed_keys=self.logical_keys.pressed_keys,
            profile=model_profile(d.model_id) if d else {},layout=physical_layout(d.model_id) if d else None)

    def key_catalog(self) -> List[KeyDef]:
        result = []
        marker=lambda:(self.session_id,self.connection_id,self.device.model_id if self.device else None,self.device.catalog_revision if self.device else None)
        identity=marker()
        cursor = 0
        revision=None
        seen_slots=set();seen_ids=set()
        while True:
            w = TlvWriter()
            if cursor:
                w.u8(1, cursor)
            hdr, payload = self._request(wire.OP_KEY_CATALOG, w.data)
            self._check_status(hdr)
            if identity!=marker():raise RpcError(7,"catalog connection changed")
            t = schema.validate_response(hdr.opcode,hdr.status,payload)
            if revision is not None and revision!=t[1]:raise ValueError("catalog changed during pagination")
            revision=t[1]
            blob = t[3]
            pos = 0
            if not blob or blob[0]>64:raise ValueError("catalog count")
            count = blob[pos]
            pos += 1
            for _ in range(count):
                if len(blob)-pos<4:raise ValueError("catalog entry truncated")
                slot = blob[pos]
                key_id = struct.unpack("<H", blob[pos + 1:pos + 3])[0]
                name_len = blob[pos + 3]
                if slot>=64 or slot in seen_slots or key_id in seen_ids or not key_id or name_len>48 or pos+4+name_len>len(blob):raise ValueError("catalog entry")
                seen_slots.add(slot);seen_ids.add(key_id)
                name = blob[pos + 4:pos + 4 + name_len].decode("utf-8")
                pos += 4 + name_len
                result.append(KeyDef(slot, key_id, name))
            if pos!=len(blob):raise ValueError("catalog trailing bytes")
            if t[2] == 255:
                break
            if not cursor<t[2]<64:raise ValueError("catalog cursor did not advance")
            cursor = t[2]
        if [k.slot for k in result]!=list(range(len(result))):raise ValueError("non-contiguous catalog")
        if self.device and self.device.state==5 and (revision!=self.device.catalog_revision or len(result)!=self.device.key_count):raise ValueError("catalog disagrees with device")
        self.catalog=result
        return list(result)

    def events_enable(self, enable: bool) -> None:
        w = TlvWriter().boolean(1, enable)
        hdr, _ = self._request(wire.OP_EVENTS_ENABLE, w.data)
        self._check_status(hdr)
        self.events_enabled_flag = enable

    # ---------------- voice ----------------

    def voice_caps(self):
        h,p=self._request(wire.OP_GET_VOICE_CAPS);self._check_status(h)
        t=schema.validate_response(h.opcode,h.status,p)
        if len(t[1])%6 or len(t[1])>96 or t[3]!=256:raise AudioError('invalid capabilities')
        codecs=tuple(struct.iter_unpack('<IH',t[1]))
        if tuple(sorted(set(codecs)))!=codecs or any(not 0<i<0x80000000 or not r for i,r in codecs) or (codecs and not 1<=t[2]<=65536) or (not codecs and t[2]):raise AudioError('invalid capabilities')
        return dict(codecs=codecs,max_unit_bytes=t[2],config_max_bytes=t[3])

    def voice_enable(self,enable:bool,accepted_codecs=None,max_unit_bytes=65536)->dict:
        w=TlvWriter().boolean(1,enable)
        old=(self._accepted_codecs,self._audio_max_unit)
        if enable:
            supported=tuple(accepted_codecs if accepted_codecs is not None else SUPPORTED_CODECS)
            caps=self.voice_caps();accepted=tuple(sorted(set(supported)&set(caps['codecs'])))
            if not accepted or caps['max_unit_bytes']>max_unit_bytes:raise AudioError('unsupported_format')
            w.blob(2,b''.join(struct.pack('<IH',*c) for c in accepted)).u32(3,max_unit_bytes)
            self._accepted_codecs=accepted;self._audio_max_unit=max_unit_bytes
        try:
            h,p=self._request(wire.OP_VOICE_ENABLE,w.data);self._check_status(h)
        except Exception:
            self._accepted_codecs,self._audio_max_unit=old;raise
        t=schema.validate_response(h.opcode,h.status,p);self.voice_enabled=enable
        if not enable:self._accepted_codecs=();self._audio_max_unit=0
        else:self._audio_ignored=False
        return dict(enabled=t[1],waiting_idle=t[2])

    def voice_start(self) -> None:
        # Request one capture; OK does not promise audio arrival.
        hdr, payload = self._request(wire.OP_VOICE_START, b'')
        self._check_status(hdr)
        schema.validate_response(hdr.opcode, hdr.status, payload)

    def voice_stop(self, stream_id: int) -> None:
        w = TlvWriter().u32(1, stream_id)
        hdr, _ = self._request(wire.OP_VOICE_STOP, w.data)
        self._check_status(hdr)

    def goodbye(self) -> None:
        if not self.session_id:
            return
        try:
            self._request(wire.OP_GOODBYE, b"", timeout=1.0)
        except (TimeoutError, SessionLost, RpcError):
            pass
        self._session_broken()

    def find_stop(self, search_id):
        h,_=self._request(wire.OP_FIND_STOP,TlvWriter().u32(1,search_id).data);self._check_status(h)

    def keys_snapshot(self):
        h,p=self._request(wire.OP_KEYS_SNAPSHOT);self._check_status(h)
        if h.connection_id!=self.connection_id:raise RpcError(7,"snapshot connection changed")
        st=self._keys(p)
        if not self._have_keys or st.input_seq>=self.keys_seq:
            self._have_keys=True;self.keys_seq=st.input_seq;self.pressed_bits=st.pressed_bits
        return st

    def set_reconnect(self, peer_id, enabled):
        h,_=self._request(wire.OP_SET_RECONNECT,TlvWriter().u32(1,peer_id).boolean(2,enabled).data);self._check_status(h)

    def connect_peer(self, peer_id=None):
        if peer_id is None:peer_id=self.get_peer()["peer_id"]
        h,_=self._request(wire.OP_CONNECT_PEER,TlvWriter().u32(1,peer_id).data);self._check_status(h)

    def disconnect(self):
        h,_=self._request(wire.OP_DISCONNECT);self._check_status(h)
