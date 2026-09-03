#!/usr/bin/env python3
# How much of a screenshot is not black. A core that draws through the GPU
# bridge and one that draws nothing both write a PNG; only this tells them
# apart, and telling them apart is the whole point of the leg.
#
# Usage: lit-pixels.py <png> <minimum lit pixels>
# Prints a one-line summary; exit 1 if the picture is (nearly) black.
import sys

path, want = sys.argv[1], int(sys.argv[2])
try:
    from PIL import Image
except ImportError:
    print("Pillow not installed; picture not checked")
    sys.exit(0)

im = Image.open(path).convert("RGB")
w, h = im.size
lit = sum(1 for r, g, b in im.getdata() if r or g or b)
print(f"{lit} of {w * h} pixels lit")
sys.exit(0 if lit >= want else 1)
