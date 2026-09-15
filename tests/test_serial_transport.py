import sys,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'client/python'))
from rbp.transport import SerialTransport
class Port:
    _port_handle=123
    in_waiting=0
    def __init__(self):self.reads=[];self.partial=False
    def __setattr__(self,n,v):
        if n in ('timeout','write_timeout'):raise AssertionError('hot path reconfigures port')
        object.__setattr__(self,n,v)
    def read(self,n):self.reads.append(n);return b'x'*n
    def write(self,b):return len(b)-1 if self.partial else len(b)
class TransportTests(unittest.TestCase):
    def test_write_uses_one_configured_deadline(self):
        t=SerialTransport.__new__(SerialTransport);t.ser=Port()
        t.write(b'abc');t.ser.partial=True
        with self.assertRaises(TimeoutError):t.write(b'abc')
    @unittest.skipUnless(sys.platform=='win32','Windows COM timeout policy')
    def test_reads_do_not_reconfigure_dcb(self):
        from serial import win32
        t=SerialTransport.__new__(SerialTransport);t.ser=Port();t._read_timeout_ms=20
        policies=[]
        def get(h,p):p._obj.WriteTotalTimeoutConstant=2000;return 1
        def set_(h,p):
            self.assertEqual(p._obj.WriteTotalTimeoutConstant,2000)
            policies.append((p._obj.ReadIntervalTimeout,p._obj.ReadTotalTimeoutConstant));return 1
        with patch.object(win32,'GetCommTimeouts',get),patch.object(win32,'SetCommTimeouts',set_):
            t.read(.02);self.assertFalse(policies)
            t.read(.0051);self.assertEqual(policies[-1],(0,6))
            t.read(.0051);self.assertEqual(len(policies),1)
            t.read(0);self.assertEqual(policies[-1],(win32.MAXDWORD,0))
            t.ser.in_waiting=5000;t.read(.2)
            self.assertEqual(t.ser.reads[-1],4096);self.assertEqual(len(policies),2)
if __name__=='__main__':unittest.main()
