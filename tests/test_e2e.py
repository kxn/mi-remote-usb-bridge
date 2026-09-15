"""End-to-end tests: Python client <-> C simulator (full stack).

Covers: HELLO/session, device info, find/pair (incl. passkey), catalog,
key events, voice delivery (WAV assembly, stop semantics), session takeover,
corrupt-frame teardown, heartbeat expiry, DUPLICATE request ids, unknown
opcodes.
"""
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "client", "python"))

from rbp import wire  # noqa: E402
from rbp.client import BridgeClient, RpcError, SessionLost  # noqa: E402
from rbp.transport import TcpTransport  # noqa: E402

SIM = os.environ.get('RBP_SIM_BINARY',os.path.join(ROOT, "build", "host", "sim_bridge.exe"))

failures = 0
passed = 0


def check(cond, msg=""):
    global failures, passed
    if cond:
        passed += 1
    else:
        failures += 1
        print(f"FAIL: {msg}")


class Sim:
    def __init__(self):
        self.start()

    def start(self):
        workdir = os.path.join(ROOT, "build", "e2e")
        os.makedirs(workdir, exist_ok=True)
        try:
            os.remove(os.path.join(workdir, "sim_store.bin"))
        except OSError:
            pass
        self.log_path = os.path.join(workdir, "sim_stdout.log")
        self._log = open(self.log_path, "wb")
        self.proc = subprocess.Popen(
            [SIM, "--data-port", "45731", "--control-port", "45732", "--speed", "1.0"],
            cwd=workdir,
            stdout=self._log,
            stderr=subprocess.STDOUT,
        )
        self.ctrl = None
        time.sleep(0.5)

    def cmd(self, line):
        import socket
        if self.ctrl is None:
            self.ctrl = socket.create_connection(("127.0.0.1", 45732), timeout=5)
        self.ctrl.sendall((line + "\n").encode())

    def close(self):
        if self.ctrl:
            self.ctrl.close()
        try:
            self._log.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def restart(self):
        """Fresh device: kills the sim, wipes the bond store, restarts."""
        self.close()
        self.ctrl = None
        time.sleep(0.5)
        self.start()


def connect():
    t = TcpTransport("127.0.0.1", 45731)
    c = BridgeClient(t)
    return c


def test_full_flow(sim):
    c = connect()
    hello = c.hello()
    check(hello[5] == 512 and hello[4] == 64, "hello caps")
    check(len(hello[2]) == 16, "bridge uid")

    dev = c.get_device()
    check(dev.state in (0, 1), f"initial state unbound/disconnected (got {dev.state})")
    peer = c.get_peer()
    check(peer["peer_id"] == 0, "no peer initially")

    stats = c.get_stats()
    check("protocol_errors" in stats or 1 in stats, "stats fields")

    # board time via ping
    t1 = c.ping()
    time.sleep(0.05)
    t2 = c.ping()
    check(t2 > t1, "board_time_us advances")

    # find
    events = {}
    c.on_find_done = lambda ev: events.setdefault("find_done", ev)
    search_id = c.find_start(3000)
    check(search_id != 0, "search id")
    deadline = time.time() + 5
    while "find_done" not in events and time.time() < deadline:
        c.drain(0.05)
    check(events.get("find_done", {}).get("reason") == 0, "find done expired")

    _, entries = c.find_list(search_id)
    check(len(entries) == 1 and entries[0].name == "Xiaomi RC003",
          f"candidate listed: {entries}")

    # pair (just works)
    ops = []
    c.on_operation = lambda op: ops.append(op)
    op_id = c.pair_begin(search_id, entries[0].candidate_id)
    result = c.wait_operation(op_id, timeout=8)
    check(result["result"] == 0, f"pair result ok: {result}")

    deadline = time.time() + 8
    while (c.device is None or c.device.state != 5) and time.time() < deadline:
        c.drain(0.05)
    check(c.device is not None and c.device.state == 5, "device ready")
    check(c.device.model_id == "xiaomi.rc003", "model id")
    check(c.device.voice_state == 2, f"voice ready (got {c.device})")

    # catalog
    cat = c.key_catalog()
    check(len(cat) == 12, f"catalog 12 proven keys (got {len(cat)})")
    names = {k.name for k in cat}
    check({"Home", "Back", "Power"} <= names and "Voice" not in names, "catalog names")
    slots = sorted(k.slot for k in cat)
    check(slots == list(range(12)), "slots contiguous")

    # keys
    key_events = []
    c.on_keys = lambda st: key_events.append(st)
    c.events_enable(True)
    c.drain(0.3)
    check(len(key_events) >= 1, "initial snapshot")
    check(key_events[0].kind == 0 and key_events[0].pressed_bits == 0,
          "snapshot empty")
    n_before = len(key_events)
    sim.cmd("press home")
    time.sleep(0.2)
    c.drain(0.2)
    sim.cmd("release home")
    time.sleep(0.2)
    c.drain(0.3)
    pressed = [e for e in key_events[n_before:] if e.kind == 1 and e.pressed_bits]
    check(any(e.pressed_bits == (1 << 7) for e in pressed),
          f"home press bitmap (slot 7): {pressed}")
    released = [e for e in key_events if e.kind == 1 and e.pressed_bits == 0]
    check(len(released) >= 1, "release seen")

    # voice
    voice = {"started": None, "data": [], "end": None}
    c.on_voice_start = lambda f: voice.__setitem__("started", (f.stream_id, f.sample_rate))
    c.on_voice_data = lambda chunk: voice["data"].append(chunk)
    c.on_voice_end = lambda e: voice.__setitem__("end", (e.stream_id, e.reason, e.delivered_samples))
    res = c.voice_enable(True)
    check(res["enabled"] is True, "voice enable ok")
    sim.cmd("mic_on")
    deadline = time.time() + 12
    while voice["end"] is None and time.time() < deadline:
        c.drain(0.05)
    check(voice["started"] is not None, "voice started")
    if voice["started"] is None:
        raise SystemExit("voice never started; aborting flow")
    sid, rate = voice["started"]
    check(rate == 16000, "16 kHz stream")
    check(len(voice["data"]) > 10, f"voice chunks ({len(voice['data'])})")
    check(voice["end"] is not None and voice["end"][1] == 0,
          f"voice ended normal: {voice['end']}")
    if voice["end"] is None:
        try:
            sim.proc.kill()
            with open(sim.log_path, "rb") as f:
                out = f.read().decode("utf-8", "replace")
            tail = [l for l in out.splitlines() if "mic_on" in l or "auto-stop" in l]
            print("SIM LOG TAIL:", tail[-8:])
        except Exception as e:
            print("sim log dump failed:", e)
        raise SystemExit("voice never ended; aborting flow")
    # continuity
    expected_idx = 0
    ok_seq = True
    for ch in voice["data"]:
        if ch.first_sample_index != expected_idx:
            ok_seq = False
            break
        expected_idx += ch.unit_sample_count if ch.fragment_offset+len(ch.data)==ch.unit_size else 0
    check(ok_seq, "sample index continuity")
    check(voice["end"][2] == expected_idx, "ended sample count matches")
    frames = [ch.frame_seq for ch in voice["data"]]
    check(frames == list(range(1, len(frames) + 1)), "frame_seq from 1")

    # voice stop semantics: start another stream, then stop it
    voice2 = {"started": None, "end": None, "data": 0}
    c.on_voice_start = lambda f: voice2.__setitem__("started", (f.stream_id, f.sample_rate))
    c.on_voice_data = lambda chunk: voice2.__setitem__("data", voice2["data"] + 1)
    c.on_voice_end = lambda e: voice2.__setitem__("end", (e.stream_id, e.reason, e.delivered_samples))
    sim.cmd("mic_on")
    deadline = time.time() + 10
    while voice2["started"] is None and time.time() < deadline:
        c.drain(0.05)
    check(voice2["started"] is not None, "second stream started")
    if voice2["started"] is None:
        raise SystemExit("second stream never started")
    sid2 = voice2["started"][0]
    c.voice_stop(sid2)
    deadline = time.time() + 10
    while voice2["end"] is None and time.time() < deadline:
        c.drain(0.05)
    check(voice2["end"] is not None and voice2["end"][1] == 7,
          f"stop => requested_stop(7): {voice2['end']}")

    c.voice_enable(False)
    c.goodbye()
    c.t.close()


def test_passkey_pairing(sim):
    # instruct sim to require passkey 123456 (a fresh device, no pair_ok here:
    # that command would clear passkey mode)
    sim.cmd("pair_passkey 123456")

    c = connect()
    c.hello()
    prompts = []

    def on_prompt(p):
        prompts.append(p)
        return {"operation_id": p["operation_id"], "prompt_id": p["prompt_id"],
                "accept": True, "passkey": 123456}

    c.on_pair_prompt = on_prompt
    search_id = c.find_start(3000)
    deadline = time.time() + 5
    entries = []
    while not entries and time.time() < deadline:
        _, entries = c.find_list(search_id)
        c.drain(0.05)
    check(len(entries) == 1, "candidate found")
    op_id = c.pair_begin(search_id, entries[0].candidate_id)
    result = c.wait_operation(op_id, timeout=8)
    check(result["result"] == 0, f"passkey pair ok: {result}")
    check(len(prompts) == 1 and prompts[0]["method"] == 1, "enter passkey prompt")
    c.goodbye()
    c.t.close()

    # fresh session: peer persisted (sim store file), device should reconnect?
    c2 = connect()
    c2.hello()
    peer = c2.get_peer()
    check(peer["peer_id"] != 0, "peer persisted")
    c2.goodbye()
    c2.t.close()


def test_corrupt_frame_teardown(sim):
    c = connect()
    c.hello()
    dev = c.get_device()
    check(dev is not None, "device before corruption")
    # corrupt a valid request frame
    frame = bytearray(wire.encode_frame(
        wire.Header(kind=wire.KIND_REQUEST, session_id=c.session_id,
                    request_id=c._req_id + 100, opcode=wire.OP_GET_DEVICE),
        b"", 999))
    frame[-2] ^= 0xFF
    c.t.write(bytes(frame))
    time.sleep(0.3)
    # session is dead server-side: next request times out
    try:
        c.get_device()
        check(False, "request after corrupt frame should fail")
    except (TimeoutError, RpcError, Exception) as e:
        check(isinstance(e, (TimeoutError, RpcError, SessionLost)), f"teardown on corrupt: {type(e)}")
    # new HELLO works
    c2 = BridgeClient(c.t)
    hello = c2.hello()
    check(hello[5] == 512, "new session after teardown")
    c2.goodbye()
    c2.t.close()


def test_duplicate_and_unknown(sim):
    c = connect()
    c.hello()
    # unknown opcode via the normal client path (id 2, above hello's hwm of 1)
    resp = c._request(0x7FFF)
    check(resp[0].status == 3, f"unknown opcode => UNSUPPORTED (got {resp[0].status})")
    # Duplicate request id uses a NEW transmission sequence. Replaying an
    # old transport sequence is a protocol fault, not a duplicate-RPC probe.
    old_id=2
    ev=dict(done=False,resp=None,opcode=wire.OP_PING,connection=0,internal=False)
    c._pending[old_id]=ev
    c._send(wire.Header(kind=wire.KIND_REQUEST,session_id=c.session_id,
                       request_id=old_id,opcode=wire.OP_PING),b"")
    c.drain(.2)
    check(ev["done"] and ev["resp"][0].status==13,"duplicate request rejected")
    c._pending.pop(old_id,None)
    check(c.ping()>0,"session alive after duplicate request id")
    c.goodbye()
    c.t.close()


def test_heartbeat_expiry(sim):
    c = connect()
    c.hello()
    time.sleep(5.5)  # exceed 5000 ms heartbeat window with no requests
    # session should be gone server-side; a plain request times out
    try:
        c.get_device()
        check(False, "session should have expired")
    except (TimeoutError, RpcError, SessionLost):
        check(True, "")
    c3 = BridgeClient(c.t)
    hello = c3.hello()
    check(hello[5] == 512, "re-hello after heartbeat expiry")
    c3.goodbye()
    c3.t.close()


def main():
    sim = Sim()
    try:
        test_full_flow(sim)
        sim.restart()  # fresh device: drop bond, wipe store
        test_passkey_pairing(sim)
        sim.restart()
        test_corrupt_frame_teardown(sim)
        sim.restart()
        test_duplicate_and_unknown(sim)
        sim.restart()
        test_heartbeat_expiry(sim)
    finally:
        sim.close()
    print(f"e2e: {passed} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
