#!/usr/bin/env python3
"""
xiaomi_sideload.py — official Xiaomi OTA sideload via MiAssistant mode.

Flashes an OFFICIAL, Xiaomi-signed OTA .zip onto a device that is in
MiAssistant / sideload mode. No bootloader unlock, no signature bypass:
the recovery still verifies the package. We only obtain the per-package
"Validate" token from Xiaomi's own OTA server and speak the ADB
sideload-host protocol directly over USB (adb.exe can't inject the token).

Mechanism reimplemented from GautamGreat/xiaomi_adb_sideload.

Requirements:
    pip install pyusb pycryptodome
    + a libusb backend bound to the phone's ADB interface (see notes at bottom).

Usage:
    python xiaomi_sideload.py info                     # just dump device info
    python xiaomi_sideload.py flash firmware.zip       # info -> token -> flash -> reboot
    python xiaomi_sideload.py flash firmware.zip --no-reboot
    python xiaomi_sideload.py flash firmware.zip --format-data   # WIPES userdata
"""

import argparse
import base64
import hashlib
import json
import os
import struct
import sys
import urllib.parse
import urllib.request

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("pyusb not installed:  pip install pyusb")

try:
    from Crypto.Cipher import AES
except ImportError:
    sys.exit("pycryptodome not installed:  pip install pycryptodome")

# ---------------------------------------------------------------- ADB protocol
A_CNXN = 0x4E584E43
A_OPEN = 0x4E45504F
A_OKAY = 0x59414B4F
A_WRTE = 0x45545257
A_CLSE = 0x45534C43

ADB_VERSION = 0x01000001
MAX_DATA = 1024 * 1024
CHUNK = 64 * 1024                       # sideload block size

IFACE_CLASS, IFACE_SUBCLASS, IFACE_PROTO = 0xFF, 0x42, 0x01   # Xiaomi ADB iface


def _backend():
    """Prefer a bundled libusb-1.0 so Windows users need no separate DLL."""
    try:
        import libusb_package
        return libusb_package.get_libusb1_backend()
    except Exception:
        return None   # fall back to pyusb's own DLL search


def _find(**kw):
    return usb.core.find(backend=_backend(), **kw)


def debug_list():
    """Print every device libusb can see, with interface signatures."""
    devs = list(_find(find_all=True)) or []
    if not devs:
        print("libusb sees NO devices at all.\n"
              "-> No libusb backend loaded. Check: pip install libusb-package")
        return
    for d in devs:
        vp = f"{d.idVendor:04x}:{d.idProduct:04x}"
        try:
            sigs, match = [], False
            for cfg in d:                          # descriptor iteration, no open
                for i in cfg:
                    s = (i.bInterfaceClass, i.bInterfaceSubClass,
                         i.bInterfaceProtocol)
                    sigs.append("cls=%#x/sub=%#x/proto=%#x" % s)
                    if s == (IFACE_CLASS, IFACE_SUBCLASS, IFACE_PROTO):
                        match = True
            tag = "  <-- MATCH (Xiaomi ADB)" if match else ""
            print(f"{vp}  {', '.join(sigs)}{tag}")
        except (usb.core.USBError, NotImplementedError) as e:
            print(f"{vp}  (cannot read descriptors: {e})")

# ------------------------------------------------ Xiaomi OTA validation server
# NB: this AES key/IV is a fixed, public transport obfuscation — not a secret.
OTA_KEY = b"miuiotavalided11"
OTA_IV = b"0102030405060708"
OTA_URL = "http://update.miui.com/updates/miotaV3.php"


def pkcs7_pad(b: bytes) -> bytes:
    p = 16 - (len(b) % 16)
    return b + bytes([p]) * p


def pkcs7_unpad(b: bytes) -> bytes:
    return b[: -b[-1]]


def md5_of_file(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


class AdbUsb:
    """Minimal ADB-over-USB speaker, just enough for MiAssistant sideload."""

    def __init__(self, dev, ep_in, ep_out, iface):
        self.dev, self.ep_in, self.ep_out, self.iface = dev, ep_in, ep_out, iface
        self.local_id = 1

    @classmethod
    def open(cls):
        seen_match = False
        for dev in _find(find_all=True):
            try:
                configs = list(dev)          # descriptor iteration, no device open
            except (usb.core.USBError, NotImplementedError):
                continue
            for cfg in configs:
                for intf in cfg:
                    sig = (intf.bInterfaceClass, intf.bInterfaceSubClass,
                           intf.bInterfaceProtocol)
                    if sig != (IFACE_CLASS, IFACE_SUBCLASS, IFACE_PROTO):
                        continue
                    seen_match = True
                    ep_in = ep_out = None
                    for ep in intf:
                        if usb.util.endpoint_type(ep.bmAttributes) != \
                                usb.util.ENDPOINT_TYPE_BULK:
                            continue
                        if usb.util.endpoint_direction(ep.bEndpointAddress) == \
                                usb.util.ENDPOINT_IN:
                            ep_in = ep.bEndpointAddress
                        else:
                            ep_out = ep.bEndpointAddress
                    if ep_in is None or ep_out is None:
                        continue
                    num = intf.bInterfaceNumber
                    try:
                        try:
                            if dev.is_kernel_driver_active(num):
                                dev.detach_kernel_driver(num)
                        except (NotImplementedError, usb.core.USBError):
                            pass
                        usb.util.claim_interface(dev, num)    # this opens the device
                    except (NotImplementedError, usb.core.USBError) as e:
                        raise RuntimeError(
                            "Found the Xiaomi ADB interface, but libusb cannot OPEN "
                            f"it ({e}).\nThe interface still has a non-libusb driver. "
                            "Install libusbK on this interface with Zadig (or use "
                            "WSL2 + usbipd-win), then retry.") from None
                    return cls(dev, ep_in, ep_out, num)
        if seen_match:
            raise RuntimeError(
                "Xiaomi ADB interface matched but no bulk endpoints were found.")
        raise RuntimeError(
            "Xiaomi ADB interface not found. Put the phone into MiAssistant mode "
            "and install a libusb driver (Zadig/libusbK) on its interface.")

    # ---- framing ----
    def _send(self, cmd, arg0, arg1, data=b""):
        if isinstance(data, str):
            data = data.encode()
        check = sum(data) & 0xFFFFFFFF
        hdr = struct.pack("<6I", cmd, arg0, arg1, len(data), check,
                          cmd ^ 0xFFFFFFFF)
        self.dev.write(self.ep_out, hdr, 3000)
        if data:
            self.dev.write(self.ep_out, data, 8000)

    def _read_exact(self, n, timeout):
        buf = b""
        while len(buf) < n:
            buf += bytes(self.dev.read(self.ep_in, n - len(buf), timeout))
        return buf

    def _recv(self, timeout=5000):
        cmd, arg0, arg1, length, _check, _magic = \
            struct.unpack("<6I", self._read_exact(24, timeout))
        data = self._read_exact(length, timeout) if length else b""
        return cmd, arg0, arg1, data

    # ---- high level ----
    def connect(self):
        self._send(A_CNXN, ADB_VERSION, MAX_DATA, b"host::\x00")
        for _ in range(10):
            cmd, _a0, _a1, data = self._recv()
            if cmd == A_CNXN:
                banner = data.split(b"\x00")[0].decode(errors="replace")
                if not banner.startswith("sideload::"):
                    raise RuntimeError(
                        f"Not in MiAssistant sideload mode (banner: {banner!r})")
                return banner
        raise RuntimeError("No CNXN handshake response from device")

    def service(self, cmd_str):
        """One-shot recovery service; returns its text response."""
        self._send(A_OPEN, self.local_id, 0, cmd_str + "\x00")
        resp = b""
        while True:
            cmd, a0, a1, data = self._recv()
            if cmd == A_WRTE:
                resp += data
            elif cmd == A_CLSE:
                try:
                    self._send(A_CLSE, a1, a0)
                except usb.core.USBError:
                    pass
                break
            # ignore OKAY
        return resp.decode(errors="replace").strip()

    def sideload(self, path, validate):
        size = os.path.getsize(path)
        open_str = f"sideload-host:{size}:{CHUNK}:{validate}:0"
        self._send(A_OPEN, self.local_id, 0, open_str + "\x00")
        last_pct = -1
        with open(path, "rb") as f:
            while True:
                cmd, a0, a1, data = self._recv(timeout=120000)
                if cmd == A_OKAY:
                    self._send(A_OKAY, a1, a0)
                    continue
                if cmd == A_CLSE:
                    self._send(A_CLSE, a1, a0)
                    break
                if cmd != A_WRTE:
                    continue
                text = data.decode(errors="replace").strip()
                if len(data) > 8:                       # status/completion msg
                    print(f"\n[device] {text}")
                    self._send(A_OKAY, a1, a0)
                    break
                try:
                    block = int(text)
                except ValueError:
                    self._send(A_OKAY, a1, a0)
                    continue
                offset = block * CHUNK
                if offset >= size:
                    break
                f.seek(offset)
                buf = f.read(min(CHUNK, size - offset))
                self._send(A_WRTE, a1, a0, buf)
                self._send(A_OKAY, a1, a0)
                pct = offset * 100 // size
                if pct != last_pct:
                    print(f"\rFlashing: {pct:3d}%", end="", flush=True)
                    last_pct = pct
        print()


def read_info(adb):
    return {
        "device":   adb.service("getdevice:"),
        "version":  adb.service("getversion:"),
        "sn":       adb.service("getsn:"),
        "codebase": adb.service("getcodebase:"),
        "branch":   adb.service("getbranch:"),
        "language": adb.service("getlanguage:"),
        "region":   adb.service("getregion:"),
        "romzone":  adb.service("getromzone:"),
    }


def build_ota_request(info, pkg_md5):
    try:
        zone = int(info.get("romzone") or 1)
    except ValueError:
        zone = 1
    return {
        "d":  info["device"],
        "v":  info["version"],
        "c":  info["codebase"],
        "b":  info["branch"],
        "sn": info["sn"],
        "r":  info.get("region") or "GL",
        "l":  info.get("language") or "en-US",
        "f":  "1",
        "id": "",
        "options": {"zone": zone},
        "pkg": pkg_md5,
    }


def get_validate(info, ota_path):
    """Ask Xiaomi's OTA server for the per-package Validate token."""
    pkg_md5 = md5_of_file(ota_path)
    payload = json.dumps(build_ota_request(info, pkg_md5)).encode()

    enc = AES.new(OTA_KEY, AES.MODE_CBC, OTA_IV).encrypt(pkcs7_pad(payload))
    q = base64.b64encode(enc).decode()
    body = urllib.parse.urlencode({"q": q, "t": "", "s": "1"}).encode()
    headers = {
        "clientId": "MITUNES",
        "Content-Type": "application/x-www-form-urlencoded",
        "User-Agent": "MiTunes_UserAgent_v3.0",
        "Accept-Encoding": "identity",
        "Connection": "Keep-Alive",
    }
    req = urllib.request.Request(OTA_URL, body, headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read().decode(errors="replace")

    dec = AES.new(OTA_KEY, AES.MODE_CBC, OTA_IV).decrypt(
        base64.b64decode(urllib.parse.unquote(raw)))
    data = json.loads(pkcs7_unpad(dec))

    pkgrom = data.get("PkgRom") or {}
    validate = pkgrom.get("Validate")
    if not validate:
        raise RuntimeError(
            "Server returned no Validate token — the package is probably not "
            "the official OTA for this exact device/version.\nServer said: "
            + json.dumps(data)[:500])
    return validate, pkg_md5


def print_info(info):
    width = max(len(k) for k in info)
    for k, v in info.items():
        print(f"  {k:<{width}} : {v}")


def main():
    ap = argparse.ArgumentParser(description="Official Xiaomi OTA sideload client")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("debug", help="list all USB devices libusb can see")
    sub.add_parser("info", help="connect and print device info")
    f = sub.add_parser("flash", help="get token and sideload an OTA zip")
    f.add_argument("firmware", help="path to official OTA .zip")
    f.add_argument("--no-reboot", action="store_true", help="don't reboot after flashing")
    f.add_argument("--format-data", action="store_true",
                   help="WIPE userdata before flashing (destroys user data!)")

    args = ap.parse_args()

    if args.cmd == "debug":
        debug_list()
        return

    adb = AdbUsb.open()
    banner = adb.connect()
    print(f"Connected: {banner}")

    info = read_info(adb)
    print("Device info:")
    print_info(info)

    if args.cmd == "info":
        return

    if not os.path.isfile(args.firmware):
        sys.exit(f"File not found: {args.firmware}")

    print("\nRequesting Validate token from Xiaomi OTA server...")
    validate, pkg_md5 = get_validate(info, args.firmware)
    print(f"  pkg md5  : {pkg_md5}")
    print(f"  validate : {validate}")

    if args.format_data:
        print("\nFormatting userdata...")
        adb.service("format-data:")

    print(f"\nSideloading {os.path.basename(args.firmware)} ...")
    adb.sideload(args.firmware, validate)
    print("Sideload finished.")

    if not args.no_reboot:
        print("Rebooting device...")
        try:
            adb.service("reboot:")
        except usb.core.USBError:
            pass


if __name__ == "__main__":
    main()
