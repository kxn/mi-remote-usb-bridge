"""RBP/3.0 client library (independent Python implementation)."""
from .wire import (Header, FrameParser, TlvWriter, parse_tlv, crc32c,
                   cobs_encode, cobs_decode, encode_frame)
from .client import (BridgeClient, DeviceInfo, KeysState, Candidate, KeyDef,
                     EncodedFragment, AudioFormat, AudioEnd, RpcError, SessionLost)

__all__ = [
    "Header", "FrameParser", "TlvWriter", "parse_tlv", "crc32c",
    "cobs_encode", "cobs_decode", "encode_frame",
    "BridgeClient", "DeviceInfo", "KeysState", "Candidate", "KeyDef",
    "EncodedFragment", "AudioFormat", "AudioEnd", "RpcError", "SessionLost",
]

from .input import Key,KeyEvent,InputSource,LogicalKeys,model_profile,physical_layout
__all__ += ["Key","KeyEvent","InputSource","LogicalKeys","model_profile","physical_layout"]
