"""CLI pairing, recording and reconnect regression against the simulator."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import os

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(dir=root/'build') as directory:
    path=Path(directory);log=path/'trace.jsonl'
    def launch():
        return subprocess.Popen([os.environ.get('RBP_SIM_BINARY',str(root/'build/host/sim_bridge.exe')),'--data-port','45941','--control-port','45942','--quiet'],cwd=directory,stdout=subprocess.DEVNULL)
    def records():
        if not log.exists():return []
        lines=log.read_text(encoding='utf-8').splitlines();result=[]
        for line in lines:
            try:result.append(json.loads(line))
            except json.JSONDecodeError:pass
        return result
    def wait_for(predicate):
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            if predicate(records()):return
            if cli.poll() is not None:raise AssertionError('CLI exited early')
            time.sleep(.05)
        raise AssertionError('diagnostic event timeout')
    sim=launch();cli=None
    try:
        time.sleep(.3)
        cli=subprocess.Popen([sys.executable,str(root/'demo/rbp_diag.py'),'--tcp','127.0.0.1','45941','--pair','--seconds','25',
                              '--record-dir',str(path/'audio'),'--log',str(log)]+(['--compact-log'] if os.environ.get('RBP_COMPACT_LOG') else [])+(['--stop-voice-after','.2'] if os.environ.get('RBP_TIMED_STOP') else []),stdout=subprocess.DEVNULL,stdin=subprocess.PIPE)
        wait_for(lambda rows:any(r['event']=='pair_selection_required' for r in rows))
        time.sleep(.3)
        assert not any(r['event']=='request' and r.get('method')=='pair_begin' for r in records())
        cli.stdin.write(b'0\n');cli.stdin.flush()
        wait_for(lambda rows:any(r['event']=='ready' for r in rows))
        with socket.create_connection(('127.0.0.1',45942)) as control:
            time.sleep(.3);control.sendall(b'mic_on\n');time.sleep(.6);control.sendall(b'mic_off\n')
        wait_for(lambda rows:any(r['event']=='wav_saved' for r in rows))
        sim.terminate();sim.wait();time.sleep(.3);sim=launch()
        wait_for(lambda rows:sum(r['event']=='connected' for r in rows)>=2)
        rows=records()
        assert sum(r['event']=='request' and r.get('method')=='pair_begin' for r in rows)==1
        assert any(r['event']=='connection_failed' for r in rows)
        assert any(r['event']=='frame' for r in rows)
        assert any(r['event']=='write_completed' and r['bytes']>0 and r['duration_ms']>=0 for r in rows)
        assert list((path/'audio').glob('*.wav'))
        if os.environ.get('RBP_TIMED_STOP'):
            stops=[r for r in rows if r['event']=='request' and r.get('method')=='voice_stop']
            assert len(stops)==1,stops
            saved=next(r for r in rows if r['event']=='wav_saved')
            assert saved['reason']==7 and saved['complete'],saved
        if os.environ.get('RBP_COMPACT_LOG'):
            assert not any(r['event'] in ('rx','tx') for r in rows)
            chunks=[r for r in rows if r['event']=='voice_data']
            assert chunks and all('encoded_bytes' in r and 'values' not in r for r in chunks)
            manifest=json.loads(next((path/'audio').glob('*.json')).read_text())
            assert sum(r['encoded_bytes'] for r in chunks)==manifest['encoded_bytes']
            assert all('payload_size' in r and 'payload' not in r for r in rows if r['event']=='frame')
        if 'debug' in os.environ.get('RBP_SIM_BINARY',''):
            assert any(r['event']=='firmware_debug' for r in rows)
            assert any(r['event']=='request' and r.get('method')=='debug_config' for r in rows)
        print('CLI: explicit-candidate pairing, WAV, raw/frame logs, disconnect/reconnect without pairing replay passed')
    finally:
        if cli:cli.terminate();cli.wait(timeout=5)
        sim.terminate();sim.wait(timeout=5)
