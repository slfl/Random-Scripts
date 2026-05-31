#!/usr/bin/env python3
"""
dumpnavi.py - Python port of DumpNAVI (Bysin.cpp) v1.4

A faithful reimplementation of the C++ tool used to inspect and modify the
Windows CE XIP ROM images (``B000FF`` format) found on Acura/Honda navigation
DVDs (e.g. ``09Touch2.bin``, ``09Touch.bin``).

Commands (identical to the original):
    list                          - list modules and files in the .bin
    extract [names...]            - extract all, or only the named entries
    update  <name> [infile]       - replace a *file* entry inside the .bin
    updateModule <name> [infile]  - replace a *module* (EXE/DLL) inside the .bin

Compression note
----------------
The original loaded ``CECompressv4.dll`` (Microsoft's ``compress_lzx`` codec)
to (de)compress entries that carry the COMPRESS attribute.  This port reuses
that exact DLL through ctypes (see ce_lzx.py) so compressed entries round-trip
byte-for-byte.  On hosts where the DLL is unavailable (e.g. Linux), every
*uncompressed* entry still lists/extracts/updates perfectly; only compressed
entries need the codec, and a clear message is shown if it is missing.

Original authors: bysin, guicide, ryebrye, DogP; structs from Willem Jan
Hengeveld (itsme).  Licensed GPLv2, as the original.

Python port & GUI: SLFL
"""

from __future__ import annotations

import argparse
import io
import os
import struct
import sys

from ce_lzx import CeLzxCodec, CodecUnavailable, make_codec

VERSION = "1.4-py"

# --------------------------------------------------------------------------- #
# Struct sizes / formats (Win32 layout, 32-bit pointers) - verified to match
# the C++ definitions byte-for-byte.
# --------------------------------------------------------------------------- #
ROMHDR_FMT = "<17I2H3I"
ROMHDR_SIZE = struct.calcsize(ROMHDR_FMT)          # 84
BLOCKHDR_FMT = "<3I"
BLOCKHDR_SIZE = struct.calcsize(BLOCKHDR_FMT)      # 12
MODULEHDR_FMT = "<8I"
MODULEHDR_SIZE = struct.calcsize(MODULEHDR_FMT)    # 32
FILEHDR_FMT = "<7I"
FILEHDR_SIZE = struct.calcsize(FILEHDR_FMT)        # 28
O32ROM_FMT = "<6I"
O32ROM_SIZE = struct.calcsize(O32ROM_FMT)          # 24
E32ROM_SIZE = 108                                  # 106 + 2 trailing pad
O32OBJ_SIZE = 40
E32EXE_SIZE = 248
DOSHDR_SIZE = 64

# File / module attribute flags
FILEATTR_COMPRESS_MODULE = 4096
FILEATTR_COMPRESS = 2048
FILEATTR_HIDDEN = 4
FILEATTR_READONLY = 2
FILEATTR_SYSTEM = 1

# Section flags
IMAGE_FILE_RELOCS_STRIPPED = 0x0001
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080
SECTION_COMPRESSED = 0x2000

IMAGE_DOS_SIGNATURE = 0x5A4D

# Commands
COMMAND_LIST = 1
COMMAND_EXTRACT = 2
COMMAND_UPDATE = 3
COMMAND_UPDATE_MODULE = 4

# e32 directory unit indices (only the ones the original uses)
EXP, IMP, RES, EXC, SEC, FIX, DEB, IMD, MSP = range(9)
RS4 = 14
STD_EXTRA = 16

# Segment naming, as in the C++ (note: these counters are *not* reset per
# module in the original - it uses globals - so we keep them per-run too).
SEG_TEXT, SEG_DATA, SEG_PDATA, SEG_RSRC, SEG_OTHER = range(5)
SEG_NAMES = [b".text", b".data", b".pdata", b".rsrc", b".other"]

# The exact DOS stub bytes the original embeds.
DOSCODE = bytes([
    0x0e, 0x1f, 0xba, 0x0e, 0x00, 0xb4, 0x09, 0xcd, 0x21, 0xb8, 0x01, 0x4c,
    0xcd, 0x21, 0x54, 0x68, 0x69, 0x73, 0x20, 0x70, 0x72, 0x6f, 0x67, 0x72,
    0x61, 0x6d, 0x20, 0x63, 0x61, 0x6e, 0x6e, 0x6f, 0x74, 0x20, 0x62, 0x65,
    0x20, 0x72, 0x75, 0x6e, 0x20, 0x69, 0x6e, 0x20, 0x44, 0x4f, 0x53, 0x20,
    0x6d, 0x6f, 0x64, 0x65, 0x2e, 0x0d, 0x0d, 0x0a, 0x24, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
])

U32_MASK = 0xFFFFFFFF


def u32(v: int) -> int:
    return v & U32_MASK


class ImageError(Exception):
    """Raised when the .bin can't be parsed or a needed region is missing."""


class Entry:
    """A cached directory entry (one module or one file). Used by the GUI."""
    __slots__ = ("kind", "index", "name", "attr", "size", "size2",
                 "offset", "romaddr", "compressed")

    def __init__(self, kind, index, name, attr, size, size2, offset, romaddr,
                 compressed):
        self.kind = kind          # "module" or "file"
        self.index = index
        self.name = name
        self.attr = attr
        self.size = size          # uncompressed size
        self.size2 = size2        # stored (possibly compressed) size; None for modules
        self.offset = offset      # ROM data offset (files); None for modules
        self.romaddr = romaddr
        self.compressed = compressed

    @property
    def flags(self) -> str:
        if self.kind == "module":
            c = "C" if self.attr & FILEATTR_COMPRESS_MODULE else "_"
        else:
            c = "C" if self.attr & FILEATTR_COMPRESS else "_"
        return (c
                + ("H" if self.attr & FILEATTR_HIDDEN else "_")
                + ("R" if self.attr & FILEATTR_READONLY else "_")
                + ("S" if self.attr & FILEATTR_SYSTEM else "_"))


# --------------------------------------------------------------------------- #
# Header containers
# --------------------------------------------------------------------------- #
class RomHdr:
    __slots__ = ("nummods", "numfiles", "raw")

    @classmethod
    def unpack(cls, data: bytes) -> "RomHdr":
        v = struct.unpack(ROMHDR_FMT, data[:ROMHDR_SIZE])
        self = cls()
        self.raw = v
        self.nummods = v[4]
        self.numfiles = v[12]
        return self


class ModuleHdr:
    __slots__ = ("attr", "time", "time2", "size", "fileaddr",
                 "e32offset", "o32offset", "offset", "name")

    @classmethod
    def unpack(cls, data: bytes) -> "ModuleHdr":
        (attr, t, t2, size, fileaddr, e32o, o32o, off) = struct.unpack(
            MODULEHDR_FMT, data[:MODULEHDR_SIZE])
        self = cls()
        self.attr, self.time, self.time2, self.size = attr, t, t2, size
        self.fileaddr, self.e32offset, self.o32offset, self.offset = (
            fileaddr, e32o, o32o, off)
        self.name = None
        return self


class FileHdr:
    __slots__ = ("attr", "time", "time2", "size", "size2",
                 "fileaddr", "offset", "name", "cap")

    @classmethod
    def unpack(cls, data: bytes) -> "FileHdr":
        (attr, t, t2, size, size2, fileaddr, off) = struct.unpack(
            FILEHDR_FMT, data[:FILEHDR_SIZE])
        self = cls()
        self.attr, self.time, self.time2 = attr, t, t2
        self.size, self.size2, self.fileaddr, self.offset = (
            size, size2, fileaddr, off)
        self.name = None
        self.cap = size2          # original reserved capacity (set at scan)
        return self


def parse_e32_rom(data: bytes) -> dict:
    """Parse the ROM e32 header (108 bytes)."""
    (objcnt, imageflags, entryrva, vbase, subsysmajor, subsysminor,
     stackmax, vsize, sect14rva, sect14size) = struct.unpack_from(
        "<HHIIHHIIII", data, 0)
    units = list(struct.iter_unpack("<II", data[32:32 + 9 * 8]))
    (subsys,) = struct.unpack_from("<H", data, 104)
    return {
        "e32_objcnt": objcnt, "e32_imageflags": imageflags,
        "e32_entryrva": entryrva, "e32_vbase": vbase,
        "e32_subsysmajor": subsysmajor, "e32_subsysminor": subsysminor,
        "e32_stackmax": stackmax, "e32_vsize": vsize,
        "e32_sect14rva": sect14rva, "e32_sect14size": sect14size,
        "e32_unit": units, "e32_subsys": subsys,
    }


def parse_o32_rom(data: bytes) -> dict:
    (vsize, rva, psize, dataptr, realaddr, flags) = struct.unpack(
        O32ROM_FMT, data[:O32ROM_SIZE])
    return {"o32_vsize": vsize, "o32_rva": rva, "o32_psize": psize,
            "o32_dataptr": dataptr, "o32_realaddr": realaddr,
            "o32_flags": flags}


def parse_pe_sections(data: bytes):
    """Minimal PE parser: returns (num_sections, [section dicts])."""
    if len(data) < 0x40 or data[0:2] != b"MZ":
        raise ValueError("input is not a PE/MZ file")
    (e_lfanew,) = struct.unpack_from("<I", data, 0x3C)
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("PE signature not found")
    coff = e_lfanew + 4
    (num_sections,) = struct.unpack_from("<H", data, coff + 2)
    (opt_size,) = struct.unpack_from("<H", data, coff + 16)
    sect_off = coff + 20 + opt_size
    sections = []
    for s in range(num_sections):
        base = sect_off + 40 * s
        name = data[base:base + 8]
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<4I", data, base + 8)
        (flags,) = struct.unpack_from("<I", data, base + 36)
        sections.append({"Name": name, "VirtualSize": vsize,
                         "VirtualAddress": vaddr, "SizeOfRawData": rawsize,
                         "PointerToRawData": rawptr, "flags": flags})
    return num_sections, sections


# --------------------------------------------------------------------------- #
# The core image - virtual-memory engine + parsing + commands
# --------------------------------------------------------------------------- #
class BinImage:
    def __init__(self, path: str, codec: CeLzxCodec | None = None,
                 outdir: str | None = None, data: bytes | None = None,
                 writable: bool = True):
        self.path = path
        # `data` not None  -> work on an in-memory copy (GUI: nothing is written
        # to disk until save()).  Otherwise open the real file (read-only unless
        # writable, so read-only .bin files can still be inspected/extracted).
        self.in_memory = data is not None
        if self.in_memory:
            self.f = io.BytesIO(data)
        else:
            self.f = open(path, "r+b" if writable else "rb")
        self.codec = codec or make_codec()
        # virtual-memory state (mirrors the C++ globals)
        self.blockstart = 0          # file offset where the block list begins
        self.blockstartpos = 0       # file offset of the current/last block hdr
        self.virtualpos = 0
        self.blocklen = 0
        self.addroffs = 0            # ROMOFFSET
        # parsed headers
        self.imageaddr = 0
        self.imagelen = 0
        self.romhdraddr = 0
        self.romhdr: RomHdr | None = None
        self.modules: list[ModuleHdr] = []
        self.files: list[FileHdr] = []
        self.entries: list[Entry] = []     # cached directory (GUI)
        # output directory name (derived from the bin filename, as in main())
        self.binfile = outdir if outdir is not None else self._derive_outdir(path)
        # per-run segment name counters (global in the original)
        self._seg_usage = [0, 0, 0, 0, 0]
        # entries skipped because the LZX codec was unavailable
        self.skipped: list[str] = []
        # True once a staged replacement has modified the in-memory buffer
        self.dirty = False

    def _skip(self, name: str, why: str):
        self.skipped.append(name)
        print("  -> skipped %s (%s)" % (name, why))

    # --- context manager -------------------------------------------------- #
    def close(self):
        try:
            self.f.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @staticmethod
    def _derive_outdir(path: str) -> str:
        base = os.path.basename(path)
        name = base.split(".", 1)[0]
        if name == base:          # no '.' in the name
            name = name + "-"
        return name

    # --- low-level "virtual memory" over the B000FF block list ------------ #
    def virtual_seek(self, addr: int) -> int:
        """Map a virtual address to a file position; sets blocklen/virtualpos.

        NOTE: faithfully mirrors the original, including that ROMOFFSET is
        re-added on each call (harmless for the navi image where ROMOFFSET=0).
        """
        addr = u32(addr + self.addroffs)
        self.f.seek(self.blockstart)
        while True:
            pos = self.f.tell()
            self.blockstartpos = pos
            hdr = self.f.read(BLOCKHDR_SIZE)
            if len(hdr) < BLOCKHDR_SIZE:
                break                       # EOF
            b_addr, b_len, _b_chk = struct.unpack(BLOCKHDR_FMT, hdr)
            if b_addr == 0:
                break                       # end-of-blocks marker
            if b_addr <= addr < b_addr + b_len:
                a = addr - b_addr
                self.f.seek(a, io.SEEK_CUR)
                self.blocklen = b_len - a
                self.virtualpos = addr
                return self.blocklen
            self.f.seek(b_len, io.SEEK_CUR)
        self.blocklen = 0
        self.blockstartpos = 0
        self.virtualpos = 0
        return 0

    def virtual_read(self, size: int) -> bytes:
        """Read `size` bytes across block boundaries (or raw if virtualpos==0)."""
        if not self.virtualpos:
            return self.f.read(size)
        if self.blocklen >= size:
            self.blocklen -= size
            self.virtualpos += size
            return self.f.read(size)
        out = bytearray()
        remaining = size
        while remaining:
            if self.blocklen:
                a = self.blocklen if self.blocklen < remaining else remaining
                chunk = self.f.read(a)
                if not chunk:
                    return bytes(out)
                out += chunk
                remaining -= a
                self.virtualpos += a
            if not self.virtual_seek(self.virtualpos):
                break
        return bytes(out)

    def virtual_calc_sum(self) -> int:
        """Recompute the checksum of the block we just wrote into."""
        savepos = self.f.tell()
        self.f.seek(self.blockstartpos)
        hdr = self.f.read(BLOCKHDR_SIZE)
        b_addr, b_len, _ = struct.unpack(BLOCKHDR_FMT, hdr)
        buf = self.f.read(b_len)
        chksum = u32(sum(buf))
        self.f.seek(self.blockstartpos)
        self.f.write(struct.pack(BLOCKHDR_FMT, b_addr, b_len, chksum))
        self.f.seek(savepos)
        return 1

    def virtual_write(self, data: bytes) -> int:
        size = len(data)
        if not self.virtualpos:
            n = self.f.write(data)
            self.virtual_calc_sum()
            return n
        if self.blocklen >= size:
            self.blocklen -= size
            self.virtualpos += size
            n = self.f.write(data)
            self.virtual_calc_sum()
            return n
        pos = 0
        remaining = size
        while remaining:
            if self.blocklen:
                a = self.blocklen if self.blocklen < remaining else remaining
                self.f.write(data[pos:pos + a])
                self.virtual_calc_sum()
                remaining -= a
                self.virtualpos += a
                pos += a
            if not self.virtual_seek(self.virtualpos):
                break
        return pos

    def update_file_size(self, file_index: int, size: int, size2: int,
                         attr: int) -> int:
        off = (self.romhdraddr + ROMHDR_SIZE
               + MODULEHDR_SIZE * self.romhdr.nummods
               + FILEHDR_SIZE * file_index)
        if not self.virtual_seek(off):
            print("Unable to read block file")
            return 0
        p = self.f.tell()
        fh = list(struct.unpack(FILEHDR_FMT, self.virtual_read(FILEHDR_SIZE)))
        fh[0] = attr      # attr
        fh[3] = size      # size
        fh[4] = size2     # size2
        self.f.seek(p)
        self.virtual_write(struct.pack(FILEHDR_FMT, *fh))
        return 1

    # --- header readers --------------------------------------------------- #
    def read_header(self) -> int:
        buf = self.virtual_read(7)
        if buf[:7] != b"B000FF\n":
            return 0
        self.imageaddr = struct.unpack("<I", self.virtual_read(4))[0]
        self.imagelen = struct.unpack("<I", self.virtual_read(4))[0]
        self.blockstart = self.f.tell()
        return 1

    def read_ecec(self) -> int:
        if not self.virtual_seek(self.imageaddr + 0x40):
            return 0
        buf = self.virtual_read(4)
        if buf[:4] != b"ECEC":
            return 0
        self.romhdraddr = struct.unpack("<I", self.virtual_read(4))[0]
        return 1

    def read_romhdr(self) -> int:
        if not self.virtual_seek(self.romhdraddr):
            return 0
        self.romhdr = RomHdr.unpack(self.virtual_read(ROMHDR_SIZE))
        return 1

    def load(self) -> "BinImage":
        """Parse the container headers. Raises ImageError on failure."""
        if not self.read_header():
            raise ImageError("Invalid XIP file")
        if not self.read_ecec():
            raise ImageError("Invalid ECEC header")
        if not self.read_romhdr():
            self.addroffs = u32(-0x07FCE000)   # WinCE-image ROMOFFSET fallback
            if not self.read_romhdr():
                raise ImageError("Invalid ROM header")
        return self

    # --- library API (used by the GUI) ----------------------------------- #
    def scan_entries(self) -> list[Entry]:
        """Resolve the full module+file directory WITHOUT extracting data.

        This is the 'cache on open' step: only names/sizes/offsets are read;
        nothing is decompressed until get_entry_bytes() is called.
        """
        entries: list[Entry] = []

        if not self.virtual_seek(self.romhdraddr + ROMHDR_SIZE):
            raise ImageError("Unable to read module table")
        self.modules = [ModuleHdr.unpack(self.virtual_read(MODULEHDR_SIZE))
                        for _ in range(self.romhdr.nummods)]
        for i, m in enumerate(self.modules):
            m.name = self._read_name(m.fileaddr)
            if m.name is None:
                raise ImageError("Unable to read module name")
            entries.append(Entry("module", i, m.name, m.attr, m.size, None,
                                  None, m.offset,
                                  bool(m.attr & FILEATTR_COMPRESS_MODULE)))

        base = (self.romhdraddr + ROMHDR_SIZE
                + MODULEHDR_SIZE * self.romhdr.nummods)
        if not self.virtual_seek(base):
            raise ImageError("Unable to read file table")
        self.files = [FileHdr.unpack(self.virtual_read(FILEHDR_SIZE))
                      for _ in range(self.romhdr.numfiles)]
        for i, fh in enumerate(self.files):
            fh.name = self._read_name(fh.fileaddr)
            if fh.name is None:
                raise ImageError("Unable to read file name")
            entries.append(Entry("file", i, fh.name, fh.attr, fh.size,
                                  fh.size2, fh.offset, fh.offset,
                                  bool(fh.attr & FILEATTR_COMPRESS)))

        self.entries = entries
        return entries

    def get_entry_bytes(self, entry: Entry) -> bytes:
        """Return the (decompressed/reconstructed) content of ONE entry.

        Decompression happens here, on demand - only for the entry requested.
        Raises CodecUnavailable/RuntimeError if a compressed entry needs the
        codec and it isn't loaded, or ImageError on a structural problem.
        """
        if entry.kind == "module":
            return self._build_module_bytes(entry.index, self.modules[entry.index])
        fh = self.files[entry.index]
        if not self.virtual_seek(fh.offset):
            raise ImageError("Unable to read block file")
        buf = self.virtual_read(fh.size2)
        if fh.attr & FILEATTR_COMPRESS:
            return self.codec.decompress(buf, fh.size)
        return buf

    def replace_file(self, index: int, data: bytes) -> tuple[bool, str]:
        """Replace a FILE entry in the in-memory buffer. Returns (ok, message).

        Mirrors the original `update` logic (compress only if the raw data
        won't fit), but returns status instead of printing, and never writes
        to disk - the change lives in the working buffer until save()."""
        fh = self.files[index]
        attr = fh.attr
        slot = fh.cap            # ORIGINAL reserved bytes (physical room; safe ceiling)
        prev_size = fh.size
        st_size = len(data)
        if not self.virtual_seek(fh.offset):
            return False, "Unable to seek to file data"

        if not (attr & FILEATTR_COMPRESS):
            if st_size > slot:
                attr |= FILEATTR_COMPRESS         # too big raw -> must compress
            else:
                self.virtual_write(data)
                self.update_file_size(index, st_size, st_size, attr)
                self.dirty = True
                fh.size, fh.size2, fh.attr = st_size, st_size, attr
                return True, ("Stored %d bytes uncompressed (slot %d, %d free)."
                              % (st_size, slot, slot - st_size))

        # compressed path
        try:
            out = self.codec.compress(data, st_size + 20)
        except CodecUnavailable as e:
            return False, "Compression needs the LZX codec: %s" % e
        except RuntimeError:
            return False, "CECompress() failed."
        if len(out) > slot:
            return (False,
                    "Won't fit: new data is %d bytes and compresses to %d, but "
                    "the reserved slot is only %d bytes. The COMPRESSED form must "
                    "fit the original slot (the .bin can't safely grow); overflow "
                    "by %d bytes." % (st_size, len(out), slot, len(out) - slot))
        self.virtual_write(out)
        self.update_file_size(index, st_size, len(out), attr)
        self.dirty = True
        fh.size, fh.size2, fh.attr = st_size, len(out), attr
        grew = ("  Logical size %d -> %d." % (prev_size, st_size)
                if st_size != prev_size else "")
        return True, ("Stored %d bytes, compressed to %d (slot %d, %d free).%s"
                      % (st_size, len(out), slot, slot - len(out), grew))

    def slot_capacity(self, index: int) -> int:
        """Original reserved bytes (max stored size) for a file entry."""
        return self.files[index].cap

    def file_fit(self, index: int, data: bytes) -> dict:
        """Dry-run: would `data` fit file #index? Computes the stored size
        (compressing if needed) WITHOUT modifying anything.

        Returns a dict: {mode, logical, stored, slot, fits, free, overflow,
        error}. `stored`/`fits` are None if compression was needed but the
        codec is unavailable."""
        fh = self.files[index]
        slot = fh.cap
        st = len(data)
        res = {"mode": None, "logical": st, "stored": None, "slot": slot,
               "fits": None, "free": None, "overflow": None, "error": None}

        if not (fh.attr & FILEATTR_COMPRESS) and st <= slot:
            res.update(mode="uncompressed", stored=st, fits=True,
                       free=slot - st, overflow=0)
            return res
        # would be stored compressed
        try:
            out = self.codec.compress(data, st + 20)
        except CodecUnavailable as e:
            res.update(mode="compressed", error=str(e))
            return res
        except RuntimeError:
            res.update(mode="compressed", error="CECompress() failed")
            return res
        stored = len(out)
        fits = stored <= slot
        res.update(mode="compressed", stored=stored, fits=fits,
                   free=max(slot - stored, 0),
                   overflow=max(stored - slot, 0))
        return res

    def replace_module(self, index: int, data: bytes) -> tuple[bool, str]:
        """Replace a MODULE (EXE/DLL) in the in-memory buffer. (ok, message)."""
        m = self.modules[index]
        if not self.virtual_seek(m.e32offset):
            return False, "Unable to locate e32offset"
        e32 = parse_e32_rom(self.virtual_read(E32ROM_SIZE))
        objcnt = e32["e32_objcnt"]
        if not self.virtual_seek(m.o32offset):
            return False, "Unable to locate o32offset"
        o32 = [parse_o32_rom(self.virtual_read(O32ROM_SIZE))
               for _ in range(objcnt)]
        try:
            num_sections, sections = parse_pe_sections(data)
        except ValueError as e:
            return False, "Not a valid PE/EXE: %s" % e
        if num_sections != objcnt:
            return (False, "Section count mismatch: new module has %d, ROM "
                    "module has %d." % (num_sections, objcnt))

        headersize = (DOSHDR_SIZE + len(DOSCODE) + E32EXE_SIZE
                      + O32OBJ_SIZE * objcnt)
        if headersize % 0x200:
            headersize += 0x200 - (headersize % 0x200)

        pos = headersize
        for s in range(num_sections):
            sec_size = sections[s]["SizeOfRawData"]
            section_data = data[pos:pos + sec_size]
            pos += sec_size
            if o32[s]["o32_flags"] & SECTION_COMPRESSED:
                try:
                    comp = self.codec.compress(section_data, sec_size + 20)
                except CodecUnavailable as e:
                    return False, "Compression needs the LZX codec: %s" % e
                except RuntimeError:
                    return False, "CECompress() failed."
                if len(comp) > o32[s]["o32_psize"]:
                    return (False, "Section %d too big: %d > %d bytes."
                            % (s, len(comp), o32[s]["o32_psize"]))
                section_data = comp
            if not self.virtual_seek(o32[s]["o32_dataptr"]):
                return False, "Unable to seek to section data"
            self.virtual_write(section_data)
        self.dirty = True
        return True, "Module updated (%d sections)." % num_sections

    def save(self, path: str | None = None) -> str:
        """Flush the in-memory buffer to disk. Returns the path written."""
        if not self.in_memory:
            self.f.flush()
            return self.path
        target = path or self.path
        with open(target, "wb") as out:
            out.write(self.f.getvalue())
        self.dirty = False
        return target

    # --- structural inspection ------------------------------------------- #
    def iter_blocks(self):
        """Yield (hdr_pos, addr, length, chksum, data_pos) for every B000FF
        block (including the terminator with addr==0). Moves the file pointer."""
        self.f.seek(self.blockstart)
        while True:
            pos = self.f.tell()
            hdr = self.f.read(BLOCKHDR_SIZE)
            if len(hdr) < BLOCKHDR_SIZE:
                return
            a, l, c = struct.unpack(BLOCKHDR_FMT, hdr)
            data_pos = self.f.tell()
            yield (pos, a, l, c, data_pos)
            if a == 0:
                return
            self.f.seek(l, io.SEEK_CUR)

    def verify_checksums(self, limit: int | None = None) -> tuple[int, int]:
        """Return (checked, mismatched) over real blocks (optionally capped)."""
        # Materialise the block list first: reading block payloads below moves
        # the file pointer, which would otherwise desync the live generator.
        blocks = [b for b in self.iter_blocks() if b[1] != 0]
        checked = mismatched = 0
        for (_pos, _a, l, c, dp) in blocks:
            self.f.seek(dp)
            if (sum(self.f.read(l)) & U32_MASK) != c:
                mismatched += 1
            checked += 1
            if limit and checked >= limit:
                break
        return checked, mismatched

    def image_info(self) -> dict:
        """A structured summary of the image (header, ROM header, blocks,
        entries, compression). Used by the `info` command and the GUI."""
        if not self.entries:
            self.scan_entries()
        names = ("dllfirst", "dlllast", "physfirst", "physlast", "nummods",
                 "ulRAMStart", "ulRAMFree", "ulRAMEnd", "ulCopyEntries",
                 "ulCopyOffset", "ulProfileLen", "ulProfileOffset", "numfiles",
                 "ulKernelFlags", "ulFSRamPercent", "ulDrivglobStart",
                 "ulDrivglobLen", "usCPUType", "usMiscFlags", "pExtensions",
                 "ulTrackingStart", "ulTrackingLen")
        romhdr = dict(zip(names, self.romhdr.raw))

        blocks = list(self.iter_blocks())
        real = [(a, l) for (_p, a, l, _c, _d) in blocks if a != 0]
        payload = sum(l for _a, l in real)
        va_lo = min((a for a, _ in real), default=0)
        va_hi = max((a + l for a, l in real), default=0)

        mods = [e for e in self.entries if e.kind == "module"]
        files = [e for e in self.entries if e.kind == "file"]
        cfiles = [e for e in files if e.compressed]

        try:
            file_size = self.f.seek(0, io.SEEK_END)
        except Exception:  # noqa: BLE001
            file_size = None

        return {
            "path": self.path,
            "file_size": file_size,
            "imageaddr": self.imageaddr,
            "imagelen": self.imagelen,
            "romhdraddr": self.romhdraddr,
            "romoffset": u32(self.addroffs),
            "romhdr": romhdr,
            "image_span": romhdr["physlast"] - romhdr["physfirst"],
            "rom_to_ram_gap": romhdr["ulRAMStart"] - romhdr["physlast"],
            "num_blocks": len(blocks),
            "block_payload": payload,
            "va_range": (va_lo, va_hi),
            "num_modules": len(mods),
            "num_files": len(files),
            "num_compressed": len(cfiles),
            "num_uncompressed": len(files) - len(cfiles),
        }

    def format_info(self) -> str:
        """Human-readable version of image_info()."""
        i = self.image_info()
        rh = i["romhdr"]
        L = []
        a = L.append
        a("File:        %s" % os.path.basename(i["path"] or "?"))
        if i["file_size"] is not None:
            a("File size:   %d bytes (0x%x)" % (i["file_size"], i["file_size"]))
        a("Image addr:  0x%08x   length 0x%08x (%d bytes)"
          % (i["imageaddr"], i["imagelen"], i["imagelen"]))
        a("ROM header:  0x%08x   ROMOFFSET 0x%08x"
          % (i["romhdraddr"], i["romoffset"]))
        a("Phys range:  0x%08x .. 0x%08x  (span %d bytes)"
          % (rh["physfirst"], rh["physlast"], i["image_span"]))
        a("RAM:         start 0x%08x  free 0x%08x  end 0x%08x"
          % (rh["ulRAMStart"], rh["ulRAMFree"], rh["ulRAMEnd"]))
        a("ROM->RAM gap: %d bytes  (free address space right after the image)"
          % i["rom_to_ram_gap"])
        a("CPU type:    0x%04x   FSRamPercent 0x%x"
          % (rh["usCPUType"], rh["ulFSRamPercent"]))
        a("Blocks:      %d   payload %d bytes   VA 0x%08x..0x%08x"
          % (i["num_blocks"], i["block_payload"], i["va_range"][0],
             i["va_range"][1]))
        a("Modules:     %d   Files: %d (compressed %d / uncompressed %d)"
          % (i["num_modules"], i["num_files"], i["num_compressed"],
             i["num_uncompressed"]))
        return "\n".join(L)

    def _read_name(self, addr: int) -> str | None:
        if not self.virtual_seek(addr):
            return None
        # original uses raw getc() after the seek; the name lives in one block.
        chars = bytearray()
        while len(chars) < 1024:
            c = self.f.read(1)
            if not c or c == b"\x00":
                break
            chars += c
        return chars.decode("latin-1")

    # --- modules ---------------------------------------------------------- #
    def read_modules(self, command: int, names: list[str]) -> int:
        if not self.virtual_seek(self.romhdraddr + ROMHDR_SIZE):
            print("Unable to read block file")
            return 0
        self.modules = [ModuleHdr.unpack(self.virtual_read(MODULEHDR_SIZE))
                        for _ in range(self.romhdr.nummods)]

        for i, m in enumerate(self.modules):
            name = self._read_name(m.fileaddr)
            if name is None:
                print("Unable to read block file")
                return 0
            m.name = name

            if command == COMMAND_LIST:
                self._print_module(m)
            elif command == COMMAND_EXTRACT:
                if names and m.name not in names:
                    continue
                if not self._extract_module(i, m):
                    return 0
            elif command == COMMAND_UPDATE_MODULE:
                if not names or names[0] != m.name:
                    continue
                if not self._update_module(i, m, names):
                    return 0
        return 1

    def _print_module(self, m: ModuleHdr):
        flags = (("C" if m.attr & FILEATTR_COMPRESS_MODULE else "_")
                 + ("H" if m.attr & FILEATTR_HIDDEN else "_")
                 + ("R" if m.attr & FILEATTR_READONLY else "_")
                 + ("S" if m.attr & FILEATTR_SYSTEM else "_"))
        print("%s%10d%10s%22s (ROM 0x%08x)" % (flags, m.size, "", m.name,
                                               m.offset))

    def _seg_name(self, segtype: int) -> bytes:
        usage = self._seg_usage[segtype]
        if usage:
            nm = SEG_NAMES[segtype] + str(usage).encode()
        else:
            nm = SEG_NAMES[segtype]
        self._seg_usage[segtype] += 1
        return nm[:8]

    def _build_module_bytes(self, i: int, m: ModuleHdr) -> bytes:
        """Rebuild a PE/EXE from the ROM module and return it as bytes.

        Raises ImageError on a structural problem, or CodecUnavailable/
        RuntimeError if a compressed section needs the (missing) codec.
        """
        if not self.virtual_seek(m.e32offset):
            raise ImageError("Unable to locate e32offset")
        e32 = parse_e32_rom(self.virtual_read(E32ROM_SIZE))
        objcnt = e32["e32_objcnt"]

        if not self.virtual_seek(m.o32offset):
            raise ImageError("Unable to locate o32offset")
        o32 = [parse_o32_rom(self.virtual_read(O32ROM_SIZE))
               for _ in range(objcnt)]

        r = io.BytesIO()

        # DOS header (64 bytes) with the specific fields the original sets.
        dos = bytearray(DOSHDR_SIZE)
        struct.pack_into("<H", dos, 0x00, IMAGE_DOS_SIGNATURE)  # e_magic
        struct.pack_into("<H", dos, 0x02, 0x90)                 # e_cblp
        struct.pack_into("<H", dos, 0x04, 3)                    # e_cp
        struct.pack_into("<H", dos, 0x08, 0x4)                  # e_cparhdr
        struct.pack_into("<H", dos, 0x0C, 0xffff)               # e_maxalloc
        struct.pack_into("<H", dos, 0x10, 0xb8)                 # e_sp
        struct.pack_into("<H", dos, 0x18, 0x40)                 # e_lfarlc
        struct.pack_into("<i", dos, 0x3C, 0xc0)                 # e_lfanew
        r.write(dos)
        r.write(DOSCODE)
        r.seek(0x40, io.SEEK_CUR)        # gap -> zero filled by BytesIO
        newe32off = r.tell()             # == 0xc0

        # Build the PE (e32_exe) header. The codesize/database loops reproduce
        # the original's behaviour, which (in the C source) indexes o32hdr[i]
        # by the *module* index i; we guard against i>=objcnt.
        oi = o32[i] if i < objcnt else None

        def sum_vsize(flag):
            if oi is None:
                return 0
            s = 0
            for _ in range(objcnt):
                if oi["o32_flags"] & flag:
                    s += oi["o32_vsize"]
            return u32(s)

        def first_vsize(flag):
            if oi is None:
                return 0
            for _ in range(objcnt):
                if oi["o32_flags"] & flag:
                    return oi["o32_vsize"]
            return 0

        # Windows FILETIME (time/time2) -> Unix time_t
        t = ((m.time << 32) | m.time2)
        t = t // 10000000 - 11644473600
        timestamp = u32(t)

        units = [(0, 0)] * STD_EXTRA
        for idx in (EXP, IMP, RES, EXC, SEC, IMD, MSP):
            units[idx] = (e32["e32_unit"][idx][0], e32["e32_unit"][idx][1])
        units[RS4] = (e32["e32_sect14rva"], e32["e32_sect14size"])

        pe_scalars = [
            b"PE\x00\x00",                                   # magic
            0x01a6,                                          # cpu (SH4)
            objcnt,                                          # objcnt
            timestamp,                                       # timestamp
            0, 0,                                            # symtaboff, symcount
            0xe0,                                            # opthdrsize
            (e32["e32_imageflags"] | IMAGE_FILE_RELOCS_STRIPPED) & 0xFFFF,
            0x10b,                                           # coffmagic
            6, 1,                                            # link maj/min
            sum_vsize(IMAGE_SCN_CNT_CODE),                   # codesize
            sum_vsize(IMAGE_SCN_CNT_INITIALIZED_DATA),       # initdsize
            sum_vsize(IMAGE_SCN_CNT_UNINITIALIZED_DATA),     # uninitdsize
            e32["e32_entryrva"],                             # entryrva
            first_vsize(IMAGE_SCN_CNT_CODE),                 # codebase
            first_vsize(IMAGE_SCN_CNT_INITIALIZED_DATA),     # database
            e32["e32_vbase"],                                # vbase
            0x1000, 0x200,                                   # objalign, filealign
            4, 0, 0, 0,                                      # os/user maj/min
            e32["e32_subsysmajor"], e32["e32_subsysminor"],
            0,                                               # res1
            e32["e32_vsize"],                                # vsize
            0,                                               # hdrsize (patched)
            0,                                               # filechksum
            e32["e32_subsys"], 0,                            # subsys, dllflags
            e32["e32_stackmax"], 0x1000,                     # stackmax, stackinit
            0x100000, 0x1000,                                # heapmax, heapinit
            0, STD_EXTRA,                                    # res2, hdrextra
        ]
        pe_fmt = "<4sHHIIIHHHBBIIIIIIIIIHHHHHHIIIIHHIIIIII"
        pe = struct.pack(pe_fmt, *pe_scalars)
        for rva, sz in units:
            pe += struct.pack("<II", rva, sz)
        assert len(pe) == E32EXE_SIZE
        r.write(pe)

        # Section headers (o32_obj, 40 bytes each)
        o32hdroff = [0] * objcnt
        for j in range(objcnt):
            o32hdroff[j] = r.tell()
            oj = o32[j]
            if (e32["e32_unit"][RES][0] == oj["o32_rva"]
                    and e32["e32_unit"][RES][1] == oj["o32_vsize"]):
                segtype = SEG_RSRC
            elif (e32["e32_unit"][EXC][0] == oj["o32_rva"]
                    and e32["e32_unit"][EXC][1] == oj["o32_vsize"]):
                segtype = SEG_PDATA
            elif oj["o32_flags"] & IMAGE_SCN_CNT_CODE:
                segtype = SEG_TEXT
            elif oj["o32_flags"] & IMAGE_SCN_CNT_INITIALIZED_DATA:
                segtype = SEG_DATA
            elif oj["o32_flags"] & IMAGE_SCN_CNT_UNINITIALIZED_DATA:
                segtype = SEG_PDATA
            else:
                segtype = SEG_OTHER
            name8 = self._seg_name(segtype).ljust(8, b"\x00")[:8]
            obj = struct.pack(
                "<8s8I", name8,
                oj["o32_vsize"], oj["o32_rva"],
                0, 0, 0, 0, 0,                       # psize/dataptr/.../temp3
                u32(oj["o32_flags"] & ~0x2000))      # flags w/o COMPRESSED bit
            r.write(obj)

        size = r.tell()
        if size % 0x200:
            r.seek(0x200 - (size % 0x200), io.SEEK_CUR)
        headersize = r.tell()

        # Section data
        for j in range(objcnt):
            oj = o32[j]
            dataofslist = r.tell()
            datalenlist = oj["o32_psize"]
            if not self.virtual_seek(oj["o32_dataptr"]):
                raise ImageError("Unable to read block file")
            buf = self.virtual_read(oj["o32_psize"])
            if oj["o32_flags"] & SECTION_COMPRESSED:
                out = self.codec.decompress(buf, oj["o32_vsize"])
                r.write(out)
                datalenlist = len(out)
            else:
                r.write(buf)
            size = r.tell()
            if size % 0x200:
                r.seek(0x200 - (size % 0x200), io.SEEK_CUR)
            r.seek(o32hdroff[j] + 16, io.SEEK_SET)
            r.write(struct.pack("<I", datalenlist))
            r.write(struct.pack("<I", dataofslist))
            r.seek(0, io.SEEK_END)

        filesize = r.tell()
        r.seek(newe32off + 0x54, io.SEEK_SET)
        r.write(struct.pack("<I", headersize))
        r.seek(filesize, io.SEEK_SET)
        return r.getvalue()

    def _extract_module(self, i: int, m: ModuleHdr) -> int:
        if (m.attr & FILEATTR_COMPRESS_MODULE) and not self.codec.available:
            self._skip(m.name, "compressed; codec unavailable")
            return 1
        print("Extracting %s ..." % m.name)
        try:
            data = self._build_module_bytes(i, m)
        except (CodecUnavailable, RuntimeError) as e:
            print("Error in CEDecompress(): %s" % e)
            self._skip(m.name, "compressed section; codec unavailable")
            return 1
        except ImageError as e:
            print(str(e))
            return 0
        os.makedirs(self.binfile, exist_ok=True)
        with open(os.path.join(self.binfile, m.name), "wb") as fh:
            fh.write(data)
        return 1

    def _update_module(self, i: int, m: ModuleHdr, names: list[str]) -> int:
        print("Updating module %s..." % m.name)
        fname = names[1] if len(names) >= 2 else names[0]
        try:
            with open(fname, "rb") as fh:
                indata = fh.read()
        except OSError:
            print("Error - can't open update module %s" % fname)
            return 0

        if not self.virtual_seek(m.e32offset):
            print("Unable to locate e32offset")
            return 0
        e32 = parse_e32_rom(self.virtual_read(E32ROM_SIZE))
        objcnt = e32["e32_objcnt"]
        if not self.virtual_seek(m.o32offset):
            print("Unable to locate o32offset")
            return 0
        o32 = [parse_o32_rom(self.virtual_read(O32ROM_SIZE))
               for _ in range(objcnt)]

        try:
            num_sections, sections = parse_pe_sections(indata)
        except ValueError as e:
            print("Error - can't map and load update module %s (%s)" % (fname, e))
            return 0
        if num_sections != objcnt:
            print("Error - can't update the module")
            print("Incoming module %s has %d sections, existing module in rom "
                  "has %d sections." % (fname, num_sections, objcnt))
            return 0

        headersize = (DOSHDR_SIZE + len(DOSCODE) + E32EXE_SIZE
                      + O32OBJ_SIZE * objcnt)
        if headersize % 0x200:
            headersize += 0x200 - (headersize % 0x200)

        pos = headersize
        for s in range(num_sections):
            sec_size = sections[s]["SizeOfRawData"]
            section_data = indata[pos:pos + sec_size]
            pos += sec_size

            if o32[s]["o32_flags"] & SECTION_COMPRESSED:
                try:
                    comp = self.codec.compress(section_data, sec_size + 20)
                except CodecUnavailable as e:
                    print("Cannot compress section: %s" % e)
                    return 0
                if len(comp) > o32[s]["o32_psize"]:
                    print("Compressing section %s of module %s results in a "
                          "section too big to fit."
                          % (sections[s]["Name"], fname))
                    print("(Compressed size: %10d - existing section size %10d)"
                          % (len(comp), o32[s]["o32_psize"]))
                    return 0
                if len(comp) != o32[s]["o32_psize"]:
                    print("not an exact match in compressed size of updated "
                          "module and the existing module.")
                    print("the crap might hit the fan.")
                section_data = comp

            if not self.virtual_seek(o32[s]["o32_dataptr"]):
                print("Unable to read block file (the .bin file)")
                return 0
            self.virtual_write(section_data)

        print("Successfully updated module %s" % fname)
        return 1

    # --- files ------------------------------------------------------------ #
    def read_files(self, command: int, names: list[str]) -> int:
        base = (self.romhdraddr + ROMHDR_SIZE
                + MODULEHDR_SIZE * self.romhdr.nummods)
        if not self.virtual_seek(base):
            print("Unable to read block file")
            return 0
        self.files = [FileHdr.unpack(self.virtual_read(FILEHDR_SIZE))
                      for _ in range(self.romhdr.numfiles)]

        didupdate = 0
        for i, fh in enumerate(self.files):
            name = self._read_name(fh.fileaddr)
            if name is None:
                print("Unable to read block file")
                return 0
            fh.name = name

            if command == COMMAND_LIST:
                self._print_file(fh)
            elif command == COMMAND_EXTRACT:
                if names and fh.name not in names:
                    continue
                if not self._extract_file(fh):
                    return 0
            elif command == COMMAND_UPDATE:
                if names[0] != fh.name:
                    continue
                didupdate = 1
                if not self._update_file(i, fh, names):
                    return 0

        if not didupdate and command == COMMAND_UPDATE:
            print("Unable to find %s on BIN" % names[0])
            return 0
        return 1

    def _print_file(self, fh: FileHdr):
        flags = (("C" if fh.attr & FILEATTR_COMPRESS else "_")
                 + ("H" if fh.attr & FILEATTR_HIDDEN else "_")
                 + ("R" if fh.attr & FILEATTR_READONLY else "_")
                 + ("S" if fh.attr & FILEATTR_SYSTEM else "_"))
        print("%s%10d%10d%22s (ROM 0x%08x)" % (flags, fh.size, fh.size2,
                                               fh.name, fh.offset))

    def _extract_file(self, fh: FileHdr) -> int:
        if (fh.attr & FILEATTR_COMPRESS) and not self.codec.available:
            self._skip(fh.name, "compressed; codec unavailable")
            return 1
        print("Extracting %s ..." % fh.name)
        os.makedirs(self.binfile, exist_ok=True)
        out_path = os.path.join(self.binfile, fh.name)
        if not self.virtual_seek(fh.offset):
            print("Unable to read block file")
            return 0
        buf = self.virtual_read(fh.size2)
        if fh.attr & FILEATTR_COMPRESS:
            try:
                data = self.codec.decompress(buf, fh.size)
            except (CodecUnavailable, RuntimeError) as e:
                print("Error in CEDecompress(): %s" % e)
                self._skip(fh.name, "decompress failed")
                return 1
        else:
            data = buf
        with open(out_path, "wb") as out:
            out.write(data)
        return 1

    def _update_file(self, i: int, fh: FileHdr, names: list[str]) -> int:
        fname = names[1] if len(names) >= 2 else names[0]
        print("Updating %s ..." % fh.name)
        if not self.virtual_seek(fh.offset):
            print("Unable to read block file")
            return 0
        try:
            with open(fname, "rb") as r:
                buf = r.read()
        except OSError:
            print("Unable to open file %s" % fname)
            return 0
        st_size = len(buf)
        attr = fh.attr

        if not (attr & FILEATTR_COMPRESS):
            if st_size > fh.size2:
                attr |= FILEATTR_COMPRESS
            else:
                self.virtual_write(buf)
                if not self.update_file_size(i, st_size, st_size, attr):
                    return 0

        if attr & FILEATTR_COMPRESS:
            try:
                out = self.codec.compress(buf, st_size + 20)
            except CodecUnavailable as e:
                print("Error in CECompress(): %s" % e)
                return 0
            except RuntimeError:
                print("Error in CECompress()")
                return 1
            if len(out) > fh.size2:
                print("The size of the updated file must be less-then or equal "
                      "to the size of the old file.")
                print("This feature might be introduced in a newer version.")
                print("Updated file (COMPRESSED): %d bytes   Old file "
                      "(COMPRESSED): %d bytes" % (len(out), fh.size2))
            else:
                self.virtual_write(out)
                if not self.update_file_size(i, st_size, len(out), attr):
                    return 0
        return 1


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #
def usage(prog: str):
    print("Bysin %s by bysin (Python port & GUI by SLFL; orig. "
          "guicide/ryebrye/DogP)\n" % VERSION)
    print("%s <filename> <command>" % prog)
    print("Valid commands are:")
    print("  info                          - image header / block / entry summary")
    print("  list                          - lists contents")
    print("  extract [files...]            - extract all/specified files")
    print("  update outfile [infile]       - update specified files")
    print("  updateModule outfile [infile] - update specified module")


def main(argv: list[str]) -> int:
    prog = "dumpnavi.py"
    # Pull optional flags out without disturbing the positional, argv-style API.
    dll_path = None
    args = []
    it = iter(argv)
    for a in it:
        if a == "--codec-dll":
            dll_path = next(it, None)
        else:
            args.append(a)

    if len(args) < 2:
        usage(prog)
        return 0

    binpath = args[0]
    cmd = args[1].lower()
    if cmd == "list":
        command = COMMAND_LIST
    elif cmd == "info":
        command = 0   # handled specially below
    elif cmd == "extract":
        command = COMMAND_EXTRACT
    elif cmd == "update" and len(args) >= 3:
        command = COMMAND_UPDATE
    elif cmd == "updatemodule" and len(args) >= 3:
        command = COMMAND_UPDATE_MODULE
    else:
        usage(prog)
        return 0

    extra = args[2:]

    bin_dir = os.path.dirname(os.path.abspath(binpath))
    codec = make_codec(dll_path, extra_dirs=[bin_dir])
    needs_codec = command in (COMMAND_EXTRACT, COMMAND_UPDATE,
                              COMMAND_UPDATE_MODULE)
    if needs_codec and not codec.available:
        print("Note: LZX codec unavailable - COMPRESSED entries (marked 'C' in "
              "`list`) will be skipped; everything else is processed normally.")
        print("      Reason: %s" % (codec.load_error or "no DLL"))

    writable = command in (COMMAND_UPDATE, COMMAND_UPDATE_MODULE)
    try:
        img = BinImage(binpath, codec=codec, writable=writable)
    except OSError:
        print("Unable to open BIN file")
        return 0

    with img:
        try:
            img.load()
        except ImageError as e:
            print(str(e))
            return 0
        if cmd == "info":
            print(img.format_info())
            checked, bad = img.verify_checksums()
            print("Block checksums: %d checked, %d mismatched%s"
                  % (checked, bad, "  (OK)" if bad == 0 else "  (!)"))
            return 0
        img.read_modules(command, extra)
        img.read_files(command, extra)
        if img.skipped and command != COMMAND_LIST:
            print("\n%d entr%s skipped (compressed, no usable codec). Run under "
                  "32-bit Python with CECompressv4.dll to handle them."
                  % (len(img.skipped), "y was" if len(img.skipped) == 1
                     else "ies were"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
