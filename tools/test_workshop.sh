#!/bin/sh
# Host test of the Workshop backend (features/workshop.cpp, tools/test_workshop.cpp): a
# catalog around a real pk3, served from a fresh temp folder by file:// and by
# tools/range_server.py, installed into a scratch game folder next to the test exe.
#     sh tools/test_workshop.sh <file.pk3> <thumbnail.jpg>        (repo root, Git Bash)
# Each run works in a new temp folder (printed), nothing in the repo is touched.
set -e
export PATH=/c/msys64/mingw32/bin:$PATH
PK3="$1"; THUMB="$2"
[ -f "$PK3" ] && [ -f "$THUMB" ] || { echo "usage: $0 <file.pk3> <thumbnail.jpg>"; exit 2; }
OUT=$(mktemp -d "${TMPDIR:-/tmp}/workshop_test.XXXXXX")
SRV="$OUT/srv"; GAME="$OUT/game"
mkdir -p "$SRV" "$GAME"
echo "--- test folder: $OUT"
cp "$PK3" "$SRV/zz_mp_liberation.pk3"
cp "$THUMB" "$SRV/liberation.jpg"
python -I tools/workshop/make_catalog.py "$SRV/workshop.json" --title "1.6X Workshop (local test)" \
    --pk3 "$SRV/zz_mp_liberation.pk3" --id mp_liberation --name Liberation --type map --version 27 \
    --author "1.6X (CoD4 port)" --thumbnail liberation.jpg --description "test item"
SRVW=$(cygpath -m "$SRV")
python -I - "$SRV" "$SRVW" <<'EOF'
import json, sys, copy
srv, srvw = sys.argv[1], sys.argv[2]
cat = json.load(open(srv + "/workshop.json", encoding="utf-8"))
good = cat["items"][0]
# the right file, a wrong hash
bad = copy.deepcopy(cat); bad["items"][0]["sha256"] = "0" * 64
json.dump(bad, open(srv + "/bad_sha.json", "w"))
# an online catalog pointing at the player's disk
fu = copy.deepcopy(cat); fu["items"][0]["url"] = "file:///" + srvw + "/zz_mp_liberation.pk3"
json.dump(fu, open(srv + "/http_fileurl.json", "w"))
# malformed items around one good one
items = []
def variant(**kw):
    it = copy.deepcopy(good); it.update(kw); items.append(it)
variant(id="evil1", file="../evil.pk3")
variant(id="evil2", file="pak0.pk3")
variant(id="evil3", folder="..")
variant(id="evil4", sha256="1234")
variant(id="evil5", url="ftp://example.invalid/x.pk3")
variant(id="bad id")
variant(id="evil6", size=0)
variant(id="good", name="Libération")
variant(id="good", name="duplicate")
open(srv + "/bad_items.json", "w", encoding="utf-8").write(json.dumps({"items": items}))
# a pack: two pk3s and a cvar, then the same with a cvar nothing may accept
import hashlib, os
files = []
for n, size in (("zz_pack_a.pk3", 300000), ("zz_pack_b.pk3", 120000)):
    data = os.urandom(size)
    open(srv + "/" + n, "wb").write(data)
    files.append({"file": n, "size": size, "sha256": hashlib.sha256(data).hexdigest(), "url": n})
pack = {"id": "hdpack", "type": "mod", "name": "Pack", "files": files, "cvars": {"com_hunkMegs": "512"}}
json.dump({"items": [pack]}, open(srv + "/pack.json", "w"))
pack["cvars"] = {"com_hunkMegs": "512;quit"}
json.dump({"items": [pack]}, open(srv + "/pack_badcvar.json", "w"))
EOF
g++ -std=c++17 -O1 -I src tools/test_workshop.cpp src/features/workshop.cpp src/features/pam_install.cpp \
    src/core/logger.cpp -lwininet -ladvapi32 -static -o "$GAME/test_workshop.exe"
python -I tools/range_server.py "$SRV" 8771 &
HTTP=$!
sleep 1
set +e
"$GAME/test_workshop.exe" "file:///$SRVW/" "http://127.0.0.1:8771/"
RC=$?
kill $HTTP 2>/dev/null
echo "--- game folder after the run:"; ls -la "$GAME/main" "$GAME/cod1reloaded/workshop" "$GAME/cod1reloaded/workshop/thumbs"
exit $RC
