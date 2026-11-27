#!/usr/bin/env python3
"""make_padbin.py -- makes a controller picture for the Controls window (pad.bin, pad_sms.bin, pad_md3.bin).

    python3 tools/make_padbin.py picture.png|webp WIDTH HEIGHT output.bin

The picture is resized (Lanczos, on premultiplied colors) to WIDTH x HEIGHT and written as two little-endian
ints (WIDTH, HEIGHT) followed by the zlib-compressed premultiplied BGRA pixels, which is what input_make_picture()
in dialogs.c reads. Needs Pillow. Keep the sizes: pad.bin 560x288, pad_sms.bin 900x373, pad_md3.bin 800x470 (the
ratio of the source picture), and the ring places of each pad in padpics[] of dialogs.c go with the picture.
"""
import struct
import sys
import zlib

from PIL import Image


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    src, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    im = Image.open(src).convert("RGBA").convert("RGBa").resize((w, h), Image.LANCZOS)
    raw = bytearray(im.tobytes())                 # R, G, B, a premultiplied
    for i in range(0, len(raw), 4):
        raw[i], raw[i + 2] = raw[i + 2], raw[i]    # -> B, G, R, a
    with open(out, "wb") as f:
        f.write(struct.pack("<ii", w, h) + zlib.compress(bytes(raw), 9))
    print(out, w, h)


if __name__ == "__main__":
    main()
