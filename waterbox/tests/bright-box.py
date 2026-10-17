#!/usr/bin/env python3
"""How much of a box in a screenshot is bright.

    bright-box.py X0 Y0 X1 Y1 A.tga [B.tga ...]

For each picture, prints its name and the percentage of the box's pixels whose
three colours average more than 200 (of 255). The box runs from (X0, Y0) to
(X1, Y1), counted from the top left corner, and every fourth pixel across and
down is looked at. Exit 2 on a file that is missing or is not an uncompressed
32-bit TGA - which is what chimera-run writes - and 0 otherwise: what share is
too much is the caller's to say."""
import struct
import sys


def read(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        print(f"{path}: {e.strerror}")
        sys.exit(2)
    if len(data) < 18 or data[2] != 2 or data[16] != 32:
        print(f"{path}: not an uncompressed 32-bit TGA")
        sys.exit(2)
    width, height = struct.unpack_from("<HH", data, 12)
    top_first = bool(data[17] & 0x20)
    start = 18 + data[0]
    pixels = data[start:start + width * height * 4]
    if len(pixels) != width * height * 4:
        print(f"{path}: {len(pixels)} bytes of pixels for {width}x{height}")
        sys.exit(2)
    return width, height, top_first, pixels


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 2
    x0, y0, x1, y1 = (int(v) for v in sys.argv[1:5])
    for path in sys.argv[5:]:
        width, height, top_first, pixels = read(path)
        if x1 > width or y1 > height:
            print(f"{path}: the box does not fit in {width}x{height}")
            return 2
        seen = bright = 0
        for y in range(y0, y1, 4):
            row = y if top_first else height - 1 - y
            for x in range(x0, x1, 4):
                i = (row * width + x) * 4
                seen += 1
                if pixels[i] + pixels[i + 1] + pixels[i + 2] > 600:
                    bright += 1
        print(f"{path} {100.0 * bright / seen:.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
