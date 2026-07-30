"""Generate the 1.6X menu textures (uncompressed 32-bit TGA, top-left origin).

All assets are WHITE luminance + alpha masks: the menu tints them at draw time via
forecolor/backcolor (stock fadebox idiom), so hover color swaps keep working.

Outputs into ui/assets/1.6x/ (packed by build_pk3.py).
"""
import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui", "assets", "1.6x")
os.makedirs(OUT, exist_ok=True)


def write_tga(path, w, h, pix):
    """pix: list of (b, g, r, a) rows flattened, top-left origin."""
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(bytes(pix))
    print("wrote", os.path.basename(path), f"{w}x{h}")


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def rounded_alpha(x, y, w, h, r, aa=1.6):
    """1.0 inside a rounded-rect inset by aa, smooth 0 at the edge."""
    cx = min(max(x, r), w - 1 - r)
    cy = min(max(y, r), h - 1 - r)
    d = math.hypot(x - cx, y - cy)
    return smooth((r - d) / aa + 0.5) if r > 0 else 1.0


def gen(path, w, h, fn):
    pix = bytearray()
    for y in range(h):
        for x in range(w):
            lum, a = fn(x, y)
            v = max(0, min(255, int(lum)))
            pix += bytes((v, v, v, max(0, min(255, int(a)))))
    write_tga(os.path.join(OUT, path), w, h, pix)


# panel: rounded-18 rect, vertical sheen (lighter top), near-opaque
def panel(x, y):
    aa = rounded_alpha(x, y, 512, 512, 18)
    lum = 255 - (y / 511.0) * 52          # subtle top->bottom gradient
    edge = smooth((3.0 - min(y, 3)) / 3.0) * 14 if y < 4 else 0
    return lum + edge, 248 * aa


# soft shadow blob: alpha falls off over the outer 56px, blank core matches panel
def soft(x, y):
    dx = max(56 - x, x - (511 - 56), 0)
    dy = max(56 - y, y - (511 - 56), 0)
    d = math.hypot(dx, dy)
    return 255, 210 * smooth(1.0 - d / 56.0)


# row: rounded-7 bar with slight vertical depth
def row(x, y):
    aa = rounded_alpha(x, y, 256, 32, 7)
    lum = 255 - (y / 31.0) * 26
    return lum, 244 * aa


# button: rounded-9, a touch more contrast
def btn(x, y):
    aa = rounded_alpha(x, y, 128, 32, 9)
    lum = 255 - (y / 31.0) * 34
    return lum, 250 * aa


# horizontal divider: soft 2px line fading out at both ends
VPROF = (18, 105, 225, 255, 255, 225, 105, 18)
def hfade(x, y):
    a = smooth(x / 48.0) * smooth((255 - x) / 48.0)
    return 255, VPROF[y] * a


# vertical divider (transpose of hfade)
def vfade(x, y):
    a = smooth(y / 48.0) * smooth((255 - y) / 48.0)
    return 255, VPROF[x] * a


# glow bar: gaussian-ish, for the active tab underline
def glow(x, y):
    ax = smooth(x / 26.0) * smooth((127 - x) / 26.0)
    c = abs(y - 15.5) / 16.0
    ay = math.exp(-c * c * 5.0)
    return 255, 255 * ax * ay


gen("panel512.tga", 512, 512, panel)
gen("soft512.tga", 512, 512, soft)
gen("row256.tga", 256, 32, row)
gen("btn128.tga", 128, 32, btn)
gen("hfade256.tga", 256, 8, hfade)
gen("vfade256.tga", 8, 256, vfade)
gen("glow128.tga", 128, 32, glow)
