"""Extract every map loading screen from the game's pk3s as Discord art assets.

CoD1 ships one levelshots/<map>.dds per map, 1024x1024 DXT1 - exactly the size
Discord recommends for Rich Presence assets, and already square. Custom maps carry
theirs in their own pk3, so rPAM's map packs are picked up by the same sweep.

Output goes to dist/ (gitignored on purpose): the artwork belongs to Activision and
to the community mappers, so it must not end up in a public repo. It is only staged
locally for a manual upload to the Discord developer portal.

Usage: python tools/extract_map_images.py ["C:\\path\\to\\Call of Duty"]
"""
import io
import os
import sys
import zipfile

from PIL import Image

GAME = sys.argv[1] if len(sys.argv) > 1 else \
    r"C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6 - dev"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist",
                   "discord_map_images")
MIN_SIDE = 512          # Discord's minimum for an art asset
OUT_SIDE = 1024         # what Discord recommends

def trim_black_bars(img):
    """The artwork is 16:9 painted into a square texture, so the file carries black
    letterbox bands. Discord shows a square tile, so keeping them would waste ~40% of
    it on black and shrink the map. Detect the bands instead of assuming 16:9, since
    custom maps do not all use the same ratio."""
    g = img.convert("L")
    w, h = g.size
    px = g.load()
    def row_is_black(y):
        step = max(1, w // 64)
        return all(px[x, y] < 16 for x in range(0, w, step))
    def col_is_black(x):
        step = max(1, h // 64)
        return all(px[x, y] < 16 for y in range(0, h, step))
    top, bottom, left, right = 0, h - 1, 0, w - 1
    while top < bottom and row_is_black(top):       top += 1
    while bottom > top and row_is_black(bottom):    bottom -= 1
    while left < right and col_is_black(left):      left += 1
    while right > left and col_is_black(right):     right -= 1
    if right - left < 16 or bottom - top < 16:      # all black: leave it alone
        return img
    return img.crop((left, top, right + 1, bottom + 1))

def square(img):
    """Centre-crop to a square so the tile is filled with map, not with bars."""
    w, h = img.size
    side = min(w, h)
    return img.crop(((w - side) // 2, (h - side) // 2,
                     (w - side) // 2 + side, (h - side) // 2 + side))

def find_pk3s(root):
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith(".pk3"):
                yield os.path.join(dirpath, f)

def main():
    # map name -> (pk3 sort key, pk3 path, entry). CoD1 loads pk3s alphabetically and
    # later ones win, so the alphabetically-last copy is the one seen in game.
    best = {}
    for pk3 in find_pk3s(GAME):
        key = os.path.basename(pk3).lower()
        try:
            with zipfile.ZipFile(pk3) as z:
                names = z.namelist()
        except Exception:
            continue
        for n in names:
            low = n.lower()
            if not low.startswith("levelshots/"):
                continue
            base = os.path.basename(low)
            stem, ext = os.path.splitext(base)
            if not stem.startswith("mp_") or ext not in (".dds", ".jpg", ".jpeg", ".tga"):
                continue
            if stem not in best or key > best[stem][0]:
                best[stem] = (key, pk3, n)

    os.makedirs(OUT, exist_ok=True)
    ok, failed = [], []
    for stem in sorted(best):
        _, pk3, entry = best[stem]
        try:
            with zipfile.ZipFile(pk3) as z:
                raw = z.read(entry)
            img = Image.open(io.BytesIO(raw)).convert("RGB")
            before = img.size
            img = square(trim_black_bars(img))
            img = img.resize((OUT_SIDE, OUT_SIDE), Image.LANCZOS)
            img.save(os.path.join(OUT, stem + ".png"), "PNG", optimize=True)
            ok.append((stem, before, os.path.basename(pk3)))
        except Exception as e:
            failed.append((stem, os.path.basename(pk3), str(e)))

    for stem, size, src in ok:
        print(f"  {stem:24} {size[0]}x{size[1]:<5} <- {src}")
    if failed:
        print("\nECHECS:")
        for stem, src, err in failed:
            print(f"  {stem:24} <- {src}: {err}")

    print(f"\n{len(ok)} images -> {os.path.normpath(OUT)}")
    print("\nLigne pour cod1reloaded.ini (n'y laisse que les maps reellement uploadees) :")
    print("discord_rpc_map_images    = " + " ".join(s for s, _, _ in ok))

if __name__ == "__main__":
    main()
