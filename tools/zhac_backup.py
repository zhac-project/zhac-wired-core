#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read or write a ZHAC full backup (.zhacbak) on a computer.

  zhac_backup.py info FILE
  zhac_backup.py decrypt FILE OUTDIR    write <partition>.bin into OUTDIR and print the esptool commands that put
                                        them back on a hub over USB (when the web restore cannot be used)
  zhac_backup.py encrypt OUTFILE NAME=PATH... [--fw V] [--host H] [--chip C] [--nwk-fc N] [--created T]

The password comes from ZHAC_BACKUP_PASSWORD, else a prompt; never from the command line (shell history).
Format: main/backup_format.h (change both together). Needs Python 3.8+ and the `cryptography` package.
"""
import getpass
import hashlib
import os
import struct
import sys
import time

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HEADER_LEN = 228
TAG_LEN = 16
KDF_ITER = 100000
MAGIC = b"ZHACBK01"
# partitions.csv, both targets: where each partition starts on the flash.
OFFSETS = {"nvs": 0xD000, "scripts": 0xC20000, "zb_storage": 0xE20000, "zhac_data": 0xE40000}


def _str(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse(b):
    if len(b) < HEADER_LEN or b[:8] != MAGIC or struct.unpack_from("<H", b, 8)[0] != HEADER_LEN:
        raise SystemExit("not a ZHAC full backup")
    n, it = struct.unpack_from("<HI", b, 10)
    if not 1 <= n <= 4:
        raise SystemExit("bad header: %d partitions" % n)
    created, fc = struct.unpack_from("<II", b, 44)
    parts = [(_str(b[148 + 20 * i:164 + 20 * i]), struct.unpack_from("<I", b, 164 + 20 * i)[0]) for i in range(n)]
    return {"kdf_iter": it, "salt": b[16:32], "nonce": b[32:44], "created": created, "nwk_fc": fc,
            "key_check": b[52:68], "chip": _str(b[68:84]), "fw": _str(b[84:116]), "host": _str(b[116:148]),
            "parts": parts}


def pack(h):
    def s(v, n):
        v = v.encode()[:n - 1]
        return v + b"\0" * (n - len(v))
    b = MAGIC + struct.pack("<HHI", HEADER_LEN, len(h["parts"]), h["kdf_iter"]) + h["salt"] + h["nonce"]
    b += struct.pack("<II", h["created"], h["nwk_fc"]) + h["key_check"]
    b += s(h["chip"], 16) + s(h["fw"], 32) + s(h["host"], 32)
    for name, size in h["parts"]:
        b += s(name, 16) + struct.pack("<I", size)
    return b + b"\0" * (HEADER_LEN - len(b))


def key_for(pw, h):
    key = hashlib.pbkdf2_hmac("sha256", pw, h["salt"], h["kdf_iter"], 32)
    return key, hashlib.sha256(b"ZHAC backup key check" + key).digest()[:16]


def password(confirm=False):
    pw = os.environ.get("ZHAC_BACKUP_PASSWORD")
    if pw is None:
        pw = getpass.getpass("Backup password: ")
        if confirm and getpass.getpass("Again: ") != pw:
            raise SystemExit("the passwords differ")
    pw = pw.encode()
    if not 8 <= len(pw) <= 63:
        raise SystemExit("the password must be 8 to 63 bytes")
    return pw


def cmd_info(path):
    with open(path, "rb") as f:
        h = parse(f.read(HEADER_LEN))
    when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(h["created"])) if h["created"] else "clock not set"
    print("hub %s (%s), firmware %s, made %s, NWK frame counter %d" % (h["host"], h["chip"], h["fw"], when, h["nwk_fc"]))
    for name, size in h["parts"]:
        print("  %-10s %8d bytes" % (name, size))


def cmd_decrypt(path, out):
    with open(path, "rb") as f:
        data = f.read()
    h = parse(data)
    total = sum(size for _, size in h["parts"])
    if len(data) != HEADER_LEN + total + TAG_LEN:
        raise SystemExit("the file is cut short or has extra bytes")
    key, check = key_for(password(), h)
    if check != h["key_check"]:
        raise SystemExit("wrong password")
    try:
        plain = AESGCM(key).decrypt(h["nonce"], data[HEADER_LEN:], data[:HEADER_LEN])
    except InvalidTag:
        raise SystemExit("the file is damaged")
    os.makedirs(out, exist_ok=True)
    off, args = 0, []
    for name, size in h["parts"]:
        p = os.path.join(out, name + ".bin")
        with open(p, "wb") as f:
            f.write(plain[off:off + size])
        off += size
        if name in OFFSETS:
            args.append("0x%X %s" % (OFFSETS[name], p))
    print("wrote %d partitions to %s" % (len(h["parts"]), out))
    print("over USB, with the old hub off for good (the network keys are in these files -- keep them private):")
    print("  esptool --port PORT write-flash " + " ".join(args))


def cmd_encrypt(out, args):
    opts = {"fw": "", "host": "zhac", "chip": "", "nwk-fc": "0", "created": str(int(time.time()))}
    parts = []
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("--") and a[2:] in opts and i + 1 < len(args):
            opts[a[2:]] = args[i + 1]
            i += 2
            continue
        name, _, p = a.partition("=")
        with open(p, "rb") as f:
            parts.append((name, f.read()))
        i += 1
    if not 1 <= len(parts) <= 4 or any(len(d) == 0 or len(d) % 4096 for _, d in parts):
        raise SystemExit("1 to 4 partitions, each a non-empty multiple of 4096 bytes")
    h = {"kdf_iter": KDF_ITER, "salt": os.urandom(16), "nonce": os.urandom(12), "created": int(opts["created"]),
         "nwk_fc": int(opts["nwk-fc"]), "key_check": b"", "chip": opts["chip"], "fw": opts["fw"],
         "host": opts["host"], "parts": [(n, len(d)) for n, d in parts]}
    key, h["key_check"] = key_for(password(confirm=True), h)
    hdr = pack(h)
    with open(out, "wb") as f:
        f.write(hdr + AESGCM(key).encrypt(h["nonce"], b"".join(d for _, d in parts), hdr))


def main(argv):
    if len(argv) >= 2 and argv[0] == "info":
        return cmd_info(argv[1])
    if len(argv) == 3 and argv[0] == "decrypt":
        return cmd_decrypt(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "encrypt":
        return cmd_encrypt(argv[1], argv[2:])
    sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
