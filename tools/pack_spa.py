#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pack www-spa's dist/ into one blob the firmware embeds (spa_serve.cpp).

The web UI then ships inside the app image: an OTA update replaces firmware
and UI together, and there is no SPIFFS partition to keep in step.

Format, little-endian:
    b"ZSPA" u32 version(=1) u32 count
    count x { u16 path_len, path (utf-8, starts with "/"), u32 offset, u32 size }
    data: each file gzip-compressed, `offset` counted from the start of data
A missing dist/ yields an empty pack (count 0); the firmware then answers
every page with a "web UI not built" message instead of failing to build.

    python3 tools/pack_spa.py <dist-dir> <out-file>
    python3 tools/pack_spa.py --self-test
"""
import gzip
import os
import struct
import sys
import tempfile

MAGIC = b"ZSPA"


def pack(dist, out):
    files = []
    if os.path.isfile(os.path.join(dist, "index.html")):
        for root, _, names in os.walk(dist):
            for name in sorted(names):
                full = os.path.join(root, name)
                rel = "/" + os.path.relpath(full, dist).replace(os.sep, "/")
                with open(full, "rb") as f:
                    # mtime=0: the same dist gives a byte-identical pack
                    files.append((rel, gzip.compress(f.read(), 9, mtime=0)))
    files.sort()
    header = MAGIC + struct.pack("<II", 1, len(files))
    index, data = b"", b""
    for rel, body in files:
        p = rel.encode()
        index += struct.pack("<H", len(p)) + p + struct.pack("<II", len(data), len(body))
        data += body
    with open(out, "wb") as f:
        f.write(header + index + data)
    return len(files)


def unpack(blob):
    assert blob[:4] == MAGIC
    _, count = struct.unpack_from("<II", blob, 4)
    pos, entries = 12, []
    for _ in range(count):
        (plen,) = struct.unpack_from("<H", blob, pos)
        path = blob[pos + 2:pos + 2 + plen].decode()
        off, size = struct.unpack_from("<II", blob, pos + 2 + plen)
        entries.append((path, off, size))
        pos += 2 + plen + 8
    return {p: gzip.decompress(blob[pos + o:pos + o + s]) for p, o, s in entries}


def self_test():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "dist", "assets"))
        files = {"index.html": b"<html>hi</html>", "assets/app-1.js": b"x" * 5000}
        for rel, body in files.items():
            with open(os.path.join(d, "dist", rel), "wb") as f:
                f.write(body)
        out = os.path.join(d, "spa.pack")
        assert pack(os.path.join(d, "dist"), out) == 2
        with open(out, "rb") as f:
            got = unpack(f.read())
        assert got == {"/index.html": files["index.html"], "/assets/app-1.js": files["assets/app-1.js"]}
        assert pack(os.path.join(d, "missing"), out) == 0
        with open(out, "rb") as f:
            assert unpack(f.read()) == {}
    print("pack_spa: self-test passed")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    elif len(sys.argv) == 3:
        n = pack(sys.argv[1], sys.argv[2])
        print(f"pack_spa: {n} file(s) -> {sys.argv[2]}" if n else
              f"pack_spa: WARNING {sys.argv[1]}/index.html not found -- empty web UI pack")
    else:
        sys.exit(__doc__)
