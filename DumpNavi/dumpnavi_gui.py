#!/usr/bin/env python3
"""
dumpnavi_gui.py - GUI front-end for the DumpNAVI Python port.

Workflow:
  * Open a .bin  -> its directory (modules + files) is parsed and CACHED.
                    Nothing is decompressed yet.
  * Click an entry / Preview -> only THAT entry's content is read & decompressed
                    on demand.
  * Select one / several / all -> Extract them to a folder.
  * Select one  -> Replace: pick a replacement file; the change is applied to an
                    in-memory working copy (the .bin on disk is untouched).
  * Save        -> all staged changes are written to the .bin in one go.

Run it with the SAME interpreter you use for the CLI. For COMPRESSED entries you
need 32-bit Python + CECompressv4.dll next to the scripts:
    py -3-32 dumpnavi_gui.py

Python port & GUI: SLFL.  Original: bysin / guicide / ryebrye / DogP.
"""

from __future__ import annotations

import os
import sys
import traceback

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import dumpnavi as D
from ce_lzx import make_codec, CodecUnavailable

APP_TITLE = "DumpNAVI - by SLFL"


def hexdump(data: bytes, limit: int = 4096) -> str:
    out = []
    chunk = data[:limit]
    for off in range(0, len(chunk), 16):
        row = chunk[off:off + 16]
        hexs = " ".join("%02x" % b for b in row)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        out.append("%08x  %-47s  %s" % (off, hexs, text))
    if len(data) > limit:
        out.append("... (%d more bytes)" % (len(data) - limit))
    return "\n".join(out)


def looks_text(data: bytes) -> bool:
    sample = data[:1024]
    if not sample:
        return True
    if b"\x00" in sample:
        return False
    printable = sum(1 for b in sample if 9 <= b <= 13 or 32 <= b < 127)
    return printable / len(sample) > 0.85


class DumpNaviGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.img: D.BinImage | None = None
        self.codec = make_codec()
        self.iid_to_entry: dict[str, D.Entry] = {}
        self._build_ui()
        self._update_title()
        self._set_status("Open a .bin file to begin." + self._codec_note())

    # ------------------------------------------------------------------ UI --
    def _build_ui(self):
        self.root.geometry("900x600")
        self.root.minsize(720, 420)

        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(side=tk.TOP, fill=tk.X)
        ttk.Button(bar, text="Open .bin", command=self.open_bin).pack(side=tk.LEFT)
        ttk.Button(bar, text="Preview", command=self.preview).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Button(bar, text="Extract selected", command=self.extract_selected).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Button(bar, text="Extract all", command=self.extract_all).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Button(bar, text="Replace...", command=self.replace_selected).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Button(bar, text="Save", command=self.save).pack(side=tk.LEFT, padx=(18, 0))
        ttk.Button(bar, text="Save As...", command=self.save_as).pack(side=tk.LEFT, padx=(6, 0))

        filt = ttk.Frame(self.root, padding=(8, 0))
        filt.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(filt, text="Filter:").pack(side=tk.LEFT)
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", lambda *_: self._refill())
        ttk.Entry(filt, textvariable=self.filter_var).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 0))

        mid = ttk.Frame(self.root, padding=(8, 6))
        mid.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        cols = ("name", "kind", "flags", "size", "csize", "rom")
        heads = {"name": "Name", "kind": "Type", "flags": "Flags",
                 "size": "Size", "csize": "Stored", "rom": "ROM addr"}
        widths = {"name": 320, "kind": 70, "flags": 60, "size": 90,
                  "csize": 90, "rom": 110}
        self.tree = ttk.Treeview(mid, columns=cols, show="headings",
                                 selectmode="extended")
        for c in cols:
            self.tree.heading(c, text=heads[c],
                              command=lambda cc=c: self._sort_by(cc))
            anchor = tk.W if c in ("name", "kind", "flags") else tk.E
            self.tree.column(c, width=widths[c], anchor=anchor,
                             stretch=(c == "name"))
        vsb = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.LEFT, fill=tk.Y)
        self.tree.bind("<Double-1>", lambda e: self.preview())

        self.status = tk.StringVar()
        ttk.Label(self.root, textvariable=self.status, relief=tk.SUNKEN,
                  anchor=tk.W, padding=(8, 3)).pack(side=tk.BOTTOM, fill=tk.X)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._sort_state = {}

    def _codec_note(self) -> str:
        if self.codec.available:
            return "  [LZX codec: ready]"
        return ("  [LZX codec: NOT loaded - compressed entries need 32-bit "
                "Python + CECompressv4.dll]")

    def _set_status(self, msg: str):
        self.status.set(msg)

    def _update_title(self):
        name = os.path.basename(self.img.path) if self.img else "no file"
        star = " *" if (self.img and self.img.dirty) else ""
        self.root.title("%s - %s%s" % (APP_TITLE, name, star))

    # --------------------------------------------------------------- open --
    def open_bin(self):
        if not self._confirm_discard():
            return
        path = filedialog.askopenfilename(
            title="Open .bin", filetypes=[("BIN images", "*.bin"), ("All", "*.*")])
        if not path:
            return
        try:
            data = open(path, "rb").read()
            img = D.BinImage(path, codec=self.codec, data=data)
            img.load()
            entries = img.scan_entries()
        except (D.ImageError, OSError) as e:
            messagebox.showerror("Open failed", str(e))
            return
        except Exception as e:  # noqa: BLE001
            messagebox.showerror("Open failed", "%s\n\n%s" % (e, traceback.format_exc()))
            return
        self.img = img
        self.entries = entries
        self._refill()
        self._update_title()
        nmod = sum(1 for e in entries if e.kind == "module")
        nfile = len(entries) - nmod
        self._set_status("Loaded %s: %d modules, %d files.%s"
                         % (os.path.basename(path), nmod, nfile, self._codec_note()))

    def _refill(self):
        if not self.img:
            return
        flt = self.filter_var.get().lower().strip()
        self.tree.delete(*self.tree.get_children())
        self.iid_to_entry.clear()
        for e in self.entries:
            if flt and flt not in e.name.lower():
                continue
            csize = "" if e.size2 is None else str(e.size2)
            rom = "" if e.romaddr is None else "0x%08x" % e.romaddr
            iid = self.tree.insert("", tk.END, values=(
                e.name, e.kind, e.flags, e.size, csize, rom))
            self.iid_to_entry[iid] = e

    def _sort_by(self, col: str):
        if not self.img:
            return
        rev = self._sort_state.get(col, False)
        idx = ("name", "kind", "flags", "size", "csize", "rom").index(col)

        def key(e):
            v = (e.name, e.kind, e.flags, e.size,
                 e.size2 or 0, e.romaddr or 0)[idx]
            return v
        self.entries = sorted(self.entries, key=key, reverse=rev)
        self._sort_state[col] = not rev
        self._refill()

    # ------------------------------------------------------ selection util --
    def _selected_entries(self) -> list[D.Entry]:
        return [self.iid_to_entry[i] for i in self.tree.selection()]

    # ------------------------------------------------------------- preview --
    def preview(self):
        sel = self._selected_entries()
        if not self.img or not sel:
            self._set_status("Select an entry to preview.")
            return
        e = sel[0]
        try:
            data = self.img.get_entry_bytes(e)
        except (CodecUnavailable, RuntimeError) as ex:
            messagebox.showwarning("Codec needed",
                                   "%s is compressed and needs the LZX codec.\n\n%s"
                                   % (e.name, ex))
            return
        except Exception as ex:  # noqa: BLE001
            messagebox.showerror("Preview failed", str(ex))
            return

        win = tk.Toplevel(self.root)
        win.title("Preview: %s  (%d bytes)" % (e.name, len(data)))
        win.geometry("760x520")
        info = "%s  |  %s  |  flags %s  |  %d bytes" % (
            e.name, e.kind, e.flags, len(data))
        ttk.Label(win, text=info, padding=(8, 6)).pack(side=tk.TOP, fill=tk.X)
        txt = tk.Text(win, wrap=tk.NONE, font=("Consolas", 10))
        sb = ttk.Scrollbar(win, orient="vertical", command=txt.yview)
        txt.configure(yscrollcommand=sb.set)
        txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.LEFT, fill=tk.Y)
        if looks_text(data):
            try:
                txt.insert("1.0", data.decode("utf-8"))
            except UnicodeDecodeError:
                txt.insert("1.0", data.decode("latin-1"))
        else:
            txt.insert("1.0", hexdump(data))
        txt.configure(state=tk.DISABLED)

    # ------------------------------------------------------------- extract --
    def _extract_many(self, entries: list[D.Entry]):
        if not entries:
            self._set_status("Nothing selected to extract.")
            return
        outdir = filedialog.askdirectory(title="Extract to folder")
        if not outdir:
            return
        ok = 0
        skipped = []
        for n, e in enumerate(entries, 1):
            try:
                data = self.img.get_entry_bytes(e)
            except (CodecUnavailable, RuntimeError):
                skipped.append(e.name)
                continue
            except Exception:  # noqa: BLE001
                skipped.append(e.name)
                continue
            with open(os.path.join(outdir, e.name), "wb") as fh:
                fh.write(data)
            ok += 1
            if n % 25 == 0:
                self._set_status("Extracting... %d/%d" % (n, len(entries)))
                self.root.update_idletasks()
        msg = "Extracted %d file(s) to %s." % (ok, outdir)
        if skipped:
            msg += "  Skipped %d compressed (codec needed)." % len(skipped)
        self._set_status(msg)
        if skipped and not self.codec.available:
            messagebox.showinfo(
                "Some entries skipped",
                "%d compressed entries were skipped because the LZX codec isn't "
                "loaded.\nRun with 32-bit Python + CECompressv4.dll to include "
                "them." % len(skipped))

    def extract_selected(self):
        if not self.img:
            return
        self._extract_many(self._selected_entries())

    def extract_all(self):
        if not self.img:
            return
        self._extract_many(list(self.entries))

    # ------------------------------------------------------------- replace --
    def replace_selected(self):
        sel = self._selected_entries()
        if not self.img or not sel:
            self._set_status("Select exactly one entry to replace.")
            return
        if len(sel) != 1:
            messagebox.showinfo("Replace", "Please select exactly ONE entry.")
            return
        e = sel[0]
        src = filedialog.askopenfilename(
            title="Choose replacement for %s" % e.name)
        if not src:
            return
        try:
            data = open(src, "rb").read()
        except OSError as ex:
            messagebox.showerror("Replace", "Can't read file:\n%s" % ex)
            return
        if e.kind == "file":
            ok, msg = self.img.replace_file(e.index, data)
        else:
            ok, msg = self.img.replace_module(e.index, data)
        if ok:
            self._refresh_entry_row(e)
            self._update_title()
            self._set_status("Replaced %s. %s  (not saved yet)" % (e.name, msg))
        else:
            messagebox.showwarning("Replace failed", "%s:\n%s" % (e.name, msg))
            self._set_status("Replace failed for %s." % e.name)

    def _refresh_entry_row(self, e: D.Entry):
        # pull updated sizes/flags from the underlying header
        if e.kind == "file":
            fh = self.img.files[e.index]
            e.size, e.size2, e.attr = fh.size, fh.size2, fh.attr
            e.compressed = bool(fh.attr & D.FILEATTR_COMPRESS)
        for iid, ent in self.iid_to_entry.items():
            if ent is e:
                csize = "" if e.size2 is None else str(e.size2)
                rom = "" if e.romaddr is None else "0x%08x" % e.romaddr
                self.tree.item(iid, values=(e.name, e.kind, e.flags,
                                            e.size, csize, rom))
                break

    # ---------------------------------------------------------------- save --
    def save(self):
        if not self.img:
            return
        if not self.img.dirty:
            self._set_status("Nothing to save.")
            return
        if not messagebox.askyesno(
                "Save", "Write changes back to:\n%s ?\n\n(A .bak backup will be "
                "made.)" % self.img.path):
            return
        try:
            if os.path.exists(self.img.path):
                bak = self.img.path + ".bak"
                if not os.path.exists(bak):
                    with open(self.img.path, "rb") as s, open(bak, "wb") as d:
                        d.write(s.read())
            self.img.save(self.img.path)
        except OSError as ex:
            messagebox.showerror("Save failed", str(ex))
            return
        self._update_title()
        self._set_status("Saved to %s" % self.img.path)

    def save_as(self):
        if not self.img:
            return
        path = filedialog.asksaveasfilename(
            title="Save .bin as", defaultextension=".bin",
            filetypes=[("BIN images", "*.bin"), ("All", "*.*")])
        if not path:
            return
        try:
            self.img.save(path)
        except OSError as ex:
            messagebox.showerror("Save failed", str(ex))
            return
        self._set_status("Saved a copy to %s" % path)

    # --------------------------------------------------------------- close --
    def _confirm_discard(self) -> bool:
        if self.img and self.img.dirty:
            return messagebox.askyesno(
                "Unsaved changes",
                "There are unsaved changes. Discard them?")
        return True

    def _on_close(self):
        if self._confirm_discard():
            self.root.destroy()


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    DumpNaviGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
