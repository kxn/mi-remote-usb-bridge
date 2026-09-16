import dataclasses
import json
from pathlib import Path
import sys
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'client/python'))
from rbp import Key,KeyDef,KeysState,InputSource,LogicalKeys,physical_layout

class LogicalInputTests(unittest.TestCase):
    def test_slots_are_not_key_codes_and_reset_releases(self):
        events=[];m=LogicalKeys(events.append)
        source=InputSource('board-a',1,7,9,'unicom.hid_ico.v1',1)
        m.install(source,[KeyDef(0,Key.MUTE,'Mute'),KeyDef(1,Key.UP,'Up')],2)
        m.feed(KeysState(1,10,1,0,0))
        self.assertEqual([e.kind for e in events],['snapshot'])
        m.feed(KeysState(2,20,2,1,0))
        self.assertEqual([(e.kind,e.key_id) for e in events[1:]],[('up',Key.MUTE),('down',Key.UP)])
        m.feed(KeysState(2,20,2,1,0));self.assertEqual(len(events),3)
        m.feed(KeysState(1,10,1,0,0));self.assertEqual(m.pressed_keys,{Key.UP})
        m.reset();self.assertEqual(events[-2].kind,'up');self.assertTrue(events[-2].synthetic)
        self.assertFalse(m.ready);self.assertFalse(m.has_key(Key.UP))

    def test_sources_unknown_ids_layout_and_invalid_catalog(self):
        a=[];b=[];ma=LogicalKeys(a.append);mb=LogicalKeys(b.append)
        source=InputSource('a',1,1,1,'future.model',1)
        for m,s in [(ma,source),(mb,dataclasses.replace(source,receiver_id='b'))]:
            m.install(s,[KeyDef(0,0x8001,'Extra')],1);m.feed(KeysState(1,0,1,1,0))
        ma.reset();self.assertEqual(mb.pressed_keys,{0x8001});self.assertNotEqual(a[0].source,b[0].source)
        self.assertIsNone(physical_layout('future.model'));self.assertIsNone(physical_layout('xiaomi.rc003'))
        self.assertIn(Key.TV,[x['key_id'] for x in physical_layout('unicom.hid_ico.v1')['buttons']])
        with self.assertRaises(ValueError):ma.install(source,[KeyDef(1,3,'Up')],1)
        with self.assertRaises(ValueError):mb.feed(KeysState(2,0,2,1,0))

    def test_enums_match_firmware(self):
        import re
        header=(ROOT/'protocol/include/rbp/defs.h').read_text()
        expected={k:int(v,16) for k,v in re.findall(r'#define RBP_KEY_(\w+) 0x([0-9a-fA-F]+)u',header)}
        self.assertEqual(expected,{k.name:k.value for k in Key})
        dotnet=(ROOT/'client/dotnet/RemoteBridge.Client/RemoteKey.cs').read_text()
        self.assertEqual(expected,{k:int(v) for k,v in re.findall(r'(\w+) = (\d+),',dotnet)})

    def test_snapshot_and_catalog_do_not_cross_connections(self):
        from rbp.client import BridgeClient,DeviceInfo,RpcError
        from rbp.wire import Header,OP_KEYS_SNAPSHOT,OP_KEY_CATALOG
        import struct
        c=BridgeClient(None);c.session_id=1;c.connection_id=2
        c._have_keys=True;c.keys_seq=10;c.pressed_bits=2
        payload=struct.pack('<IQQBBH',9,100,1,0,0,0)
        c._request=lambda *args:(Header(opcode=OP_KEYS_SNAPSHOT,connection_id=2),payload)
        self.assertEqual(c.keys_snapshot().input_seq,9)
        self.assertEqual((c.keys_seq,c.pressed_bits),(10,2))
        c._request=lambda *args:(Header(opcode=OP_KEYS_SNAPSHOT,connection_id=1),payload)
        with self.assertRaises(RpcError):c.keys_snapshot()
        c.device=DeviceInfo(connection_id=2,peer_id=1,state=5,model_id='test',catalog_revision=1,key_count=1)
        c.catalog=[KeyDef(0,Key.UP,'Up')]
        def changed(*args):
            c.connection_id=3
            return Header(opcode=OP_KEY_CATALOG,connection_id=2),b''
        c._request=changed
        with self.assertRaises(RpcError):c.key_catalog()
        self.assertEqual(c.catalog[0].key_id,Key.UP)

    def test_high_bit_and_reset_validation(self):
        events=[];m=LogicalKeys(events.append)
        m.install(InputSource('a',1,1,1,'future',1),[KeyDef(i,0x8000+i,str(i)) for i in range(64)],64)
        m.feed(KeysState(1,0,1<<63,1,0))
        self.assertEqual(events[-1].key_id,0x803f)
        with self.assertRaises(ValueError):m.feed(KeysState(2,0,1,2,0))
        m.feed(KeysState(2,0,0,2,1))
        self.assertTrue(events[-2].synthetic);self.assertEqual(events[-1].kind,'reset')

    def test_auto_catalog_simulator(self):
        from test_e2e import Sim,connect
        sim=Sim();c=connect();events=[];c.on_key_event=events.append
        try:
            c.hello();c.enable_logical_keys()
            search=c.find_start(500);c.drain(.7);_,entries=c.find_list(search)
            c.wait_operation(c.pair_begin(search,entries[0].candidate_id))
            deadline=time.monotonic()+5
            while not c.logical_keys.ready and time.monotonic()<deadline:c.drain(.05)
            self.assertTrue(c.has_key(Key.UP));self.assertFalse(c.has_key(Key.DIGIT_1))
            sim.cmd('press up');c.drain(.15);sim.cmd('release up');c.drain(.15)
            self.assertIn(('down',Key.UP),[(e.kind,e.key_id) for e in events])
            self.assertIn(('up',Key.UP),[(e.kind,e.key_id) for e in events])
            self.assertTrue(all(e.source.receiver_id==c.receiver_id for e in events))
            sim.cmd('press up');c.drain(.15);c.disconnect();c.drain(.2)
            self.assertFalse(c.logical_keys.ready)
            self.assertTrue(any(e.kind=='up' and e.synthetic and e.key_id==Key.UP for e in events))
        finally:
            c._session_broken();c.t.close();sim.close()

if __name__=='__main__':unittest.main(verbosity=2)
