"""Generate ui_mp/joinserver.menu (server browser) with the 1.6 / 1.5 list selector.

The vanilla menu is taken from the game's paka.pk3 - the newest of the three copies
(pak0, pak8, paka), which is the one the engine actually loads - and patched here so
the edit is reproducible instead of a hand-mangled 35 KB blob. The generated file IS
committed, because CI has no game install to read paka.pk3 from.

Usage: python gen_joinserver_menu.py [path\\to\\paka.pk3]
"""
import os
import re
import sys
import zipfile

SRC = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(SRC, "ui_mp", "joinserver.menu")
DEFAULT_PAK = r"C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6 - dev\Main\paka.pk3"

# The top-left panel (y 5..130) and the bottom button row are both full in vanilla.
# refreshdate is RIGHT-aligned, so narrowing it from the left keeps its text exactly
# where it was and frees x 10..115 on the same row for the selector.
OLD_DATE_RECT = "rect 10 113 265 18"
NEW_DATE_RECT = "rect 120 113 155 18"

# Two items on the same rect, each visible for one value of cod1x_masterlist - the
# cvarTest/showCVar idiom vanilla already uses for createFavorite in this very file.
# The action only sets the cvar: the mod re-pokes protocol+master and issues the
# re-query itself, because a refresh fired from here would run in the same frame and
# still carry the previous ecosystem.
BUTTON = '''

// ---- COD1.6X: which master list to browse -------------------------------------
// cod1x_masterlist  0 = 1.6 (protocol 10, our master)
//                   1 = 1.5 (protocol 6, codmaster.activision.com)
itemDef {
	name		cod1xList16
	text		"1.6 SERVERS"
	type		ITEM_TYPE_BUTTON
	textfont	UI_FONT_NORMAL
	textscale	GLOBAL_TEXTSCALE1
	textstyle	UI_BUTTON_TEXT_STYLE
	style		UI_BUTTON_STYLE
	border		UI_BUTTON_BORDER
	bordercolor	UI_BUTTON_BORDER_COLOR
	rect		10 113 105 15
	textalign	1
	textalignx	51
	textaligny	11
	backcolor	UI_BUTTON_BACK_COLOR
	forecolor	UI_BUTTON_TEXT_COLOR
	visible		1
	cvarTest	"cod1x_masterlist"
	showCVar	{ "0" }
	action		{ play "mouse_click" ; setcvar cod1x_masterlist "1" }
	mouseEnter	{ setitemcolor cod1xList16 backcolor UI_BUTTON_BACK_COLOR_HOVER ; play "mouse_over" }
	mouseExit	{ setitemcolor cod1xList16 backcolor UI_BUTTON_BACK_COLOR }
}

itemDef {
	name		cod1xList15
	text		"1.5 SERVERS"
	type		ITEM_TYPE_BUTTON
	textfont	UI_FONT_NORMAL
	textscale	GLOBAL_TEXTSCALE1
	textstyle	UI_BUTTON_TEXT_STYLE
	style		UI_BUTTON_STYLE
	border		UI_BUTTON_BORDER
	bordercolor	UI_BUTTON_BORDER_COLOR
	rect		10 113 105 15
	textalign	1
	textalignx	51
	textaligny	11
	backcolor	UI_BUTTON_BACK_COLOR
	forecolor	UI_BUTTON_TEXT_COLOR
	visible		1
	cvarTest	"cod1x_masterlist"
	showCVar	{ "1" }
	action		{ play "mouse_click" ; setcvar cod1x_masterlist "0" }
	mouseEnter	{ setitemcolor cod1xList15 backcolor UI_BUTTON_BACK_COLOR_HOVER ; play "mouse_over" }
	mouseExit	{ setitemcolor cod1xList15 backcolor UI_BUTTON_BACK_COLOR }
}
'''


def main():
    pak = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PAK
    if not os.path.exists(pak):
        sys.exit(f"paka.pk3 introuvable: {pak}")

    with zipfile.ZipFile(pak) as z:
        text = z.read("ui_mp/joinserver.menu").decode("latin-1")

    if OLD_DATE_RECT not in text:
        sys.exit(f"refreshdate rect introuvable ({OLD_DATE_RECT}) - le menu vanilla a change")
    text = text.replace(OLD_DATE_RECT, NEW_DATE_RECT, 1)

    # insert right after the refreshdate itemDef closes
    i = text.index("name refreshdate")
    j = text.index("}", text.index("decoration", i))
    text = text[: j + 1] + BUTTON + text[j + 1 :]

    # the two new items must not collide with an existing name
    for name in ("cod1xList16", "cod1xList15"):
        if text.count(f"name\t\t{name}") != 1:
            sys.exit(f"insertion ratee pour {name}")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="latin-1", newline="\n") as f:
        f.write(text)
    print(f"OK -> {OUT} ({len(text)} octets)")


if __name__ == "__main__":
    main()
