#!/usr/bin/env python3
"""Builds controllerdb.bin (the pad layouts the program starts a DirectInput pad with) from SDL's gamecontrollerdb.txt.

    python3 tools/make_controllerdb.py gamecontrollerdb.txt

Source: https://github.com/mdqinc/SDL_GameControllerDB (the zlib license of SDL: Copyright (C) 1997-2025 Sam Lantinga).
Only the Windows lines are kept, and of those only what the program uses: the USB vendor and product id (from the GUID) and the buttons a, b, x, y,
back, start, leftshoulder, rightshoulder, lefttrigger and righttrigger. One line each, "vvvv:pppp|a=b0,b=b1,...", compressed like genres.bin
(4 bytes of length, little endian, then zlib)."""
import re, struct, sys, zlib

KEEP = ("a", "b", "x", "y", "back", "start", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger")

def main(src, dst):
    seen, out = set(), []
    for line in open(src, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#") or "platform:Windows" not in line:
            continue
        f = line.split(",")
        g = f[0].lower()
        if len(g) != 32 or not re.fullmatch(r"[0-9a-f]{32}", g):
            continue                                    # "xinput" and the like
        vid = g[10:12] + g[8:10]                        # bytes 4-5, little endian
        pid = g[18:20] + g[16:18]                       # bytes 8-9
        if g[12:16] != "0000" or g[20:24] != "0000":
            continue                                    # not the usual vendor/product form
        key = vid.upper() + ":" + pid.upper()
        if key in seen:
            continue
        m = {}
        for item in f[2:]:
            if ":" in item:
                k, v = item.split(":", 1)
                if k in KEEP and re.fullmatch(r"[+-]?[ab]\d+~?", v):
                    m[k] = v
        if not m:
            continue
        seen.add(key)
        out.append(key + "|" + ",".join(k + "=" + m[k] for k in KEEP if k in m))
    out.sort()
    data = "\n".join(out).encode("ascii") + b"\n"
    open(dst, "wb").write(struct.pack("<I", len(data)) + zlib.compress(data, 9))
    print(len(out), "pads,", len(data), "bytes,", len(open(dst, "rb").read()), "compressed")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "controllerdb.bin")
