"""
ce_lzx.py - Windows CE ROM (LZX) compression codec wrapper.

DumpNAVI's compression is Microsoft's Windows CE `compress_lzx` codec, exposed
by `CECompressv4.dll` as `CECompress` / `CEDecompress`. There is no portable
pure-source implementation of this algorithm: even cross-platform tools (e.g.
itsme's eimgfs) run this very 32-bit DLL's machine code rather than reimplement
it. So, exactly like the original C++ tool, we load and call that DLL.

IMPORTANT — bitness: `CECompressv4.dll` is a 32-bit (i386) DLL. A 32-bit DLL can
ONLY be loaded by a 32-bit process. Therefore, to handle *compressed* entries
you must run this under **32-bit Python** on Windows. Uncompressed entries need
no codec and work under any Python on any OS.

If you have 64-bit Python and don't want to install 32-bit Python, you can still
`list` everything and `extract`/`update` all *uncompressed* entries; compressed
ones are simply skipped with a clear notice.
"""

from __future__ import annotations

import ctypes
import os
import struct
import sys
from ctypes import c_uint32, c_uint16, c_void_p

DEFAULT_STEP = 1
DEFAULT_PAGESIZE = 4096

_MACHINE_NAMES = {0x14C: "32-bit (i386)", 0x8664: "64-bit (amd64)",
                  0x1C0: "ARM", 0x1C4: "ARMv7", 0xAA64: "ARM64"}


class CodecUnavailable(RuntimeError):
    """Raised when a compressed entry needs the codec but it can't be loaded."""


def _python_bits() -> int:
    return struct.calcsize("P") * 8


def _pe_machine(path: str):
    """Return the PE 'machine' word of a DLL/EXE file, or None."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
            if head[:2] != b"MZ":
                return None
            e_lfanew = struct.unpack_from("<I", head, 0x3C)[0]
            f.seek(e_lfanew)
            sig = f.read(6)
            if sig[:4] != b"PE\x00\x00":
                return None
            return struct.unpack_from("<H", sig, 4)[0]
    except OSError:
        return None


class CeLzxCodec:
    """ctypes wrapper around CECompressv4.dll (compress_lzx)."""

    def __init__(self, dll_path: str | None = None,
                 extra_dirs: list[str] | None = None):
        self._dll = None
        self._decompress = None
        self._compress = None
        self.dll_path = None
        self._load_error: str | None = None
        self._explicit = dll_path
        self._extra_dirs = extra_dirs or []
        try:
            self._load()
        except Exception as exc:  # noqa: BLE001 - defer the failure to use-time
            self._load_error = str(exc)

    # ------------------------------------------------------------------ #
    def _search_dirs(self):
        seen = set()
        here = os.path.dirname(os.path.abspath(__file__))
        for d in (os.getcwd(), here, *self._extra_dirs,
                  os.path.join(here, "Release"), os.path.join(here, "Debug")):
            if d and d not in seen:
                seen.add(d)
                yield d

    def _candidate_paths(self):
        if self._explicit:
            yield self._explicit
        for d in self._search_dirs():
            for name in ("CECompressv4.dll", "CECompressv3.dll", "CECompress.dll"):
                yield os.path.join(d, name)

    def _load(self):
        if sys.platform != "win32":
            raise CodecUnavailable(
                "The LZX codec (CECompressv4.dll) is a Windows-only DLL, so "
                "compressed entries can't be processed on this OS. Uncompressed "
                "entries still work.")

        py_bits = _python_bits()
        found_path = None
        found_mach = None
        last_oserr = None

        for cand in self._candidate_paths():
            if not (self._explicit and cand == self._explicit) \
                    and not os.path.isfile(cand):
                continue
            mach = _pe_machine(cand)
            if found_path is None:
                found_path, found_mach = cand, mach
            # 32-bit DLL cannot load into 64-bit Python (and vice-versa).
            if mach == 0x14C and py_bits != 32:
                continue
            if mach == 0x8664 and py_bits != 64:
                continue
            try:
                # Let the DLL resolve its own neighbours, then load by abspath.
                try:
                    os.add_dll_directory(os.path.dirname(os.path.abspath(cand)))
                except (OSError, AttributeError):
                    pass
                self._dll = ctypes.CDLL(os.path.abspath(cand))
            except OSError as exc:
                last_oserr = exc
                continue
            self.dll_path = cand
            self._bind()
            return

        # Nothing loaded - build a precise, actionable diagnosis.
        raise CodecUnavailable(self._diagnose(found_path, found_mach,
                                              py_bits, last_oserr))

    def _diagnose(self, found_path, found_mach, py_bits, last_oserr) -> str:
        if found_path is None:
            dirs = ", ".join(self._search_dirs())
            return ("CECompressv4.dll not found. Put it next to dumpnavi.py "
                    "(or pass --codec-dll <path>). Searched: %s" % dirs)
        mach_name = _MACHINE_NAMES.get(found_mach, "0x%x" % (found_mach or 0))
        if found_mach == 0x14C and py_bits != 32:
            return ("Found %s but it is %s while your Python is %d-bit. A 32-bit "
                    "DLL can only be loaded by 32-bit Python. Fix: install 32-bit "
                    "Python from python.org ('Windows installer (32-bit)'), then "
                    "run:  py -3-32 dumpnavi.py <file.bin> <command>   (keep the "
                    "DLL next to the script)." % (found_path, mach_name, py_bits))
        if last_oserr is not None:
            return ("Failed to load %s (%s). It may need a runtime dependency "
                    "(e.g. an MSVC runtime)." % (found_path, last_oserr))
        return "Could not load the LZX codec (%s)." % found_path

    def _bind(self):
        dec = self._dll.CEDecompress
        dec.restype = c_uint32
        dec.argtypes = [c_void_p, c_uint32, c_void_p, c_uint32,
                        c_uint32, c_uint16, c_uint32]
        self._decompress = dec
        comp = self._dll.CECompress
        comp.restype = c_uint32
        comp.argtypes = [c_void_p, c_uint32, c_void_p, c_uint32,
                         c_uint16, c_uint32]
        self._compress = comp

    # ------------------------------------------------------------------ #
    @property
    def available(self) -> bool:
        return self._dll is not None

    @property
    def load_error(self) -> str | None:
        return self._load_error

    def _require(self):
        if not self.available:
            raise CodecUnavailable(self._load_error or "LZX codec unavailable.")

    @staticmethod
    def _as_signed(value: int) -> int:
        return value - (1 << 32) if value >= (1 << 31) else value

    def decompress(self, data: bytes, out_size: int, skip: int = 0,
                   step: int = DEFAULT_STEP,
                   pagesize: int = DEFAULT_PAGESIZE) -> bytes:
        self._require()
        src = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        dst = (ctypes.c_ubyte * max(out_size, 1))()
        ret = self._as_signed(self._decompress(
            ctypes.cast(src, c_void_p), len(data),
            ctypes.cast(dst, c_void_p), out_size, skip, step, pagesize))
        if ret < 0:
            raise RuntimeError("CEDecompress() failed")
        return bytes(dst[:ret])

    def compress(self, data: bytes, max_out: int | None = None,
                 step: int = DEFAULT_STEP,
                 pagesize: int = DEFAULT_PAGESIZE) -> bytes:
        self._require()
        if max_out is None:
            max_out = len(data) + 20
        src = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        dst = (ctypes.c_ubyte * max(max_out, 1))()
        ret = self._as_signed(self._compress(
            ctypes.cast(src, c_void_p), len(data),
            ctypes.cast(dst, c_void_p), max_out, step, pagesize))
        if ret < 0:
            raise RuntimeError("CECompress() failed")
        return bytes(dst[:ret])


def make_codec(dll_path: str | None = None,
               extra_dirs: list[str] | None = None):
    """Return the best available codec.

    1. Try to load the DLL in-process (works when running 32-bit Python, or if a
       matching-bitness DLL is present).
    2. If that fails and we're on 64-bit Windows, transparently spawn a 32-bit
       helper process (ce_bridge) that loads the real 32-bit DLL.
    3. Otherwise return the (unavailable) in-process codec, which still carries a
       helpful diagnostic message.

    The returned object always exposes .available / .load_error / .decompress /
    .compress, so callers don't care which path was taken.
    """
    direct = CeLzxCodec(dll_path, extra_dirs=extra_dirs)
    if direct.available:
        return direct
    if sys.platform == "win32" and _python_bits() == 64:
        try:
            from ce_bridge import CeBridgeCodec
            bridge = CeBridgeCodec(dll_path=dll_path, extra_dirs=extra_dirs)
            if bridge.available:
                return bridge
            # Bridge failed too: merge both reasons for a clearer message.
            direct._load_error = (
                "%s | 64-bit helper: %s"
                % (direct._load_error or "no in-process DLL", bridge.load_error))
        except Exception as exc:  # noqa: BLE001
            direct._load_error = "%s | bridge error: %s" % (
                direct._load_error or "no in-process DLL", exc)
    return direct
