"""COD 1.6X menu skin v2 — full-color pre-rendered assets (no engine tinting).

Every asset is drawn at 2x display size with Pillow (real gaussian shadows, layered
gradients, glows), then LANCZOS-resampled to power-of-two textures (the engine
resamples NPOT badly). The menu draws them with neutral colors (1 1 1 1); hover
states are dedicated images toggled via a transparent overlay item.

Panels bake: drop shadow (+20px display margin), rounded body, gradient, border,
top highlight, orange left accent + glow, lighter header zone and header divider.
"""
import struct
from PIL import Image, ImageDraw, ImageFilter

import os
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui", "assets", "1.6x")
os.makedirs(OUT, exist_ok=True)

S = 2  # supersample factor vs display pixels

ORANGE      = (255, 122, 26)
ORANGE_DEEP = (232, 101, 10)


def save_tga(img, name):
    """Uncompressed 32-bit TGA, top-left origin (descriptor 0x28)."""
    img = img.convert("RGBA")
    w, h = img.size
    px = img.tobytes()  # RGBA
    out = bytearray(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28))
    for i in range(0, len(px), 4):
        r, g, b, a = px[i], px[i+1], px[i+2], px[i+3]
        out += bytes((b, g, r, a))
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(out)
    print(f"wrote {name} {w}x{h}")


def vgrad(w, h, top, bot):
    """Vertical linear gradient RGBA image."""
    img = Image.new("RGBA", (w, h))
    p = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(4))
        for x in range(w):
            p[x, y] = c
    return img


def rounded_mask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], radius=r, fill=255)
    return m


def hfade_alpha(img, fade_px):
    """Fade an image's alpha to 0 at the left/right ends."""
    w, h = img.size
    p = img.load()
    for x in range(w):
        f = min(1.0, x / fade_px, (w - 1 - x) / fade_px)
        f = f * f * (3 - 2 * f)
        if f >= 1.0:
            continue
        for y in range(h):
            r, g, b, a = p[x, y]
            p[x, y] = (r, g, b, int(a * f))
    return img


def make_panel(disp_w, disp_h, header_h, pot_w, pot_h, name):
    m = 20 * S                       # baked shadow margin (20 display px)
    pw, ph = disp_w * S, disp_h * S
    W, H = pw + 2 * m, ph + 2 * m
    r = 10 * S

    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    mask = rounded_mask(pw, ph, r)

    # drop shadow: blurred silhouette, slightly down
    sil = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    black = Image.new("RGBA", (pw, ph), (0, 0, 0, 165))
    sil.paste(black, (m, m + 5 * S), mask)
    img = Image.alpha_composite(img, sil.filter(ImageFilter.GaussianBlur(9 * S)))

    # body: vertical gradient, near-opaque
    body = vgrad(pw, ph, (26, 31, 41, 249), (13, 16, 22, 249))

    # header zone: slightly lifted
    hdr = vgrad(pw, header_h * S, (255, 255, 255, 10), (255, 255, 255, 0))
    body.alpha_composite(hdr, (0, 0))

    # top inner highlight
    body.alpha_composite(vgrad(pw, 3 * S, (255, 255, 255, 26), (255, 255, 255, 0)), (0, 0))

    # orange left accent + glow (between the corner radii)
    bar_h = ph - 2 * r
    glow = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    ImageDraw.Draw(glow).rectangle([0, r, 5 * S, r + bar_h], fill=ORANGE + (110,))
    body.alpha_composite(glow.filter(ImageFilter.GaussianBlur(5 * S)))
    bar = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    ImageDraw.Draw(bar).rectangle([0, r, 3 * S - 1, r + bar_h], fill=ORANGE + (235,))
    body.alpha_composite(bar)

    # header divider (fading ends)
    div = Image.new("RGBA", (pw - 24 * S, S), (255, 255, 255, 26))
    div = hfade_alpha(div, 30 * S)
    body.alpha_composite(div, (12 * S, header_h * S))

    # clip to rounded shape + 1px border
    clipped = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    clipped.paste(body, (0, 0), mask)
    ImageDraw.Draw(clipped).rounded_rectangle(
        [0, 0, pw - 1, ph - 1], radius=r, outline=(255, 255, 255, 22), width=S)

    img.alpha_composite(clipped, (m, m))
    save_tga(img.resize((pot_w, pot_h), Image.LANCZOS), name)


def make_bar(disp_w, disp_h, r_disp, pot_w, pot_h, name, hover):
    """Row / slot bar. hover=True adds orange left edge + lift."""
    w, h = disp_w * S, disp_h * S
    r = r_disp * S
    mask = rounded_mask(w, h, r)
    if hover:
        body = vgrad(w, h, (47, 56, 73, 235), (33, 40, 53, 235))
        body.alpha_composite(vgrad(w, 2 * S, (255, 255, 255, 30), (255, 255, 255, 0)), (0, 0))
        # orange bloom on the left
        bloom = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(bloom).rectangle([0, 0, 5 * S, h], fill=ORANGE + (120,))
        body.alpha_composite(bloom.filter(ImageFilter.GaussianBlur(4 * S)))
        edge = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(edge).rectangle([0, 0, 3 * S - 1, h], fill=ORANGE + (255,))
        body.alpha_composite(edge)
    else:
        body = vgrad(w, h, (30, 36, 47, 216), (21, 26, 34, 216))
        body.alpha_composite(vgrad(w, 2 * S, (255, 255, 255, 15), (255, 255, 255, 0)), (0, 0))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(body, (0, 0), mask)
    save_tga(out.resize((pot_w, pot_h), Image.LANCZOS), name)


def make_btn(name, hover):
    w, h = 120 * S, 24 * S
    r = 4 * S
    mask = rounded_mask(w, h, r)
    if hover:
        body = vgrad(w, h, ORANGE + (255,), ORANGE_DEEP + (255,))
        body.alpha_composite(vgrad(w, 3 * S, (255, 255, 255, 70), (255, 255, 255, 0)), (0, 0))
    else:
        body = vgrad(w, h, (38, 45, 58, 240), (27, 33, 44, 240))
        body.alpha_composite(vgrad(w, 2 * S, (255, 255, 255, 22), (255, 255, 255, 0)), (0, 0))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(body, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle(
        [0, 0, w - 1, h - 1], radius=r,
        outline=(255, 255, 255, 30) if not hover else (255, 200, 150, 60), width=S)
    save_tga(out.resize((256, 64), Image.LANCZOS), name)


def make_line(name, vertical=False):
    if vertical:
        img = Image.new("RGBA", (S, 440 * S), (255, 255, 255, 26))
        img = img.transpose(Image.TRANSPOSE)
        img = hfade_alpha(img, 30 * S)
        img = img.transpose(Image.TRANSPOSE)
        img = img.resize((8, 512), Image.LANCZOS)
    else:
        img = Image.new("RGBA", (872, S), (255, 255, 255, 26))
        img = hfade_alpha(img, 30 * S)
        img = img.resize((1024, 4), Image.LANCZOS)
    save_tga(img, name)


def make_glow(name):
    w, h = 78 * S, 10 * S
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(img).rectangle([0, h // 2 - S, w, h // 2 + S], fill=ORANGE + (255,))
    img = img.filter(ImageFilter.GaussianBlur(2.5 * S))
    img = hfade_alpha(img, 14 * S)
    save_tga(img.resize((256, 16), Image.LANCZOS), name)


# panels: settings 460x248 (header 44), files 520x328 (header 44)
make_panel(460, 248, 44, 1024, 512, "panel_settings.tga")
make_panel(520, 328, 44, 1024, 512, "panel_files.tga")
# settings rows 423x22 r4 / files slots 240x18 r3
make_bar(423, 22, 4, 1024, 64, "row.tga", hover=False)
make_bar(423, 22, 4, 1024, 64, "row_hover.tga", hover=True)
make_bar(240, 18, 3, 512, 32, "slot.tga", hover=False)
make_bar(240, 18, 3, 512, 32, "slot_hover.tga", hover=True)
make_btn("btn.tga", hover=False)
make_btn("btn_hover.tga", hover=True)
make_line("line.tga")
make_line("vline.tga", vertical=True)
make_glow("glow_tab.tga")
