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
2. **Info** shows the image header, ROM header, block map and entry/compression
   summary (plus a block-checksum check). Click a row and **Preview** (or
   double-click) — only *that* entry is read and decompressed on demand.
3. Select one / several / all rows → **Extract selected** / **Extract all**.
4. Select one row → **Replace...** → pick the new file. The change is applied to
   an **in-memory working copy** — the `.bin` on disk is *not* touched yet.
   * Selecting a single file shows a **room indicator** in the status bar: the
     slot capacity (max stored bytes), how much is currently used and how much is
     free. Modules show that they can't be resized.
   * **Check fit...** is a dry run: pick a candidate file and it computes the
     stored/compressed size and tells you whether it fits (headroom or overflow)
     — without changing anything.
5. **Save** writes all staged changes back to the `.bin` at once (it makes a
   `.bak` backup first); **Save As...** writes a copy. The `.bin` keeps its
   original size.

## Replacing files — and can a file be made BIGGER?

Short answer: the **logical** size of a file can grow a lot, but the **stored**
(on-disk) bytes must still fit the slot the original occupied. The tool handles
this automatically:

* If the new data fits the slot uncompressed → stored as-is.
* If it's larger than the slot → it is **LZX-compressed**; if the compressed
  form fits the slot, it's stored and the file's *uncompressed* size is updated
  to the new (larger) value. So replacing a 1 MB file with 2 MB of *compressible*
  content works whenever the 2 MB compresses down to ≤ the original slot. The
  device allocates RAM by the uncompressed size and decompresses on load.
* If even the compressed form is bigger than the slot → it's refused, with the
  exact overflow reported.

**Why the `.bin` itself can't simply grow:** these images are flash partitions
laid out as `physfirst..physlast`, immediately followed by RAM (`ulRAMStart`),
with **zero** trailing slack in the file. Modules are XIP (execute-in-place) at
fixed addresses and can't be moved to make room. Enlarging the ROM region would
collide with the memory map / flash partition and is not safe to do blindly — so
the tool deliberately keeps stored data within the original slot (exactly the
limitation the original `Bysin` documented as "must be ≤ the old size"). Use
`info` to see each image's `ROM->RAM gap` and confirm there's no room to grow.

Modules (EXE/DLL) cannot be grown at all (fixed XIP layout); only their sections
can be replaced in place.

There's a filter box (type to narrow the 700+ entries) and click-to-sort
columns. The title bar shows `*` while there are unsaved changes.

### Languages

The current language is shown as a small badge (e.g. **EN** / **RU**) in the
bottom-right corner. Click it to pick another from a small drop-up menu; the
whole UI updates instantly and your choice is remembered (`gui_settings.ini`).

All text lives in **`language.ini`** — one `[Section]` per language. To add a
language, copy the `[English]` block, rename it (e.g. `[Italiano]`), set
`code = IT`, translate the values, and save. It appears in the menu
automatically the next time you open it. Partial translations are fine —
anything you don't translate falls back to English. Keep the `{0}`, `{1}`
placeholders and `\n` line breaks intact. Ships with English and Russian.


## Requirements

* Python 3.8+
* No third-party packages (standard library only).
* For **compressed** entries (marked `C` in `list`): `CECompressv4.dll`
  (shipped here), Microsoft's `compress_lzx` codec.

### ⚠️ Compressed entries and Python bitness

`CECompressv4.dll` is a **32-bit (i386)** DLL and there is no portable
open-source reimplementation of this codec, so the genuine DLL must do the work.
There are two supported ways to run:

**A) 32-bit Python (simplest).** Run everything under 32-bit Python; the DLL
loads in-process. `py -3-32 dumpnavi_gui.py` / `py -3-32 dumpnavi.py ...`.

**B) 64-bit Python with the auto helper (recommended).** You can run the CLI and
GUI under your normal **64-bit** Python. When it detects it can't load the
32-bit DLL in-process, it transparently spawns a tiny **32-bit helper process**
(`ce_worker.py` via `ce_bridge.py`) that loads the real DLL and does the
(de)compression over a pipe — so the result is still byte-for-byte identical.
This needs a 32-bit Python to exist *as the helper* (it does not run your main
program). By default the helper is launched with the `py -3-32` launcher; if
your 32-bit Python is elsewhere, point to it:

```
set DUMPNAVI_PY32=C:\Path\to\python32\python.exe
py dumpnavi_gui.py            # 64-bit main process, 32-bit helper under the hood
```

If neither a matching in-process DLL nor a 32-bit helper is available, the tools
still `list` and handle all **uncompressed** entries; compressed ones are
skipped with a clear notice.

Keep `CECompressv4.dll` next to the scripts (or pass `--codec-dll <path>`).



## Usage

```
python dumpnavi.py <filename.bin> <command> [args...]

Commands:
  info                          - image header / block map / entry summary
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
