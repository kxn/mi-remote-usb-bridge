"""Deterministic lifecycle interleavings; no serial device required."""
import queue
import sys
import threading
import unittest
from concurrent.futures import Future, TimeoutError
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'client/python'))
from rbp.worker import BridgeWorker

class WorkerRaces(unittest.TestCase):
    def test_submit_and_shutdown_have_one_admission_boundary(self):
        w=BridgeWorker.__new__(BridgeWorker)
        w._stop=threading.Event();w._lifecycle=threading.Lock()
        entered=threading.Event();release=threading.Event();finished=threading.Event()
        class PausedQueue(queue.Queue):
            def put_nowait(self,item):
                entered.set()
                if not release.wait(2):raise AssertionError('barrier timeout')
                super().put_nowait(item)
        w.commands=PausedQueue(2);result=[]
        submit=threading.Thread(target=lambda:result.append(w.submit('get_device')))
        def shutdown():
            w._request_stop()
            while True:
                try:f,*_=w.commands.get_nowait()
                except queue.Empty:break
                if f.set_running_or_notify_cancel():f.set_exception(ConnectionError('closed'))
            finished.set()
        submit.start();self.assertTrue(entered.wait(2))
        stop=threading.Thread(target=shutdown);stop.start()
        self.assertFalse(finished.wait(.03))
        release.set();submit.join(2);stop.join(2)
        self.assertFalse(submit.is_alive() or stop.is_alive())
        self.assertTrue(result[0].done())
        with self.assertRaises(ConnectionError):result[0].result()
        with self.assertRaises(RuntimeError):w.submit('get_device')

    def test_call_timeout_cancels_unstarted_command(self):
        w=BridgeWorker.__new__(BridgeWorker)
        f=Future();w.submit=lambda *a,**k:f
        with self.assertRaises(TimeoutError):w.call('pair_begin',timeout=0)
        self.assertTrue(f.cancelled())

    def test_cleanup_rejects_admission_even_if_transport_close_raises(self):
        w=BridgeWorker.__new__(BridgeWorker)
        w._stop=threading.Event();w._stop.set();w._lifecycle=threading.Lock()
        w.commands=queue.Queue();f=Future();w.commands.put((f,'get_device',(),{}))
        class Transport:
            def close(self):raise OSError('close failed')
        class Client:
            t=Transport()
            def goodbye(self):pass
            def _session_broken(self):pass
        w.client=Client()
        with self.assertRaises(OSError):w._run()
        with self.assertRaises(ConnectionError):f.result(0)
        with self.assertRaises(RuntimeError):w.submit('get_device')

if __name__=='__main__':unittest.main()
