"""Logical key state, independent of transport and physical layout.

Snapshots establish state, never invent physical presses. Releases precede
presses. Source identity includes the receiver because two boards may issue
the same numeric session/connection IDs. Unknown model-specific IDs are kept.
"""
from dataclasses import dataclass
from enum import IntEnum
from importlib.resources import files
import json

_PROFILES=json.loads(files(__package__).joinpath('input_profiles.json').read_text('utf-8'))
Key=IntEnum('Key',_PROFILES['keys'])


@dataclass(frozen=True)
class LogicalKey:
    slot:int
    key_id:int
    name:str


@dataclass(frozen=True)
class InputSource:
    receiver_id:str
    session_id:int
    peer_id:int
    connection_id:int
    model_id:str
    catalog_revision:int


@dataclass(frozen=True)
class KeyEvent:
    source:InputSource
    kind:str                 # down, up, snapshot, reset
    key_id:int=0
    captured_us:int=0
    input_seq:int=0
    synthetic:bool=False
    pressed_keys:tuple=()
    reason:str=''


def model_profile(model_id):
    """Copy of optional presentation metadata; never used for protocol selection."""
    return dict(_PROFILES['models'].get(model_id,{}))


def physical_layout(model_id):
    identity=model_profile(model_id).get('layout_id')
    return json.loads(json.dumps(_PROFILES['layouts'][identity])) if identity else None


class LogicalKeys:
    def __init__(self,emit):
        self.emit=emit;self.source=None;self.catalog=();self._bits=0;self._seq=None

    @property
    def ready(self):return self.source is not None

    @property
    def pressed_keys(self):
        return frozenset(k.key_id for k in self.catalog if self._bits&(1<<k.slot))

    def has_key(self,key):return any(k.key_id==key for k in self.catalog)

    def install(self,source,catalog,key_count):
        catalog=tuple(LogicalKey(k.slot,k.key_id,k.name) for k in catalog)
        if (not source.session_id or not source.connection_id or not source.peer_id or
            not 0<=key_count<=64 or len(catalog)!=key_count or
            [k.slot for k in catalog]!=list(range(key_count)) or
            len({k.key_id for k in catalog})!=key_count or
            any(not 0<k.key_id<=65535 for k in catalog)):
            raise ValueError('invalid logical key catalog')
        self.reset('catalog_changed')
        self.source=source;self.catalog=catalog;self._bits=0;self._seq=None

    def reset(self,reason='source_lost'):
        source=self.source;held=sorted(self.pressed_keys)
        self.source=None;self.catalog=();self._bits=0;self._seq=None
        if source:
            for key in held:self.emit(KeyEvent(source,'up',key,synthetic=True,reason=reason))
            self.emit(KeyEvent(source,'reset',synthetic=True,reason=reason))

    def feed(self,state):
        if not self.ready:return
        if state.kind not in (0,1,2) or not 0<=state.reason<=3 or not 0<=state.input_seq<=0xffffffff or (state.kind==2 and state.pressed_bits):raise ValueError("key state values")
        if state.pressed_bits<0 or state.pressed_bits>>len(self.catalog):raise ValueError('unknown key slot')
        if self._seq is not None:
            if state.input_seq<self._seq:return # snapshot RPC may finish after a newer event
            if state.input_seq==self._seq and state.kind!=2:return
        before=self.pressed_keys
        self._bits=state.pressed_bits;self._seq=state.input_seq
        after=self.pressed_keys
        if state.kind==0:
            self.emit(KeyEvent(self.source,'snapshot',captured_us=state.captured_us,
                input_seq=state.input_seq,synthetic=True,pressed_keys=tuple(sorted(after))))
            return
        for key in sorted(before-after):
            self.emit(KeyEvent(self.source,'up',key,state.captured_us,state.input_seq,state.kind==2,reason=str(state.reason)))
        for key in sorted(after-before):
            self.emit(KeyEvent(self.source,'down',key,state.captured_us,state.input_seq))
        if state.kind==2:self.emit(KeyEvent(self.source,'reset',captured_us=state.captured_us,
            input_seq=state.input_seq,synthetic=True,reason=str(state.reason)))
