"""Optional native ICO decoder; one state per stream, PCM16LE output."""
import ctypes
import os
from pathlib import Path
import struct
import sys
import threading
_GATE=threading.RLock()

_NAME='ico.dll' if os.name=='nt' else 'libico.dylib' if sys.platform=='darwin' else 'libico.so'
_PACKAGED=Path(__file__).resolve().with_name(_NAME)
_DEFAULT=_PACKAGED if _PACKAGED.exists() else Path(__file__).resolve().parents[3]/'build/ico'/_NAME
DEFAULT_LIBRARY=Path(os.environ.get('RBP_ICO_LIBRARY',str(_DEFAULT)))

class NativeIco:
    def __init__(self, path=DEFAULT_LIBRARY):
        self.lib = ctypes.CDLL(str(Path(path).resolve()))
        self.lib.ico_create.restype = ctypes.c_void_p
        self.lib.ico_destroy.argtypes = [ctypes.c_void_p]
        self.lib.ico_destroy.restype = None
        self.lib.ico_restart.argtypes = [ctypes.c_void_p]
        self.lib.ico_restart.restype = ctypes.c_int
        self.lib.ico_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        self.lib.ico_decode.restype = ctypes.c_int
        with _GATE:self.state = self.lib.ico_create()
        if not self.state:
            raise MemoryError('ico_create failed')

    def reset(self):
        with _GATE:result=self.lib.ico_restart(self.state)
        if result != 0:
            raise RuntimeError('ICO reset failed')

    def decode(self, frame):
        if len(frame) != 40:
            raise ValueError('Expected 40-byte ICO frame')
        src = ctypes.create_string_buffer(bytes(frame))
        pcm = (ctypes.c_int16 * 320)()
        with _GATE:result=self.lib.ico_decode(self.state, src, 40, pcm)
        if result != 320:
            raise RuntimeError('ICO decode failed')
        return struct.pack('<320h', *pcm)

    def close(self):
        with _GATE:
            if self.state:
                self.lib.ico_destroy(self.state)
                self.state = None


    def __del__(self):
        if getattr(self, "state", None): self.close()

def available():
    try:
        d=NativeIco();d.close();return True
    except (OSError, AttributeError):return False
