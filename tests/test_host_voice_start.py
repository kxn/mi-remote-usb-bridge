import json, subprocess, sys, tempfile, time, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'client/python')]
from rbp.worker import BridgeWorker
from rbp.transport import TcpTransport

class HostVoiceStart(unittest.TestCase):
    def test_cli_single_capture_does_not_restart(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as tmp:
            directory=Path(tmp)
            sim=subprocess.Popen([str(ROOT/'build/host/sim_bridge.exe'),'--data-port','45861','--control-port','45862','--quiet'],
                cwd=tmp,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
            worker=None
            try:
                time.sleep(.4)
                worker=BridgeWorker(TcpTransport('127.0.0.1',45861))
                worker.call('hello');search=worker.call('find_start',100);time.sleep(.2)
                _,entries=worker.call('find_list',search)
                operation=worker.call('pair_begin',search,entries[0].candidate_id)
                self.assertEqual(worker.call('wait_operation',operation)['result'],0)
                worker.close();worker=None
                log=directory/'diag.jsonl'
                run=subprocess.run([sys.executable,str(ROOT/'demo/rbp_diag.py'),'--tcp','127.0.0.1','45861',
                    '--no-connect','--start-voice',
                    '--stop-voice-after','1','--seconds','7','--record-dir',str(directory/'audio'),
                    '--log',str(log),'--compact-log'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=20)
                self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
                events=[json.loads(l) for l in log.read_text(encoding='utf8').splitlines()]
                (ROOT/'build/logs/host-start-e2e.jsonl').write_text(log.read_text(encoding='utf8'),encoding='utf8')
                starts=[e for e in events if e['event']=='voice_start']
                ends=[e for e in events if e['event']=='voice_end']
                requests=[e for e in events if e['event']=='request' and e['method']=='voice_start']
                self.assertEqual(len(requests),1,events[-8:])
                self.assertEqual(len(starts),1,events[-8:]);self.assertEqual(len(ends),1)
                captures=list((directory/'audio').glob('*.json'))
                self.assertEqual(len(captures),1)
                for p in captures:
                    capture=json.loads(p.read_text(encoding='utf8'))
                    self.assertTrue(capture['complete']);self.assertEqual(capture['reason'],7)
                    self.assertGreater(capture['samples'],0)
                    self.assertEqual(capture['samples'],capture['declared_samples'])
                self.assertFalse(any(e['event']=='request' and e['method']=='connect_peer' for e in events))
            finally:
                if worker:worker.close()
                sim.terminate();sim.wait(timeout=5)
if __name__=='__main__':unittest.main()
