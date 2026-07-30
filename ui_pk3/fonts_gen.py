"""Replace the CoD1 bitmap fonts with a modern typeface (Bahnschrift SemiBold SemiCondensed).

The engine loads fonts/fontImage_<size>.dat (glyph metrics + UV boxes + page names) and
the referenced page images. Every .dat stays byte-identical (metrics/UVs untouched);
only the PAGE IMAGES are redrawn. Pages are emitted at 512x512 (original 256; UVs are
normalized so any resolution works) -> 2x sharper text at 1080p.

CoD1 glyphInfo record (stride 80, RE'd from fontImage_16.dat):
  int height@0; int cellw@4; float top@8; float pitch@12; float xSkip@16;
  int imageWidth@20; int imageHeight@24; float s,t,s2,t2@28; int glyph@44; char name[32]@48
The engine draws each glyph's box quad with its top edge at (baseline - top*scale), so
the ink we place in a box must have its baseline exactly `top` rows below the box top.
Glyphs are rendered at a UNIFORM face size (calibrated on the capital-H cell height)
and baseline-anchored into their windows — no per-glyph stretching.
"""
import os
import struct
from PIL import Image, ImageDraw, ImageFont

SRC_FONTS = r"C:\Users\bitpo\OneDrive\Bureau\codps3\fonts"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fonts")
os.makedirs(OUT, exist_ok=True)

PAGE = 512          # output page resolution (original 256)
SS = PAGE // 256    # metric upscale from original cell space
SIZES = (12, 16, 18, 24, 30, 32)
GS = 80


def make_face(px):
    f = ImageFont.truetype("C:/Windows/Fonts/bahnschrift.ttf", px)
    try:
        f.set_variation_by_name("SemiBold SemiCondensed")
    except Exception:
        pass
    return f


def face_for_cap(px_cap):
    """Face sized so the capital-H ink height ~= px_cap."""
    probe = make_face(100)
    bbox = probe.getbbox("H")
    cap100 = bbox[3] - bbox[1]
    return make_face(max(6, round(100.0 * px_cap / cap100)))


def parse_dat(size):
    """-> {page_basename: [(code, box_px, top_px), ...]} in PAGE pixel space."""
    d = open(os.path.join(SRC_FONTS, f"fontImage_{size}.dat"), "rb").read()
    pages = {}
    for i in range(256):
        off = i * GS
        top = struct.unpack_from("<f", d, off + 8)[0]
        iw, ih = struct.unpack_from("<2i", d, off + 20)
        s, t, s2, t2 = struct.unpack_from("<4f", d, off + 28)
        name = d[off + 48:off + 80].split(b"\0")[0].decode(errors="replace")
        if iw <= 0 or ih <= 0 or s2 <= s or t2 <= t or not name:
            continue
        base = os.path.basename(name).rsplit(".", 1)[0]
        box = (round(s * PAGE), round(t * PAGE), round(s2 * PAGE), round(t2 * PAGE))
        pages.setdefault(base, []).append((i, box, top * SS))
    return pages


def render_cell(face, ch, box_w, box_h, top):
    """Uniform-size render, baseline anchored `top` rows below the window top."""
    W = box_w * 4 + 96
    H = box_h * 4 + 96
    B = H // 2
    tmp = Image.new("L", (W, H), 0)
    try:
        ImageDraw.Draw(tmp).text((W // 2, B), ch, font=face, fill=255, anchor="ms")
    except Exception:
        return None
    ink = tmp.getbbox()
    if not ink:
        return None
    y0 = int(round(B - top))
    cx = (ink[0] + ink[2]) // 2
    x0 = cx - box_w // 2
    return tmp.crop((x0, y0, x0 + box_w, y0 + box_h))


def dxt5_white(img):
    """DXT5-encode a white+alpha RGBA image (solid white color, alpha quantized)."""
    w, h = img.size
    a = img.getchannel("A").load()
    LEVELS = (255, 0, 219, 182, 146, 109, 73, 36)  # a0>a1 interpolation order
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            codes = 0
            for i in range(16):
                px = a[bx + (i % 4), by + (i // 4)]
                code = min(range(8), key=lambda c: abs(LEVELS[c] - px))
                codes |= code << (3 * i)
            out += bytes((255, 0)) + codes.to_bytes(6, "little")
            out += b"\xff\xff\xff\xff\x00\x00\x00\x00"
    return bytes(out)


def write_dds(img, path):
    w, h = img.size
    hdr = bytearray(b"DDS " + b"\0" * 124)
    struct.pack_into("<6I", hdr, 4, 124, 0x00081007, h, w, w * h, 0)
    struct.pack_into("<2I4s", hdr, 76, 32, 0x4, b"DXT5")
    struct.pack_into("<I", hdr, 108, 0x1000)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(dxt5_white(img))


def write_tga(img, path):
    w, h = img.size
    px = img.tobytes()
    out = bytearray(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28))
    for i in range(0, len(px), 4):
        r, g, b, al = px[i], px[i+1], px[i+2], px[i+3]
        out += bytes((b, g, r, al))
    open(path, "wb").write(out)


for size in SIZES:
    pages = parse_dat(size)
    h_cell = None
    for glyphs in pages.values():
        for code, (x0, y0, x1, y1), top in glyphs:
            if code == ord("H"):
                h_cell = y1 - y0
    face = face_for_cap(h_cell if h_cell else size * SS)
    for base, glyphs in pages.items():
        img = Image.new("RGBA", (PAGE, PAGE), (255, 255, 255, 0))
        for code, (x0, y0, x1, y1), top in glyphs:
            ch = bytes([code]).decode("latin1")
            bw, bh = x1 - x0, y1 - y0
            if bw <= 0 or bh <= 0:
                continue
            cell_l = render_cell(face, ch, bw, bh, top)
            if cell_l is None:
                continue
            cell = Image.new("RGBA", (bw, bh), (255, 255, 255, 0))
            cell.putalpha(cell_l)
            img.alpha_composite(cell, (x0, y0))
        write_dds(img, os.path.join(OUT, base + ".dds"))
        write_tga(img, os.path.join(OUT, base + ".tga"))
        print(f"page {base}: {len(glyphs)} glyphs (H cell {h_cell}px)")
print("done")
