# DumpNAVI (Python port)

A faithful Python reimplementation of **DumpNAVI** (`Bysin.cpp`, v1.4) — the tool
used to inspect and modify the Windows CE **XIP ROM images** (`B000FF` format)
found on Acura/Honda navigation DVDs (e.g. `09Touch2.bin`, `09Touch.bin`).

It performs the *identical* analysis and file operations as the original C++
program: it walks the same `B000FF` block/virtual-memory layout, parses the same
`ECEC`/ROM/module/file headers, and supports the same four commands — plus a
graphical interface (`dumpnavi_gui.py`).

*Python port & GUI by **SLFL**. Original: bysin / guicide / ryebrye / DogP;
WinCE structs by Willem Jan Hengeveld (itsme).*

## GUI (dumpnavi_gui.py)

A point-and-click front-end built on Tkinter (bundled with the standard Windows
Python installer — nothing extra to install).

```
py -3-32 dumpnavi_gui.py        # 32-bit, so compressed entries work too
```

Workflow:
1. **Open .bin** — the directory (all modules + files) is parsed and **cached**
   instantly. Nothing is decompressed yet.
2. Click a row and **Preview** (or double-click) — only *that* entry is read and
   decompressed on demand (text shown as text, binary as a hex dump).
3. Select one / several / all rows → **Extract selected** / **Extract all**.
4. Select one row → **Replace...** → pick the new file. The change is applied to
   an **in-memory working copy** — the `.bin` on disk is *not* touched yet.
   (A replacement that doesn't fit the original slot is refused with a clear note.)
5. **Save** writes all staged changes back to the `.bin` at once (it makes a
   `.bak` backup first); **Save As...** writes a copy.

There's a filter box (type to narrow the 700+ entries) and click-to-sort
columns. The title bar shows `*` while there are unsaved changes.


## Requirements

* Python 3.8+
* No third-party packages (standard library only).
* For **compressed** entries (marked `C` in `list`): `CECompressv4.dll`
  (shipped here), Microsoft's `compress_lzx` codec.

### ⚠️ Compressed entries need 32-bit Python on Windows

`CECompressv4.dll` is a **32-bit (i386)** DLL, and there is no portable
open-source reimplementation of this codec — even cross-platform CE tools just
run this same DLL's machine code. A 32-bit DLL can only be loaded by a **32-bit
process**, so to decompress/compress entries you must run under **32-bit
Python**. (The original `Bysin.exe` was a 32-bit program, which is why it could
use this DLL.)

If you only need to inspect the image or pull out the **uncompressed** entries,
any Python on any OS works — compressed entries are simply skipped with a notice.

**To handle compressed entries:**
1. Install 32-bit Python from python.org — choose *"Windows installer (32-bit)"*.
   You can keep your existing 64-bit Python; both can coexist.
2. Keep `CECompressv4.dll` in the same folder as `dumpnavi.py` (or pass
   `--codec-dll C:\path\to\CECompressv4.dll`).
3. Run with the 32-bit interpreter via the `py` launcher:
   ```
   py -3-32 dumpnavi.py SMGA2.bin extract
   ```
   (Verify you're 32-bit: `py -3-32 -c "import struct;print(struct.calcsize('P')*8)"` → `32`.)


## Usage

```
python dumpnavi.py <filename.bin> <command> [args...]

Commands:
  list                          - list all modules (EXE/DLL) and files
  extract [names...]            - extract all entries, or only the named ones
  update  <name> [infile]       - replace a FILE entry inside the .bin
  updateModule <name> [infile]  - replace a MODULE (EXE/DLL) inside the .bin

Options:
  --codec-dll <path>            - explicit path to CECompressv4.dll
```

If `infile` is omitted in `update`/`updateModule`, the file named `<name>` in the
current directory is used (same rule as the original).

### Examples

```
python dumpnavi.py 09Touch2.bin list
python dumpnavi.py 09Touch2.bin extract                 # everything -> ./09Touch2/
python dumpnavi.py 09Touch2.bin extract NAVI.EXE
python dumpnavi.py 09Touch2.bin update  config.txt
python dumpnavi.py 09Touch2.bin updateModule NAVI.EXE patched_NAVI.EXE
```

Extracted files are written to a directory named after the `.bin` (the part
before the first `.`), e.g. `09Touch2.bin` -> `./09Touch2/`.

> **Back up your `.bin` first.** `update`/`updateModule` modify the image
> in place (just like the original).

## How it works (for when you extend it)

* `ce_lzx.py` — ctypes wrapper around `CECompressv4.dll` (`CECompress` /
  `CEDecompress`). Swap in a pure-Python codec here later if you want
  cross-platform compression.
* `dumpnavi.py`
  * `BinImage` holds the open file plus the "virtual memory" state and
    implements `virtual_seek / virtual_read / virtual_write / virtual_calc_sum`
    — the engine that maps ROM virtual addresses onto the `B000FF` block list.
  * `read_header / read_ecec / read_romhdr` parse the container.
  * `read_modules` handles `list` / `extract` (rebuilds a real PE/EXE from the
    ROM's `e32`/`o32` headers) and `updateModule`.
  * `read_files` handles `list` / `extract` / `update`.

All on-disk structures are parsed with `struct` formats whose sizes were
verified against the original Win32 layout (`_romhdr`=84, `_blockhdr`=12,
`_modulehdr`=32, `_filehdr`=28, `e32_rom`=108, `e32_exe`=248, `o32_obj`=40).

### Faithful-to-the-original quirks (kept on purpose)

So that output matches the original `Bysin.exe` byte-for-byte:

* `ROMOFFSET` is re-added inside `virtual_seek` on cross-block reads (a no-op for
  the navi image where `ROMOFFSET=0`).
* Segment-name counters (`.text`, `.data`, …) are not reset between modules.
* The PE `codesize`/`database` fields are computed with the original's
  module-indexed access pattern.
* `ROMOFFSET` auto-fallback: if the first ROM-header read fails, it retries with
  `ROMOFFSET = -0x07FCE000` (used by the WinCE system image, e.g. `09Touch.bin`).

### Difference from the original

The original aborted any extract/update command if the DLL was missing. This port
is more forgiving: it loads the codec lazily, performs all **uncompressed**
operations without it, and **skips** only the specific compressed entries that
need decompression (printing a notice and a final count) instead of stopping. So
on 64-bit Python you still get every uncompressed file in one pass.

## Troubleshooting

**`extract` skips lots of files / "LZX codec unavailable"** — you're on 64-bit
Python (or the DLL isn't found). The tool tells you which. See the 32-bit Python
steps above. The message distinguishes the two cases:
* *"...is 32-bit while your Python is 64-bit..."* → use `py -3-32`.
* *"CECompressv4.dll not found..."* → put the DLL next to `dumpnavi.py`.

**`list` shows everything but `extract` only pulls some files** — expected on
64-bit Python: the `_` (uncompressed) entries extract; the `C` (compressed) ones
are skipped until you run under 32-bit Python.

## License

GPLv2, as the original. Python port & GUI: SLFL. Original authors: bysin,
guicide, ryebrye, DogP; WinCE structs by Willem Jan Hengeveld (itsme).
