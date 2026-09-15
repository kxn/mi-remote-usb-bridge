"""Device-state driven voice restoration keeps codec negotiation, not old streams."""
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'client/python'))
from rbp.client import BridgeClient
from rbp import wire

class Reconnect(unittest.TestCase):
    def test_device_voice_state_restoration(self):
        c=BridgeClient(None)
        c._accepted_codecs=((1,1),);c._audio_max_unit=512
        def state(connection, enabled):
            w=wire.TlvWriter().u32(1,connection).u32(2,7).u8(3,5 if connection else 1)
            w.text(4,'xiaomi.rc003').text(5,'Xiaomi Remote 2 Pro').u32(6,1).u8(7,13)
            w.u8(8,2 if connection else 0).u32(9,16000 if connection else 0).u8(10,97).u8(11,0)
            w.boolean(12,enabled).boolean(13,False).u32(14,0).boolean(15,False).u16(16,0)
            w.u8(18,3 if connection else 0).u32(19,120000 if connection else 0)
            c._handle_event(wire.Header(kind=wire.KIND_EVENT,opcode=wire.OP_DEVICE_STATE_EV),w.data)
        state(1,True);self.assertTrue(c.voice_enabled)
        state(0,False);self.assertFalse(c.voice_enabled)
        state(2,True);self.assertTrue(c.voice_enabled)
        self.assertEqual(c._accepted_codecs,((1,1),))
        self.assertEqual(c._audio_max_unit,512)
        self.assertIsNone(c._stream)

if __name__=='__main__':unittest.main()
