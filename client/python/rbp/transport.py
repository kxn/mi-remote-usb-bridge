"""Transports for the RBP client: TCP (simulator/tests) and serial (CDC)."""
from __future__ import annotations

import socket
import sys


class TcpTransport:
    """Byte-stream transport against the simulator's data port."""

    def __init__(self, host: str = "127.0.0.1", port: int = 45731, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setblocking(False)
        self._buf = bytearray()

    def write(self, data: bytes) -> None:
        self.sock.settimeout(2.0)
        try: self.sock.sendall(data)
        finally: self.sock.setblocking(False)

    def read(self, timeout: float) -> bytes:
        if not self._buf:
            import select
            r, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
            if not r:
                return b""
            try:
                chunk = self.sock.recv(65536)
            except BlockingIOError:
                return b""
            if not chunk:
                raise ConnectionError("transport closed")
            self._buf += chunk
        out = bytes(self._buf)
        self._buf.clear()
        return out

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class SerialTransport:
    """CDC transport over a real COM port (pySerial).

    The port is opened 8N1 with all flow control off; RBP does not use
    DTR/RTS semantics and the bridge never resets on DTR.
    """

    def __init__(self, port: str, baudrate: int = 115200):
        import serial  # pyserial
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,  # ignored by native USB CDC
            bytesize=8,
            parity="N",
            stopbits=1,
            xonxoff=False,
            rtscts=False,
            dsrdtr=False,
            timeout=.02 if sys.platform=="win32" else 0,
            write_timeout=2.0,
            exclusive=True if sys.platform!='win32' else None,
        )
        self._read_timeout_ms=20 if sys.platform=='win32' else 0
        if sys.platform=='darwin':
            # Kernel exclusive access supplements pySerial's cooperative flock.
            import fcntl,termios
            try:fcntl.ioctl(self.ser.fileno(),termios.TIOCEXCL)
            except Exception:
                self.ser.close();raise

    def write(self, data: bytes) -> None:
        # pySerial owns the configured two-second write deadline. Changing
        # write_timeout per write reconfigures the entire Windows COM port.
        if self.ser.write(data)!=len(data):
            raise TimeoutError("serial write incomplete")

    def read(self, timeout: float) -> bytes:
        available=self.ser.in_waiting
        if available:return self.ser.read(min(4096,available))
        if sys.platform=='win32':
            import ctypes
            import math
            from serial import win32
            milliseconds=max(0,math.ceil(timeout*1000))
            if milliseconds!=self._read_timeout_ms:
                # Change only the OS wait policy, never DCB/CDC line coding.
                # pySerial's public timeout setter calls SetCommState too.
                # Its Windows read uses _port_handle and tests timeout==0;
                # the positive constructor timeout keeps ReadFile enabled.
                policy=win32.COMMTIMEOUTS()
                if not win32.GetCommTimeouts(self.ser._port_handle,ctypes.byref(policy)):
                    raise ctypes.WinError()
                policy.ReadIntervalTimeout=0 if milliseconds else win32.MAXDWORD
                policy.ReadTotalTimeoutMultiplier=0
                policy.ReadTotalTimeoutConstant=milliseconds
                if not win32.SetCommTimeouts(self.ser._port_handle,ctypes.byref(policy)):
                    raise ctypes.WinError()
                self._read_timeout_ms=milliseconds
            return self.ser.read(1)
        import select
        ready,_,_=select.select([self.ser.fileno()],[],[],max(0,timeout))
        return self.ser.read(max(1,min(4096,self.ser.in_waiting))) if ready else b""

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass
