#!/usr/bin/env python3
"""
ce_worker.py - 32-bit helper process for the LZX codec.

This is launched (normally automatically) by ce_bridge.py running under 64-bit
Python. It must itself run under **32-bit Python**, because it loads the 32-bit
``CECompressv4.dll`` and exposes its CECompress/CEDecompress over a binary pipe.

Protocol (length-prefixed frames on stdin/stdout):
    frame = uint32 length (little-endian) + payload

  worker -> parent, handshake (first frame):
      b"R"                         codec ready
      b"E" + utf8 error            codec not available (message follows)

  parent -> worker request:
      b"D" + <u32 out_size><u32 n> + n bytes      decompress
      b"C" + <u32 max_out><u32 n> + n bytes        compress
      b"Q"                                         quit

  worker -> parent response:
      b"K" + result_bytes          ok (result is the frame payload after 'K')
      b"X" + utf8 error            failure

Run with DUMPNAVI_FAKE_CODEC=1 to use a reversible stub codec (for testing the
IPC without the Windows DLL).
"""

import os
import struct
import sys

# Ensure binary, untranslated stdio on Windows.
if sys.platform == "win32":
    import msvcrt
    msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
    msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)

_IN = sys.stdin.buffer
_OUT = sys.stdout.buffer


def read_exact(n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = _IN.read(n - len(buf))
        if not chunk:
            return b""           # EOF
        buf += chunk
    return bytes(buf)


def read_frame():
    hdr = read_exact(4)
    if len(hdr) < 4:
        return None
    (length,) = struct.unpack("<I", hdr)
    return read_exact(length)


def write_frame(payload: bytes):
    _OUT.write(struct.pack("<I", len(payload)))
    _OUT.write(payload)
    _OUT.flush()


class _FakeCodec:
    """Reversible stub for testing the bridge without the real DLL."""
    available = True
    load_error = None

    def compress(self, data, max_out=None, **kw):
        return b"Z" + data

    def decompress(self, data, out_size, **kw):
        return data[1:]          # strip the 'Z' marker


def _build_codec():
    if os.environ.get("DUMPNAVI_FAKE_CODEC") == "1":
        return _FakeCodec()
    from ce_lzx import CeLzxCodec
    dll_path = None
    extra_dirs = []
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == "--codec-dll" and i + 1 < len(argv):
            dll_path = argv[i + 1]
            i += 2
        elif argv[i] == "--dll-dir" and i + 1 < len(argv):
            extra_dirs.append(argv[i + 1])
            i += 2
        else:
            i += 1
    return CeLzxCodec(dll_path, extra_dirs=extra_dirs)


def main():
    codec = _build_codec()
    if not codec.available:
        write_frame(b"E" + (codec.load_error or "codec unavailable").encode("utf-8"))
        return
    write_frame(b"R")

    while True:
        msg = read_frame()
        if msg is None or msg[:1] == b"Q":
            break
        op = msg[:1]
        try:
            if op == b"D":
                out_size, n = struct.unpack_from("<II", msg, 1)
                data = msg[9:9 + n]
                res = codec.decompress(data, out_size)
                write_frame(b"K" + res)
            elif op == b"C":
                max_out, n = struct.unpack_from("<II", msg, 1)
                data = msg[9:9 + n]
                res = codec.compress(data, max_out)
                write_frame(b"K" + res)
            else:
                write_frame(b"X" + b"unknown op")
        except Exception as exc:  # noqa: BLE001
            write_frame(b"X" + str(exc).encode("utf-8"))


if __name__ == "__main__":
    main()
