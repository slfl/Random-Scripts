"""
ce_bridge.py - run the 32-bit LZX codec from a 64-bit process.

A 32-bit DLL cannot be loaded into a 64-bit process. This bridge spawns a small
32-bit Python helper (ce_worker.py) that loads the real CECompressv4.dll, and
proxies CECompress/CEDecompress calls to it over a pipe. Result: the main
program (CLI/GUI) runs as 64-bit Python while compressed entries still work
byte-for-byte, because the genuine DLL does the work in the helper.

The bridge presents the SAME interface as ce_lzx.CeLzxCodec (`.available`,
`.load_error`, `.decompress`, `.compress`), so it is a drop-in replacement.

It needs a 32-bit Python somewhere. By default it tries the Windows `py`
launcher with `-3-32`. Override with the env var DUMPNAVI_PY32 (full path to a
32-bit python.exe) if needed.
"""

from __future__ import annotations

import atexit
import os
import struct
import subprocess
import sys

_WORKER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ce_worker.py")


def _py32_candidates():
    """Yield candidate command prefixes that should launch 32-bit Python."""
    env = os.environ.get("DUMPNAVI_PY32")
    if env:
        yield [env]
    # Windows py launcher selecting a 32-bit install:
    yield ["py", "-3-32"]
    # Common explicit fallbacks (32-bit installs):
    for base in (os.environ.get("LOCALAPPDATA", ""), r"C:\\"):
        if base:
            yield [os.path.join(base, "Programs", "Python", "Python312-32",
                                "python.exe")]


class CeBridgeCodec:
    def __init__(self, dll_path: str | None = None,
                 extra_dirs: list[str] | None = None):
        self._proc: subprocess.Popen | None = None
        self.load_error: str | None = None
        self._start(dll_path, extra_dirs or [])
        if self._proc is not None:
            atexit.register(self.close)

    # ------------------------------------------------------------------ #
    def _worker_args(self, dll_path, extra_dirs):
        args = [_WORKER]
        if dll_path:
            args += ["--codec-dll", dll_path]
        for d in extra_dirs:
            args += ["--dll-dir", d]
        return args

    def _start(self, dll_path, extra_dirs):
        if not os.path.isfile(_WORKER):
            self.load_error = "ce_worker.py not found next to ce_bridge.py"
            return
        wargs = self._worker_args(dll_path, extra_dirs)
        last = None
        flags = 0
        if sys.platform == "win32":
            # Don't pop up a console window for the helper.
            flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        for prefix in _py32_candidates():
            try:
                proc = subprocess.Popen(
                    [*prefix, *wargs],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    creationflags=flags)
            except (OSError, ValueError) as exc:
                last = "spawn failed (%s): %s" % (" ".join(prefix), exc)
                continue
            self._proc = proc
            payload = self._read_frame()
            if payload is None:
                last = "worker exited during handshake (%s)" % " ".join(prefix)
                self._kill()
                continue
            if payload[:1] == b"R":
                return                       # ready
            # payload starts with b"E" -> codec error inside the (32-bit) worker
            last = payload[1:].decode("utf-8", "replace")
            self._kill()
        self.load_error = (
            "Could not start a 32-bit codec helper. Tried the `py -3-32` "
            "launcher and DUMPNAVI_PY32. Last reason: %s" % (last or "none"))

    # ------------------------------------------------------------------ #
    @property
    def available(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def _kill(self):
        if self._proc is not None:
            try:
                self._proc.kill()
            except Exception:  # noqa: BLE001
                pass
            self._proc = None

    def close(self):
        if self._proc is not None and self._proc.poll() is None:
            try:
                self._write_frame(b"Q")
                self._proc.wait(timeout=2)
            except Exception:  # noqa: BLE001
                self._kill()

    # ------------------------------------------------------------- framing --
    def _read_exact(self, n: int) -> bytes:
        out = bytearray()
        stream = self._proc.stdout
        while len(out) < n:
            chunk = stream.read(n - len(out))
            if not chunk:
                return b""
            out += chunk
        return bytes(out)

    def _read_frame(self):
        hdr = self._read_exact(4)
        if len(hdr) < 4:
            return None
        (length,) = struct.unpack("<I", hdr)
        return self._read_exact(length)

    def _write_frame(self, payload: bytes):
        self._proc.stdin.write(struct.pack("<I", len(payload)))
        self._proc.stdin.write(payload)
        self._proc.stdin.flush()

    # ------------------------------------------------------------- codec API --
    def _request(self, payload: bytes) -> bytes:
        if not self.available:
            raise RuntimeError(self.load_error or "codec helper not running")
        self._write_frame(payload)
        resp = self._read_frame()
        if resp is None:
            self._kill()
            raise RuntimeError("codec helper died")
        if resp[:1] == b"K":
            return resp[1:]
        raise RuntimeError(resp[1:].decode("utf-8", "replace"))

    def decompress(self, data: bytes, out_size: int, **_kw) -> bytes:
        return self._request(b"D" + struct.pack("<II", out_size, len(data)) + data)

    def compress(self, data: bytes, max_out: int | None = None, **_kw) -> bytes:
        if max_out is None:
            max_out = len(data) + 20
        return self._request(b"C" + struct.pack("<II", max_out, len(data)) + data)
