"""Build zz_cod1x_ui.pk3 from the ui_pk3/ source tree.

Usage: python build_pk3.py            -> writes ../dist/zz_cod1x_ui.pk3
       python build_pk3.py --deploy   -> also copies it into the game Main/ (with .bak)
"""
import glob
import os
import shutil
import sys
import zipfile

SRC = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(SRC, "..", "dist")
PK3 = os.path.join(OUT_DIR, "zz_cod1x_ui.pk3")
GAME_MAIN = r"C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.5\Main"

FILES = [
    "ui_mp/main.menu",
    "ui_mp/menus.txt",
    "ui_mp/cod1x_settings.menu",
    "ui_mp/cod1x_files.menu",
]
# NOTE: the textured skin (ui/assets/1.6x, assets_gen2.py) and the font override
# (fonts/, fonts_gen.py) are NOT shipped — enzo prefers the flat engine-drawn look
# (2026-07-17). The generators stay in the repo if ever revisited.

os.makedirs(OUT_DIR, exist_ok=True)
with zipfile.ZipFile(PK3, "w", zipfile.ZIP_DEFLATED) as z:
    for rel in FILES:
        z.write(os.path.join(SRC, rel), rel)
print("built", PK3)

if "--deploy" in sys.argv:
    dst = os.path.join(GAME_MAIN, "zz_cod1x_ui.pk3")
    if os.path.exists(dst):
        shutil.copy2(dst, dst + ".bak")
    shutil.copy2(PK3, dst)
    print("deployed to", dst)
