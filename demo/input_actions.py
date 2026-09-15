"""Optional OS actions. Mapping keys are stable model_id/key_id, never slots."""
import ctypes
import json
import sys


class WindowsKeys:
    def __init__(self):
        if sys.platform != "win32": raise RuntimeError("Windows backend on another OS")
        u32, u16, i32, ptr = ctypes.c_uint32, ctypes.c_uint16, ctypes.c_int32, ctypes.c_size_t
        class Mouse(ctypes.Structure):
            _fields_ = [("dx",i32),("dy",i32),("data",u32),("flags",u32),("time",u32),("extra",ptr)]
        class Keyboard(ctypes.Structure):
            _fields_ = [("vk",u16),("scan",u16),("flags",u32),("time",u32),("extra",ptr)]
        class Hardware(ctypes.Structure):
            _fields_ = [("msg",u32),("low",u16),("high",u16)]
        class Union(ctypes.Union):
            _fields_ = [("mouse",Mouse),("keyboard",Keyboard),("hardware",Hardware)]
        class Input(ctypes.Structure):
            _fields_ = [("type",u32),("value",Union)]
        self.Input = Input
        self.send = ctypes.WinDLL("user32", use_last_error=True).SendInput
        self.send.argtypes = [u32,ctypes.POINTER(Input),ctypes.c_int]
        self.send.restype = u32

    def key(self, code, down):
        value = self.Input()
        value.type = 1
        value.value.keyboard.vk = code
        extended = code in (0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x2d,0x2e,0x5b,0x5c,0x6f,0xa3,0xa5)
        value.value.keyboard.flags = (0 if down else 2) | (1 if extended else 0)
        if self.send(1,ctypes.byref(value),ctypes.sizeof(value)) != 1:
            raise OSError(ctypes.get_last_error(), "SendInput failed")


class MacKeys:
    def __init__(self):
        if sys.platform != "darwin": raise RuntimeError("macOS backend on another OS")
        import Quartz
        if not Quartz.AXIsProcessTrusted():
            raise PermissionError("Grant Accessibility permission to this application in macOS Settings")
        self.q = Quartz
        self.modifiers=set()
        self.modifier_flags={54:Quartz.kCGEventFlagMaskCommand,55:Quartz.kCGEventFlagMaskCommand,
            56:Quartz.kCGEventFlagMaskShift,60:Quartz.kCGEventFlagMaskShift,
            58:Quartz.kCGEventFlagMaskAlternate,61:Quartz.kCGEventFlagMaskAlternate,
            59:Quartz.kCGEventFlagMaskControl,62:Quartz.kCGEventFlagMaskControl}

    def key(self, code, down):
        event = self.q.CGEventCreateKeyboardEvent(None, code, down)
        if event is None: raise RuntimeError("CGEventCreateKeyboardEvent failed")
        if code in self.modifier_flags:
            if down:self.modifiers.add(code)
            else:self.modifiers.discard(code)
        flags=0
        for modifier in self.modifiers:flags|=self.modifier_flags[modifier]
        self.q.CGEventSetFlags(event,flags)
        self.q.CGEventPost(self.q.kCGHIDEventTap,event)


class InputMapper:
    def __init__(self, backend, mapping, platform=None):
        self.backend = backend
        self.platform = platform or ("windows" if sys.platform == "win32" else "macos")
        self.mapping = mapping
        self.held = []

    @staticmethod
    def load(path):
        with open(path, encoding="utf-8") as f: data = json.load(f)
        if data.get("version") != 1 or not isinstance(data.get("models"),dict):
            raise ValueError("mapping requires version=1 and models object")
        for model, keys in data["models"].items():
            if not isinstance(model,str) or not isinstance(keys,dict): raise ValueError("invalid model mapping")
            for key, platforms in keys.items():
                if not 1 <= int(key) <= 65535 or not isinstance(platforms,dict): raise ValueError("invalid key_id")
                for platform,codes in platforms.items():
                    limit=255 if platform=="windows" else 127
                    if platform not in ("windows","macos") or not isinstance(codes,list) or len(codes)>8 or any(type(c) is not int or c<0 or c>limit for c in codes):
                        raise ValueError("invalid shortcut")
        return data["models"]

    def update(self, model, catalog, bits):
        desired=[]
        rules=self.mapping.get(model,{})
        for key in catalog:
            if bits & (1<<key.slot):
                for code in rules.get(str(key.key_id),{}).get(self.platform,[]):
                    if code not in desired: desired.append(code)
        try:
            for code in reversed(self.held.copy()):
                if code not in desired: self.backend.key(code,False);self.held.remove(code)
            for code in desired:
                if code not in self.held: self.backend.key(code,True);self.held.append(code)
        except Exception:
            self.release()
            raise

    def release(self):
        errors=[]
        for code in reversed(self.held.copy()):
            try: self.backend.key(code,False);self.held.remove(code)
            except Exception as exc: errors.append(exc)
        if errors: raise errors[0]
