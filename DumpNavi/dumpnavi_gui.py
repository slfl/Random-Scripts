#!/usr/bin/env python3
"""
dumpnavi_gui.py - GUI front-end for the DumpNAVI Python port (localized).

Open a .bin, browse modules/files, preview/extract on demand, replace entries
(staged in memory) and Save. Compressed entries need the LZX codec (32-bit
Python directly, or 64-bit Python with the auto 32-bit helper - see README).

Languages live in language.ini (one [Section] per language). The current
language is shown as a small badge in the bottom-right corner; click it to pick
another. Add a new [Section] to language.ini and it appears automatically.

Python port & GUI: SLFL.  Original: bysin / guicide / ryebrye / DogP.
"""

from __future__ import annotations

import configparser
import os
import traceback

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import dumpnavi as D
from ce_lzx import make_codec, CodecUnavailable

HERE = os.path.dirname(os.path.abspath(__file__))
LANG_FILE = os.path.join(HERE, "language.ini")
SETTINGS_FILE = os.path.join(HERE, "gui_settings.ini")
APP_TITLE = "DumpNAVI - by SLFL"

# Built-in English fallback so the GUI still works if language.ini is missing.
FALLBACK_EN = {
    "code": "EN", "open": "Open .bin", "info": "Info", "preview": "Preview",
    "extract_selected": "Extract selected", "extract_all": "Extract all",
    "replace": "Replace...", "check_fit": "Check fit...", "save": "Save",
    "save_as": "Save As...", "filter": "Filter:", "col_name": "Name",
    "col_kind": "Type", "col_flags": "Flags", "col_size": "Size",
    "col_stored": "Stored", "col_rom": "ROM addr", "kind_module": "module",
    "kind_file": "file", "codec_ready": "[LZX codec: ready]",
    "codec_missing": "[LZX codec: NOT loaded - needs 32-bit Python + CECompressv4.dll]",
    "status_start": "Open a .bin file to begin.{0}",
    "status_loaded": "Loaded {0}: {1} modules, {2} files.{3}",
    "open_fail": "Open failed", "preview_select": "Select an entry to preview.",
    "codec_needed": "Codec needed",
    "codec_needed_msg": "{0} is compressed and needs the LZX codec.\n\n{1}",
    "preview_fail": "Preview failed",
    "preview_title": "Preview: {0}  ({1} bytes)",
    "preview_info": "{0}  |  {1}  |  flags {2}  |  {3} bytes",
    "extract_none": "Nothing selected to extract.",
    "extract_to": "Extract to folder", "extracting": "Extracting... {0}/{1}",
    "extracted": "Extracted {0} file(s) to {1}.",
    "extracted_skipped": "Skipped {0} compressed (codec needed).",
    "some_skipped": "Some entries skipped",
    "some_skipped_msg": "{0} compressed entries were skipped (codec not loaded).",
    "replace_select": "Select exactly one entry to replace.",
    "replace": "Replace...", "replace_title": "Replace",
    "replace_one": "Please select exactly ONE entry.",
    "choose_replacement": "Choose replacement for {0}",
    "cant_read": "Can't read file:\n{0}",
    "replaced": "Replaced {0}. {1}  (not saved yet)",
    "replace_failed": "Replace failed",
    "replace_failed_status": "Replace failed for {0}.",
    "checkfit_select": "Select exactly one entry to check.",
    "checkfit": "Check fit",
    "checkfit_module": "Modules can't be resized; sections are replaced in place.",
    "checkfit_choose": "Check fit of file against {0}",
    "checkfit_codec_msg": "{0} is {1} bytes (stored COMPRESSED) but the codec isn't available:\n\n{2}",
    "checkfit_fits": "FITS - {0} bytes free in the slot.",
    "checkfit_nofit": "DOES NOT FIT - overflows by {0} bytes.",
    "checkfit_msg": "Candidate: {0}\nLogical size: {1} bytes\nWould be stored: {2} bytes ({3})\nSlot capacity: {4} bytes\n\n{5}",
    "checkfit_title": "Check fit: {0}",
    "checkfit_status": "Check fit {0}: stored {1} / slot {2} -> {3}",
    "word_fits": "fits", "word_toobig": "too big",
    "indicator_module": "{0} - module (XIP): fixed size, can't be resized.",
    "indicator_file": "{0} - slot {1} bytes. Stored {2}, {3} free. A bigger file is OK if it compresses to <= {1} bytes.",
    "info_open_first": "Open a .bin first.", "info_fail": "Info failed",
    "info_title": "Image info: {0}",
    "info_checksums": "\nBlock checksums: {0} checked, {1} mismatched {2}",
    "ok_suffix": "(OK)", "bad_suffix": "(!)", "save_nothing": "Nothing to save.",
    "save_confirm": "Write changes back to:\n{0} ?\n\n(A .bak backup will be made.)",
    "save_failed": "Save failed", "saved": "Saved to {0}",
    "save_as_title": "Save .bin as", "saved_copy": "Saved a copy to {0}",
    "unsaved": "Unsaved changes",
    "unsaved_msg": "There are unsaved changes. Discard them?",
    "lang_changed": "Language: {0}",
}


class I18N:
    """Loads language.ini; one section per language. Falls back to English."""

    def __init__(self, path: str):
        self.path = path
        self.cp = configparser.ConfigParser(interpolation=None)
        self.languages: list[str] = []
        self.name = "English"
        self._cur: dict[str, str] = {}
        self.reload()

    def reload(self):
        self.cp = configparser.ConfigParser(interpolation=None)
        try:
            with open(self.path, encoding="utf-8") as f:
                self.cp.read_file(f)
        except OSError:
            pass
        self.languages = list(self.cp.sections()) or ["English"]

    def set_language(self, name: str):
        self.name = name
        self._cur = dict(self.cp.items(name)) if self.cp.has_section(name) else {}

    def t(self, key: str, *args) -> str:
        s = self._cur.get(key)
        if s is None:
            s = FALLBACK_EN.get(key, key)
        s = s.replace("\\n", "\n")
        if args:
            try:
                s = s.format(*args)
            except (IndexError, KeyError, ValueError):
                pass
        return s

    def code(self) -> str:
        c = self._cur.get("code")
        if c:
            return c
        if self.name == "English":
            return FALLBACK_EN["code"]
        return self.name[:2].upper()


def load_setting(key: str, default: str = "") -> str:
    cp = configparser.ConfigParser(interpolation=None)
    try:
        cp.read(SETTINGS_FILE, encoding="utf-8")
        return cp.get("general", key, fallback=default)
    except (OSError, configparser.Error):
        return default


def save_setting(key: str, value: str):
    cp = configparser.ConfigParser(interpolation=None)
    try:
        cp.read(SETTINGS_FILE, encoding="utf-8")
    except (OSError, configparser.Error):
        pass
    if not cp.has_section("general"):
        cp.add_section("general")
    cp.set("general", key, value)
    try:
        with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
            cp.write(f)
    except OSError:
        pass


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
        self.entries: list[D.Entry] = []
        self.codec = make_codec()
        self.iid_to_entry: dict[str, D.Entry] = {}
        self._sort_state = {}

        self.i18n = I18N(LANG_FILE)
        start_lang = load_setting("language", "")
        if start_lang not in self.i18n.languages:
            start_lang = ("English" if "English" in self.i18n.languages
                          else self.i18n.languages[0])
        self.i18n.set_language(start_lang)
        self.t = self.i18n.t

        self._build_ui()
        self._retranslate()
        self._set_status(self.t("status_start", self._codec_note()))

    # ------------------------------------------------------------------ UI --
    def _build_ui(self):
        self.root.geometry("920x600")
        self.root.minsize(740, 420)
        self.buttons: dict[str, ttk.Button] = {}

        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(side=tk.TOP, fill=tk.X)
        specs = [("open", self.open_bin), ("info", self.show_info),
                 ("preview", self.preview),
                 ("extract_selected", self.extract_selected),
                 ("extract_all", self.extract_all),
                 ("replace", self.replace_selected),
                 ("check_fit", self.check_fit)]
        for key, cmd in specs:
            b = ttk.Button(bar, text=key, command=cmd)
            b.pack(side=tk.LEFT, padx=(0 if key == "open" else 6, 0))
            self.buttons[key] = b
        b = ttk.Button(bar, text="save", command=self.save)
        b.pack(side=tk.LEFT, padx=(18, 0))
        self.buttons["save"] = b
        b = ttk.Button(bar, text="save_as", command=self.save_as)
        b.pack(side=tk.LEFT, padx=(6, 0))
        self.buttons["save_as"] = b

        filt = ttk.Frame(self.root, padding=(8, 0))
        filt.pack(side=tk.TOP, fill=tk.X)
        self.filter_lbl = ttk.Label(filt, text="filter")
        self.filter_lbl.pack(side=tk.LEFT)
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", lambda *_: self._refill())
        ttk.Entry(filt, textvariable=self.filter_var).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 0))

        mid = ttk.Frame(self.root, padding=(8, 6))
        mid.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        self.cols = ("name", "kind", "flags", "size", "csize", "rom")
        self.col_keys = {"name": "col_name", "kind": "col_kind",
                         "flags": "col_flags", "size": "col_size",
                         "csize": "col_stored", "rom": "col_rom"}
        widths = {"name": 330, "kind": 70, "flags": 60, "size": 90,
                  "csize": 90, "rom": 110}
        self.tree = ttk.Treeview(mid, columns=self.cols, show="headings",
                                 selectmode="extended")
        for c in self.cols:
            self.tree.heading(c, text=c,
                              command=lambda cc=c: self._sort_by(cc))
            anchor = tk.W if c in ("name", "kind", "flags") else tk.E
            self.tree.column(c, width=widths[c], anchor=anchor,
                             stretch=(c == "name"))
        vsb = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.LEFT, fill=tk.Y)
        self.tree.bind("<Double-1>", lambda e: self.preview())
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._on_select())

        bottom = ttk.Frame(self.root)
        bottom.pack(side=tk.BOTTOM, fill=tk.X)
        self.status = tk.StringVar()
        ttk.Label(bottom, textvariable=self.status, relief=tk.SUNKEN,
                  anchor=tk.W, padding=(8, 3)).pack(side=tk.LEFT, fill=tk.X,
                                                    expand=True)
        self.lang_lbl = ttk.Label(bottom, text="EN", relief=tk.SUNKEN,
                                  anchor=tk.CENTER, padding=(12, 3),
                                  cursor="hand2")
        self.lang_lbl.pack(side=tk.RIGHT, fill=tk.Y)
        self.lang_lbl.bind("<Button-1>", self._popup_language_menu)

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------------------------------------------------- language --
    def _popup_language_menu(self, event=None):
        self.i18n.reload()                       # pick up edits / new languages
        menu = tk.Menu(self.root, tearoff=0)
        for name in self.i18n.languages:
            mark = "* " if name == self.i18n.name else "   "
            menu.add_command(label=mark + name,
                             command=lambda n=name: self.set_language(n))
        x = self.lang_lbl.winfo_rootx()
        y = self.lang_lbl.winfo_rooty()
        try:
            menu.tk_popup(x, y - 4 - 22 * len(self.i18n.languages))
        except tk.TclError:
            menu.tk_popup(x, y)
        finally:
            menu.grab_release()

    def set_language(self, name: str):
        self.i18n.set_language(name)
        save_setting("language", name)
        self._retranslate()
        self._set_status(self.t("lang_changed", name))

    def _retranslate(self):
        for key, w in self.buttons.items():
            w.config(text=self.t(key))
        self.filter_lbl.config(text=self.t("filter"))
        for c in self.cols:
            self.tree.heading(c, text=self.t(self.col_keys[c]),
                              command=lambda cc=c: self._sort_by(cc))
        self.lang_lbl.config(text=self.i18n.code())
        if self.img:
            self._refill()
        self._update_title()

    # ------------------------------------------------------------- status --
    def _codec_note(self) -> str:
        return " " + (self.t("codec_ready") if self.codec.available
                      else self.t("codec_missing"))

    def _set_status(self, msg: str):
        self.status.set(msg)

    def _update_title(self):
        name = os.path.basename(self.img.path) if self.img else "-"
        star = " *" if (self.img and self.img.dirty) else ""
        self.root.title("%s - %s%s" % (APP_TITLE, name, star))

    # --------------------------------------------------------------- open --
    def open_bin(self):
        if not self._confirm_discard():
            return
        path = filedialog.askopenfilename(
            title=self.t("open"),
            filetypes=[("BIN images", "*.bin"), ("All", "*.*")])
        if not path:
            return
        try:
            data = open(path, "rb").read()
            img = D.BinImage(path, codec=self.codec, data=data)
            img.load()
            entries = img.scan_entries()
        except (D.ImageError, OSError) as e:
            messagebox.showerror(self.t("open_fail"), str(e))
            return
        except Exception as e:  # noqa: BLE001
            messagebox.showerror(self.t("open_fail"),
                                 "%s\n\n%s" % (e, traceback.format_exc()))
            return
        self.img = img
        self.entries = entries
        self._refill()
        self._update_title()
        nmod = sum(1 for e in entries if e.kind == "module")
        self._set_status(self.t("status_loaded", os.path.basename(path),
                                nmod, len(entries) - nmod, self._codec_note()))

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
                e.name, self.t("kind_" + e.kind), e.flags, e.size, csize, rom))
            self.iid_to_entry[iid] = e

    def _sort_by(self, col: str):
        if not self.img:
            return
        rev = self._sort_state.get(col, False)
        idx = self.cols.index(col)

        def key(e):
            return (e.name, e.kind, e.flags, e.size,
                    e.size2 or 0, e.romaddr or 0)[idx]
        self.entries = sorted(self.entries, key=key, reverse=rev)
        self._sort_state[col] = not rev
        self._refill()

    # ------------------------------------------------------ selection util --
    def _selected_entries(self) -> list[D.Entry]:
        return [self.iid_to_entry[i] for i in self.tree.selection()]

    def _on_select(self):
        sel = self._selected_entries()
        if not self.img or len(sel) != 1:
            return
        e = sel[0]
        if e.kind == "module":
            self._set_status(self.t("indicator_module", e.name))
            return
        cap = self.img.slot_capacity(e.index)
        self._set_status(self.t("indicator_file", e.name, cap, e.size2 or 0,
                                cap - (e.size2 or 0)))

    # ----------------------------------------------------------- info/prev --
    def show_info(self):
        if not self.img:
            self._set_status(self.t("info_open_first"))
            return
        try:
            text = self.img.format_info()
            checked, bad = self.img.verify_checksums()
            suffix = self.t("ok_suffix") if bad == 0 else self.t("bad_suffix")
            text += self.t("info_checksums", checked, bad, suffix)
        except Exception as ex:  # noqa: BLE001
            messagebox.showerror(self.t("info_fail"), str(ex))
            return
        self._text_window(self.t("info_title", os.path.basename(self.img.path)),
                          text, "620x360")

    def preview(self):
        sel = self._selected_entries()
        if not self.img or not sel:
            self._set_status(self.t("preview_select"))
            return
        e = sel[0]
        try:
            data = self.img.get_entry_bytes(e)
        except (CodecUnavailable, RuntimeError) as ex:
            messagebox.showwarning(self.t("codec_needed"),
                                   self.t("codec_needed_msg", e.name, ex))
            return
        except Exception as ex:  # noqa: BLE001
            messagebox.showerror(self.t("preview_fail"), str(ex))
            return
        body = (data.decode("utf-8", "replace") if looks_text(data)
                else hexdump(data))
        header = self.t("preview_info", e.name, self.t("kind_" + e.kind),
                        e.flags, len(data))
        self._text_window(self.t("preview_title", e.name, len(data)),
                          body, "760x520", header=header)

    def _text_window(self, title, body, geom, header=None):
        win = tk.Toplevel(self.root)
        win.title(title)
        win.geometry(geom)
        if header:
            ttk.Label(win, text=header, padding=(8, 6)).pack(side=tk.TOP, fill=tk.X)
        txt = tk.Text(win, wrap=tk.NONE, font=("Consolas", 10))
        sb = ttk.Scrollbar(win, orient="vertical", command=txt.yview)
        txt.configure(yscrollcommand=sb.set)
        txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.LEFT, fill=tk.Y)
        txt.insert("1.0", body)
        txt.configure(state=tk.DISABLED)

    # ------------------------------------------------------------- extract --
    def _extract_many(self, entries):
        if not entries:
            self._set_status(self.t("extract_none"))
            return
        outdir = filedialog.askdirectory(title=self.t("extract_to"))
        if not outdir:
            return
        ok = 0
        skipped = 0
        for n, e in enumerate(entries, 1):
            try:
                data = self.img.get_entry_bytes(e)
            except Exception:  # noqa: BLE001
                skipped += 1
                continue
            with open(os.path.join(outdir, e.name), "wb") as fh:
                fh.write(data)
            ok += 1
            if n % 25 == 0:
                self._set_status(self.t("extracting", n, len(entries)))
                self.root.update_idletasks()
        msg = self.t("extracted", ok, outdir)
        if skipped:
            msg += " " + self.t("extracted_skipped", skipped)
        self._set_status(msg)
        if skipped and not self.codec.available:
            messagebox.showinfo(self.t("some_skipped"),
                                self.t("some_skipped_msg", skipped))

    def extract_selected(self):
        if self.img:
            self._extract_many(self._selected_entries())

    def extract_all(self):
        if self.img:
            self._extract_many(list(self.entries))

    # ------------------------------------------------------------- replace --
    def replace_selected(self):
        sel = self._selected_entries()
        if not self.img or not sel:
            self._set_status(self.t("replace_select"))
            return
        if len(sel) != 1:
            messagebox.showinfo(self.t("replace_title"), self.t("replace_one"))
            return
        e = sel[0]
        src = filedialog.askopenfilename(
            title=self.t("choose_replacement", e.name))
        if not src:
            return
        try:
            data = open(src, "rb").read()
        except OSError as ex:
            messagebox.showerror(self.t("replace_title"), self.t("cant_read", ex))
            return
        if e.kind == "file":
            ok, msg = self.img.replace_file(e.index, data)
        else:
            ok, msg = self.img.replace_module(e.index, data)
        if ok:
            self._refresh_entry_row(e)
            self._update_title()
            self._set_status(self.t("replaced", e.name, msg))
        else:
            messagebox.showwarning(self.t("replace_failed"),
                                   "%s:\n%s" % (e.name, msg))
            self._set_status(self.t("replace_failed_status", e.name))

    def _refresh_entry_row(self, e):
        if e.kind == "file":
            fh = self.img.files[e.index]
            e.size, e.size2, e.attr = fh.size, fh.size2, fh.attr
            e.compressed = bool(fh.attr & D.FILEATTR_COMPRESS)
        for iid, ent in self.iid_to_entry.items():
            if ent is e:
                csize = "" if e.size2 is None else str(e.size2)
                rom = "" if e.romaddr is None else "0x%08x" % e.romaddr
                self.tree.item(iid, values=(e.name, self.t("kind_" + e.kind),
                                            e.flags, e.size, csize, rom))
                break

    # ------------------------------------------------------------ checkfit --
    def check_fit(self):
        sel = self._selected_entries()
        if not self.img or len(sel) != 1:
            self._set_status(self.t("checkfit_select"))
            return
        e = sel[0]
        if e.kind == "module":
            messagebox.showinfo(self.t("checkfit"), self.t("checkfit_module"))
            return
        src = filedialog.askopenfilename(title=self.t("checkfit_choose", e.name))
        if not src:
            return
        try:
            data = open(src, "rb").read()
        except OSError as ex:
            messagebox.showerror(self.t("checkfit"), self.t("cant_read", ex))
            return
        info = self.img.file_fit(e.index, data)
        cand = os.path.basename(src)
        if info["error"]:
            messagebox.showwarning(
                self.t("checkfit"),
                self.t("checkfit_codec_msg", cand, info["logical"], info["error"]))
            return
        if info["fits"]:
            verdict = self.t("checkfit_fits", info["free"])
            icon = messagebox.showinfo
            word = self.t("word_fits")
        else:
            verdict = self.t("checkfit_nofit", info["overflow"])
            icon = messagebox.showwarning
            word = self.t("word_toobig")
        icon(self.t("checkfit_title", e.name),
             self.t("checkfit_msg", cand, info["logical"], info["stored"],
                    info["mode"], info["slot"], verdict))
        self._set_status(self.t("checkfit_status", cand, info["stored"],
                                info["slot"], word))

    # ---------------------------------------------------------------- save --
    def save(self):
        if not self.img:
            return
        if not self.img.dirty:
            self._set_status(self.t("save_nothing"))
            return
        if not messagebox.askyesno(self.t("save"),
                                   self.t("save_confirm", self.img.path)):
            return
        try:
            if os.path.exists(self.img.path):
                bak = self.img.path + ".bak"
                if not os.path.exists(bak):
                    with open(self.img.path, "rb") as s, open(bak, "wb") as d:
                        d.write(s.read())
            self.img.save(self.img.path)
        except OSError as ex:
            messagebox.showerror(self.t("save_failed"), str(ex))
            return
        self._update_title()
        self._set_status(self.t("saved", self.img.path))

    def save_as(self):
        if not self.img:
            return
        path = filedialog.asksaveasfilename(
            title=self.t("save_as_title"), defaultextension=".bin",
            filetypes=[("BIN images", "*.bin"), ("All", "*.*")])
        if not path:
            return
        try:
            self.img.save(path)
        except OSError as ex:
            messagebox.showerror(self.t("save_failed"), str(ex))
            return
        self._set_status(self.t("saved_copy", path))

    # --------------------------------------------------------------- close --
    def _confirm_discard(self) -> bool:
        if self.img and self.img.dirty:
            return messagebox.askyesno(self.t("unsaved"), self.t("unsaved_msg"))
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
