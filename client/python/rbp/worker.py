"""Optional application helper: one IO owner, bounded events and commands.

BridgeClient and the wire protocol remain independent of this helper and Qt.
UI callbacks never run on this thread; the UI consumes next_event().
"""
import queue
import threading
from concurrent.futures import Future

from .client import BridgeClient


class BridgeWorker:
    def __init__(self, transport):
        self.client = BridgeClient(transport)
        self.events = queue.Queue(maxsize=512)
        self.commands = queue.Queue(maxsize=32)
        self._stop = threading.Event()
        self._lifecycle = threading.Lock()
        self.client.should_cancel=self._stop.is_set
        self._overflow = False
        self._dropping_audio = False
        for name in ("keys", "device", "voice_start", "voice_data", "voice_end",
                     "voice_format", "voice_abort", "pair_prompt", "operation", "find_done",
                     "session_lost"):
            setattr(self.client, "on_" + name, self._callback(name))
        self.thread = threading.Thread(target=self._run, name="rbp-io", daemon=True)
        self.thread.start()

    def _callback(self, name):
        def deliver(*args):
            if name == "session_lost" and not args:
                args = (self.client.last_session_error or "session reset",)
            # Reserve space for keys/control; a slow audio consumer must not
            # take down the link or strand an OS key in the pressed state.
            if name.startswith("voice_") and name != "voice_abort":
                if self._dropping_audio:
                    if name == "voice_start": self._dropping_audio = False
                    else: return
                if self.events.qsize() >= max(1, self.events.maxsize - 32):
                    self._dropping_audio = True
                    self.client._reject_audio("local audio consumer overrun")
                    return
            try:
                self.events.put_nowait((name, args))
            except queue.Full:
                self._overflow = True
        return deliver

    def submit(self, method, *args, **kwargs):
        # Admission and shutdown share one boundary: shutdown cannot finish
        # draining the queue between the closed check and enqueue.
        with self._lifecycle:
            if self._stop.is_set():
                raise RuntimeError("bridge worker closed")
            future = Future()
            self.commands.put_nowait((future, method, args, kwargs))
            return future

    def call(self, method, *args, timeout=8, **kwargs):
        future=self.submit(method, *args, **kwargs)
        try:return future.result(timeout)
        except TimeoutError:
            # Prevent a queued timed-out command executing later. A running
            # command retains its actual outcome; cancellation cannot undo IO.
            future.cancel()
            raise

    def next_event(self, timeout=0.1):
        return self.events.get(timeout=timeout)

    def _run(self):
        try:
            while not self._stop.is_set():
                try:
                    future, method, args, kwargs = self.commands.get_nowait()
                except queue.Empty:
                    future = None
                if future is not None and future.set_running_or_notify_cancel():
                    try:
                        if method.startswith("_"):
                            raise ValueError("private client method")
                        future.set_result(getattr(self.client, method)(*args, **kwargs))
                    except Exception as exc:
                        future.set_exception(exc)
                try:
                    self.client.drain(.02)
                except Exception as exc:
                    self.client._session_broken()
                    self._callback("error")(str(exc))
                    # A removed serial port must be reopened by its owner.
                    self._request_stop()
                if self._overflow:
                    self._overflow = False
                    while True:
                        try: self.events.get_nowait()
                        except queue.Empty: break
                    try:
                        if self.client.session_id:
                            self.client.voice_enable(False)
                            self.client.goodbye()
                    except Exception:
                        pass
                    self.client._session_broken()
                    self._callback("session_lost")()
                    self._callback("error")("local event queue overrun; input and recording reset")
        finally:
            self._request_stop()
            self.client.should_cancel=None
            try:
                try:self.client.goodbye()
                except Exception:pass
                self.client._session_broken()
            finally:
                try:self.client.t.close()
                finally:
                    while True:
                        try:future, _, _, _ = self.commands.get_nowait()
                        except queue.Empty:break
                        if future.set_running_or_notify_cancel():
                            future.set_exception(ConnectionError("worker closed"))

    def _request_stop(self):
        with self._lifecycle:self._stop.set()

    def close(self):
        self._request_stop()
        self.thread.join(timeout=6)
        if self.thread.is_alive():
            raise TimeoutError("IO worker did not stop")
